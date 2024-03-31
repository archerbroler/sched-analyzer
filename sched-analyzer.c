/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2022 Qais Yousef */
#include <bpf/libbpf.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "parse_argp.h"
#include "parse_kallsyms.h"
#include "perfetto_wrapper.h"

#include "sched-analyzer-events.h"
#include "sched-analyzer.skel.h"

#ifdef DEBUG
#define pr_debug(...)	fprintf(__VA_ARGS__)
#else
#define pr_debug(...)
#endif

#define clamp(val, lo, hi)    ((val) >= (hi) ? (hi) : ((val) <= (lo) ? (lo) : (val)))

static volatile bool exiting = false;

static void sig_handler(int sig)
{
	exiting = true;
}

static bool ignore_pid_comm(pid_t pid, char *comm)
{
	unsigned int i;

	if (!sa_opts.num_pids && !sa_opts.num_comms)
		return false;

	for (i = 0; i < sa_opts.num_pids; i++)
		if (sa_opts.pid[i] == pid)
			return false;

	for (i = 0; i < sa_opts.num_comms; i++)
		if (strstr(comm, sa_opts.comm[i]))
			return false;

	return true;
}

static int handle_rq_pelt_event(void *ctx, void *data, size_t data_sz)
{
	struct rq_pelt_event *e = data;

	if (sa_opts.load_avg_cpu && e->load_avg != -1)
		trace_cpu_load_avg(e->ts, e->cpu, e->load_avg);

	if (sa_opts.runnable_avg_cpu && e->runnable_avg != -1)
		trace_cpu_runnable_avg(e->ts, e->cpu, e->runnable_avg);

	if (e->type == PELT_TYPE_THERMAL){
		if (sa_opts.load_avg_thermal)
			trace_cpu_load_avg_thermal(e->ts, e->cpu, e->load_avg);
	}

	if (e->util_avg != -1) {
		switch (e->type) {
		case PELT_TYPE_CFS:
			if (sa_opts.util_avg_cpu) {
				trace_cpu_util_avg(e->ts, e->cpu, e->util_avg);
				if (e->uclamp_min != -1 && e->uclamp_max != -1) {
					unsigned long uclamped_avg = clamp(e->util_avg,
									 e->uclamp_min,
									 e->uclamp_max);
					trace_cpu_uclamped_avg(e->ts, e->cpu, uclamped_avg);
				}
			}
			break;
		case PELT_TYPE_RT:
			if (sa_opts.util_avg_rt)
				trace_cpu_util_avg_rt(e->ts, e->cpu, e->util_avg);
			break;
		case PELT_TYPE_DL:
			if (sa_opts.util_avg_dl)
				trace_cpu_util_avg_dl(e->ts, e->cpu, e->util_avg);
			break;
		case PELT_TYPE_IRQ:
			if (sa_opts.util_avg_irq)
				trace_cpu_util_avg_irq(e->ts, e->cpu, e->util_avg);
			break;
		default:
			fprintf(stderr, "Unexpected PELT type: %d\n", e->type);
			break;
		}
	}

	if (sa_opts.util_est_cpu && e->util_est_enqueued != -1)
		trace_cpu_util_est_enqueued(e->ts, e->cpu, e->util_est_enqueued);

	return 0;
}

static int handle_task_pelt_event(void *ctx, void *data, size_t data_sz)
{
	struct task_pelt_event *e = data;

	if (ignore_pid_comm(e->pid, e->comm))
		return 0;

	if (sa_opts.load_avg_task && e->load_avg != -1)
		trace_task_load_avg(e->ts, e->comm, e->pid, e->load_avg);

	if (sa_opts.runnable_avg_task && e->runnable_avg != -1)
		trace_task_runnable_avg(e->ts, e->comm, e->pid, e->runnable_avg);

	if (sa_opts.util_avg_task && e->util_avg != -1) {
		trace_task_util_avg(e->ts, e->comm, e->pid, e->util_avg);
		if (e->uclamp_min != -1 && e->uclamp_max != -1) {
			unsigned long uclamped_avg = clamp(e->util_avg,
							 e->uclamp_min,
							 e->uclamp_max);
			trace_task_uclamped_avg(e->ts, e->comm, e->pid, uclamped_avg);
		}
	}

	if (sa_opts.util_est_task && e->util_est_enqueued != -1) {
		trace_task_util_est_enqueued(e->ts, e->comm, e->pid, e->util_est_enqueued);
		trace_task_util_est_ewma(e->ts, e->comm, e->pid, e->util_est_ewma);
	}

	return 0;
}

static int handle_rq_nr_running_event(void *ctx, void *data, size_t data_sz)
{
	struct rq_nr_running_event *e = data;

	if (sa_opts.cpu_nr_running)
		trace_cpu_nr_running(e->ts, e->cpu, e->nr_running);

	return 0;
}

static int handle_sched_switch_event(void *ctx, void *data, size_t data_sz)
{
	struct sched_switch_event *e = data;

	if (ignore_pid_comm(e->pid, e->comm))
		return 0;

	/* Reset load_avg to 0 for !running */
	if (!e->running && sa_opts.util_avg_task)
		trace_task_load_avg(e->ts, e->comm, e->pid, 0);

	/* Reset util_avg to 0 for !running */
	if (!e->running && sa_opts.util_avg_task) {
		trace_task_util_avg(e->ts, e->comm, e->pid, 0);
		trace_task_uclamped_avg(e->ts, e->comm, e->pid, 0);
	}

	/* Reset util_est to 0 for !running */
	if (!e->running && sa_opts.util_est_task) {
		trace_task_util_est_enqueued(e->ts, e->comm, e->pid, 0);
		trace_task_util_est_ewma(e->ts, e->comm, e->pid, 0);
	}

	return 0;
}

static int handle_freq_idle_event(void *ctx, void *data, size_t data_sz)
{
	struct freq_idle_event *e = data;

	if (sa_opts.cpu_idle) {
		trace_cpu_idle(e->ts, e->cpu, e->idle_state);
		if (e->idle_miss)
			trace_cpu_idle_miss(e->ts, e->cpu, e->idle_state, e->idle_miss);
	}

	return 0;
}

static int handle_softirq_event(void *ctx, void *data, size_t data_sz)
{
	struct softirq_event *e = data;
	(void)e;
	return 0;
}

static int handle_lb_event(void *ctx, void *data, size_t data_sz)
{
	struct lb_event *e = data;
	char *phase = "unknown";

	switch (e->phase) {
	case LB_NOHZ_IDLE_BALANCE:
		phase = "_nohz_idle_balance()";
		break;
	case LB_RUN_REBALANCE_DOMAINS:
		phase = "run_rebalance_domains()";
		break;
	case LB_REBALANCE_DOMAINS:
		phase = "rebalance_domains()";
		if (e->entry)
			trace_lb_sd_stats(e->ts, &e->sd_stats);
		break;
	case LB_BALANCE_FAIR:
		phase = "balance_fair()";
		break;
	case LB_PICK_NEXT_TASK_FAIR:
		phase = "pick_next_task_fair()";
		break;
	case LB_NEWIDLE_BALANCE:
		phase = "newidle_balance()";
		break;
	case LB_LOAD_BALANCE:
		phase = "load_balance()";
		break;
	}

	if (e->overloaded != -1)
		trace_lb_overloaded(e->ts, e->overloaded);

	if (e->overutilized != -1)
		trace_lb_overutilized(e->ts, e->overutilized);

	if (e->misfit_task_load != -1)
		trace_lb_misfit(e->ts, e->lb_cpu, e->misfit_task_load);

	if (e->entry)
		trace_lb_entry(e->ts, e->this_cpu, e->lb_cpu, phase);
	else
		trace_lb_exit(e->ts, e->this_cpu, e->lb_cpu);
	return 0;
}

static int handle_ipi_event(void *ctx, void *data, size_t data_sz)
{
	struct ipi_event *e = data;

	trace_ipi_send_cpu(e->ts, e->from_cpu, e->target_cpu,
			   find_kallsyms(e->callsite), e->callsite,
			   find_kallsyms(e->callback), e->callback);

	return 0;
}

#define INIT_EVENT_RB(event)	struct ring_buffer *event##_rb = NULL

#define CREATE_EVENT_RB(event) do {							\
		event##_rb = ring_buffer__new(bpf_map__fd(skel->maps.event##_rb),	\
					      handle_##event##_event, NULL, NULL);	\
		if (!event##_rb) {							\
			err = -1;							\
			fprintf(stderr, "Failed to create " #event " ringbuffer\n");	\
			goto cleanup;							\
		}									\
	} while(0)

#define DESTROY_EVENT_RB(event) do {							\
		ring_buffer__free(event##_rb);						\
	} while(0)

#define POLL_EVENT_RB(event) do {							\
		err = ring_buffer__poll(event##_rb, 1000);				\
		if (err == -EINTR) {							\
			err = 0;							\
			break;								\
		}									\
		if (err < 0) {								\
			fprintf(stderr, "Error polling " #event " ring buffer: %d\n", err); \
			break;								\
		}									\
		pr_debug(stdout, "[" #event "] consumed %d events\n", err);		\
	} while(0)

#define INIT_EVENT_THREAD(event) pthread_t event##_tid; int event##_err = -1

#define CREATE_EVENT_THREAD(event) do {							\
		event##_err = pthread_create(&event##_tid, NULL, event##_thread_fn, NULL);	\
		if (event##_err) {								\
			fprintf(stderr, "Failed to create " #event " thread: %d\n", event##_err); \
			goto cleanup;							\
		}									\
	} while(0)

#define DESTROY_EVENT_THREAD(event) do {						\
		err = event##_err ? 0 : pthread_join(event##_tid, NULL);		\
		if (err)								\
			fprintf(stderr, "Failed to destory " #event " thread: %d\n", err); \
	} while(0)

#define EVENT_THREAD_FN(event)								\
	void *event##_thread_fn(void *data)						\
	{										\
		int err;								\
		INIT_EVENT_RB(event);							\
		CREATE_EVENT_RB(event);							\
		while (!exiting) {							\
			POLL_EVENT_RB(event);						\
			usleep(10000);							\
		}									\
	cleanup:									\
		DESTROY_EVENT_RB(event);						\
		return NULL;								\
	}


/*
 * All events require to access this variable to get access to the ringbuffer.
 * Make it available for all event##_thread_fn.
 */
struct sched_analyzer_bpf *skel;

/*
 * Define a pthread function handler for each event
 */
EVENT_THREAD_FN(rq_pelt)
EVENT_THREAD_FN(task_pelt)
EVENT_THREAD_FN(rq_nr_running)
EVENT_THREAD_FN(sched_switch)
EVENT_THREAD_FN(freq_idle)
EVENT_THREAD_FN(softirq)
EVENT_THREAD_FN(lb)
EVENT_THREAD_FN(ipi)

int main(int argc, char **argv)
{
	INIT_EVENT_THREAD(rq_pelt);
	INIT_EVENT_THREAD(task_pelt);
	INIT_EVENT_THREAD(rq_nr_running);
	INIT_EVENT_THREAD(sched_switch);
	INIT_EVENT_THREAD(freq_idle);
	INIT_EVENT_THREAD(softirq);
	INIT_EVENT_THREAD(lb);
	INIT_EVENT_THREAD(ipi);
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	if (sa_opts.ipi)
		parse_kallsyms();

	init_perfetto();

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	skel = sched_analyzer_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	/* Initialize BPF global variables */
	skel->bss->sa_opts = sa_opts;

	if (!sa_opts.load_avg_cpu && !sa_opts.runnable_avg_cpu && !sa_opts.util_avg_cpu)
		bpf_program__set_autoload(skel->progs.handle_pelt_cfs, false);
	if (!sa_opts.load_avg_task && !sa_opts.runnable_avg_task && !sa_opts.util_avg_task)
		bpf_program__set_autoload(skel->progs.handle_pelt_se, false);
	if (!sa_opts.util_avg_rt)
		bpf_program__set_autoload(skel->progs.handle_pelt_rt, false);
	if (!sa_opts.util_avg_dl)
		bpf_program__set_autoload(skel->progs.handle_pelt_dl, false);
	if (!sa_opts.util_avg_irq)
		bpf_program__set_autoload(skel->progs.handle_pelt_irq, false);
	if (!sa_opts.load_avg_thermal)
		bpf_program__set_autoload(skel->progs.handle_pelt_thermal, false);
	if (!sa_opts.util_est_cpu)
		bpf_program__set_autoload(skel->progs.handle_util_est_cfs, false);
	if (!sa_opts.util_est_task)
		bpf_program__set_autoload(skel->progs.handle_util_est_se, false);
	if (!sa_opts.cpu_nr_running)
		bpf_program__set_autoload(skel->progs.handle_sched_update_nr_running, false);
	if (!sa_opts.cpu_idle) {
		bpf_program__set_autoload(skel->progs.handle_cpu_idle, false);
		bpf_program__set_autoload(skel->progs.handle_cpu_idle_miss, false);
	}
	if (!sa_opts.load_balance) {
		bpf_program__set_autoload(skel->progs.handle_run_rebalance_domains_exit, false);
		bpf_program__set_autoload(skel->progs.handle_run_rebalance_domains_entry, false);
		bpf_program__set_autoload(skel->progs.handle_run_rebalance_domains_exit, false);
		bpf_program__set_autoload(skel->progs.handle_rebalance_domains_entry, false);
		bpf_program__set_autoload(skel->progs.handle_rebalance_domains_exit, false);
		bpf_program__set_autoload(skel->progs.handle_balance_fair_entry, false);
		bpf_program__set_autoload(skel->progs.handle_balance_fair_exit, false);
		bpf_program__set_autoload(skel->progs.handle_pick_next_task_fair_entry, false);
		bpf_program__set_autoload(skel->progs.handle_pick_next_task_fair_exit, false);
		bpf_program__set_autoload(skel->progs.handle_newidle_balance_entry, false);
		bpf_program__set_autoload(skel->progs.handle_newidle_balance_exit, false);
		bpf_program__set_autoload(skel->progs.handle_load_balance_entry, false);
		bpf_program__set_autoload(skel->progs.handle_load_balance_exit, false);
	}
	if (!sa_opts.ipi)
		bpf_program__set_autoload(skel->progs.handle_ipi_send_cpu, false);

	/* Make sure we zero out PELT signals for tasks when they exit */
	if (!sa_opts.load_avg_task && !sa_opts.runnable_avg_task && !sa_opts.util_avg_task && !sa_opts.util_est_task)
		bpf_program__set_autoload(skel->progs.handle_sched_process_free, false);

	/* We can't reliably attach to those yet, so always disable them */
	bpf_program__set_autoload(skel->progs.handle_nohz_idle_balance_entry, false);
	bpf_program__set_autoload(skel->progs.handle_nohz_idle_balance_exit, false);

	/*
	 * Were used for old csv mode, no longer used but keep the traces lying
	 * around but disabled for now.
	 */
	bpf_program__set_autoload(skel->progs.handle_cpu_frequency, false);
	bpf_program__set_autoload(skel->progs.handle_softirq_entry, false);
	bpf_program__set_autoload(skel->progs.handle_softirq_exit, false);

	/*
	 * Was used to zero out pelt signals when task is not running.
	 */
	bpf_program__set_autoload(skel->progs.handle_sched_switch, false);


	err = sched_analyzer_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load and verify BPF skeleton\n");
		goto cleanup;
	}

	err = sched_analyzer_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF skeleton\n");
		goto cleanup;
	}

	CREATE_EVENT_THREAD(rq_pelt);
	CREATE_EVENT_THREAD(task_pelt);
	CREATE_EVENT_THREAD(rq_nr_running);
	CREATE_EVENT_THREAD(sched_switch);
	CREATE_EVENT_THREAD(freq_idle);
	CREATE_EVENT_THREAD(softirq);
	CREATE_EVENT_THREAD(lb);
	CREATE_EVENT_THREAD(ipi);

	printf("Collecting data, CTRL+c to stop\n");

	start_perfetto_trace();

	while (!exiting) {
		sleep(1);
	}

	stop_perfetto_trace();

	printf("\rCollected %s/%s\n", sa_opts.output_path, sa_opts.output);

cleanup:
	DESTROY_EVENT_THREAD(rq_pelt);
	DESTROY_EVENT_THREAD(task_pelt);
	DESTROY_EVENT_THREAD(rq_nr_running);
	DESTROY_EVENT_THREAD(sched_switch);
	DESTROY_EVENT_THREAD(freq_idle);
	DESTROY_EVENT_THREAD(softirq);
	DESTROY_EVENT_THREAD(lb);
	DESTROY_EVENT_THREAD(ipi);
	sched_analyzer_bpf__destroy(skel);
	return err < 0 ? -err : 0;
}

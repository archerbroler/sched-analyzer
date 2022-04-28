/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2022 Qais Yousef */
#ifndef __SCHED_ANALYZER_EVENTS_H__
#define __SCHED_ANALYZER_EVENTS_H__

#define TASK_COMM_LEN	16
#define PELT_TYPE_LEN	4

struct rq_pelt_event {
	unsigned long long ts;
	int cpu;
	char type[PELT_TYPE_LEN];
	unsigned long util_avg;
	unsigned long uclamp_min;
	unsigned long uclamp_max;
};

struct task_pelt_event {
	unsigned long long ts;
	char comm[TASK_COMM_LEN];
	unsigned long util_avg;
	unsigned long uclamp_min;
	unsigned long uclamp_max;
};


char type_cfs[PELT_TYPE_LEN] = "cfs";
char type_rt[PELT_TYPE_LEN] = "rt";

static inline void copy_pelt_type(char *dst, char *src)
{
	int i;
	for (i = 0; i < PELT_TYPE_LEN; i++)
		dst[i] = src[i];
}

#endif /* __SCHED_ANALYZER_EVENTS_H__ */

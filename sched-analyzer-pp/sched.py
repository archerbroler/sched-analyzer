#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 Qais Yousef

import pandas as pd
import settings
import utils


def init_states(trace):

    global df_states
    df_states = utils.get_df_states()
    if df_states.empty:
        return

    df_states.dur = df_states.dur.astype(float) / 1000000
    df_states = utils.convert_ts(df_states)

def init(trace):

    pd.set_option('display.max_columns', None)
    pd.set_option('display.max_rows', None)
    pd.set_option('display.width', settings.fig_width_tui)

    init_states(trace)

def states_summary(plt, threads=[], parent=None):

    if df_states.empty:
        return

    for thread in threads:
        df = df_states[df_states.name.str.contains(thread).fillna(False)]

        for thread in sorted(df.name.unique()):
            if parent:
                df_thread = df[(df.name == thread) & (df.parent == parent)]
            else:
                df_thread = df[df.name == thread]

            for tid in sorted(df_thread.tid.unique()):
                df_tid = df_thread[df_thread.tid == tid]
                df_tid_running = df_tid[df_tid.state == 'Running']

                print()
                print()
                print()
                fmt = "::  {} | {} | {} ::".format(tid, thread, df_tid.parent.unique())
                print("=" * len(fmt))
                print(fmt)
                print("="*settings.fig_width_tui)
                print("-"*settings.fig_width_tui)
                states = sorted(df_tid.state.unique())
                if 'S' in states:
                    states.remove('S')
                data = []
                total = 0
                for state in states:
                    sum = df_tid[df_tid.state == state].dur.sum().round(2)
                    total += sum
                    data.append(sum)

                plt.clf()
                plt.cld()
                plt.simple_bar(states, data, width=settings.fig_width_tui, title="Sum Time in State Exclude Sleeping (ms)")
                plt.show()

                print()
                data = [d * settings.fig_width_tui / total for d in data]

                plt.clf()
                plt.cld()
                plt.simple_bar(states, data, width=settings.fig_width_tui, title="% Time in State Exclude Sleeping (ms)")
                plt.show()

                print()
                cpus = sorted(df_tid_running.cpu.unique())
                labels = ['CPU{}'.format(cpu) for cpu in cpus]
                data = []
                total = 0
                for cpu in cpus:
                    df_cpu = df_tid_running[df_tid_running.cpu == cpu]
                    sum = df_cpu.dur.sum().round(2)
                    total += sum
                    data.append(sum)

                plt.clf()
                plt.cld()
                plt.simple_bar(labels, data, width=settings.fig_width_tui, title="Sum Time Running on CPU (ms)")
                plt.show()

                print()
                data = [d * settings.fig_width_tui / total for d in data]

                plt.clf()
                plt.cld()
                plt.simple_bar(labels, data, width=settings.fig_width_tui, title="% Time Running on CPU (ms)")
                plt.show()

                print()
                print("Time in State (ms):")
                print("-"*settings.fig_width_tui)
                print(df_tid.groupby(['state']) \
                        .dur.describe(percentiles=[.75, .90, .95, .99]).round(2))

                print()
                print("Time Running on CPU (ms):")
                print("-"*settings.fig_width_tui)
                print(df_tid_running.groupby(['cpu']) \
                        .dur.describe(percentiles=[.75, .90, .95, .99]).round(2))

def states_save_csv(prefix):

    df_states.to_csv(prefix + '_sched_states.csv')

def sched_report(plt):

    nr_top = settings.fig_width_tui

    if df_states.empty:
        return

    print()
    print("States Summary (ms):")
    print("-"*settings.fig_width_tui)
    print(df_states.groupby('state').dur.describe(percentiles=[.75, .90, .95, .99]).round(2))

    df_runnable = df_states[(df_states.state == 'R') | (df_states.state == 'R+')]
    df_running = df_states[df_states.state == 'Running']
    df_usleep = df_states[df_states.state == 'D']

    if not df_runnable.empty:
        print()
        print("Top {} Runnable Tasks (ms) - sorted-by max:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_runnable.sort_values(['dur'], ascending=False) \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['max'], ascending=False) \
                .head(nr_top))
        print()
        print("Top {} Runnable Tasks (ms) - sorted-by 90%:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_runnable.sort_values(['dur'], ascending=False) \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['90%'], ascending=False) \
                .head(nr_top))

    if not df_running.empty:
        print()
        print("Top {} Running Tasks (ms) - sorted-by max:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_running.sort_values(['dur'], ascending=False)  \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['max'], ascending=False) \
                .head(nr_top))
        print()
        print("Top {} Running Tasks (ms) - sorted-by 90%:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_running.sort_values(['dur'], ascending=False)  \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['90%'], ascending=False) \
                .head(nr_top))

    if not df_usleep.empty:
        print()
        print("Top {} Uninterruptible Sleep Tasks (ms) - sorted-by max:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_usleep.sort_values(['dur'], ascending=False)   \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['max'], ascending=False) \
                .head(nr_top))
        print()
        print("Top {} Uninterruptible Sleep Tasks (ms) - sorted-by 90%:".format(nr_top))
        print("-"*settings.fig_width_tui)
        print(df_usleep.sort_values(['dur'], ascending=False)   \
                .groupby(['name', 'tid'])                       \
                .dur.describe(percentiles=[.75, .90, .95, .99]) \
                .round(2).sort_values(['90%'], ascending=False) \
                .head(nr_top))

def states_summary_parent(plt, parents=[]):

    if df_states.empty:
        return

    for parent in parents:
        df = df_states[df_states.parent.str.contains(parent).fillna(False)]

        for parent in sorted(df.parent.unique()):
            df_parent = df[df.parent == parent]

            states_summary(plt, df_parent.name.unique(), parent)

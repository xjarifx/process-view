#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "action.h"
#include "dashboard.h"
#include "monitor.h"

static volatile sig_atomic_t g_stop = 0;

#define AUTO_KILL_CPU_THRESHOLD 95.0
#define AUTO_KILL_MEM_THRESHOLD_KB (5L * 1024L * 1024L)
#define AUTO_KILL_CONSECUTIVE_LIMIT 3

typedef struct {
    pid_t pid;
    int count;
} ViolationRecord;

typedef struct {
    ViolationRecord *records;
    size_t count;
    size_t capacity;
} AutoKillState;

/* Finds the violation counter index for a PID, or -1 if it is not tracked yet. */
static int find_violation_record(const AutoKillState *state, pid_t pid) {
    size_t i;
    for (i = 0; i < state->count; ++i) {
        if (state->records[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

/* Returns whether the process should never be auto-terminated by safety policy. */
static int is_protected_process(pid_t self_pid, const ProcessInfo *proc) {
    static const char *protected_names[] = {"systemd", "init", "bash", "sshd", "process_monitor"};
    size_t i;

    if (proc->pid == self_pid) {
        return 1;
    }

    for (i = 0; i < sizeof(protected_names) / sizeof(protected_names[0]); ++i) {
        if (strcmp(proc->name, protected_names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Rebuilds the violation table from the latest process snapshot and counts. */
static int update_violation_state(AutoKillState *state, const ProcessInfo *list, size_t n) {
    ViolationRecord *next = NULL;
    size_t i;

    if (n > 0) {
        next = (ViolationRecord *)malloc(n * sizeof(ViolationRecord));
        if (!next) {
            return -1;
        }
    }

    for (i = 0; i < n; ++i) {
        next[i].pid = list[i].pid;
        next[i].count = list[i].violation_count;
    }

    free(state->records);
    state->records = next;
    state->count = n;
    state->capacity = n;
    return 0;
}

/* Frees resources used by auto-kill violation tracking. */
static void cleanup_violation_state(AutoKillState *state) {
    free(state->records);
    state->records = NULL;
    state->count = 0;
    state->capacity = 0;
}

/* Signal handler that requests a clean shutdown of the main loop. */
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Program entry point: initialize subsystems, run scan/update loop, and cleanup. */
int main(void) {
    MonitorContext monitor;
    DashboardState dashboard;
    AutoKillState auto_kill = {0};
    pid_t self_pid = getpid();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (monitor_init(&monitor) != 0) {
        fprintf(stderr, "Failed to initialize monitor\n");
        return 1;
    }

    if (dashboard_init(&dashboard) != 0) {
        fprintf(stderr, "Failed to initialize dashboard\n");
        monitor_cleanup(&monitor);
        return 1;
    }

    if (dashboard_start(&dashboard) != 0) {
        fprintf(stderr, "Failed to start dashboard server\n");
        dashboard_cleanup(&dashboard);
        monitor_cleanup(&monitor);
        return 1;
    }

    printf("Process monitor started\n");
    printf("Dashboard: http://127.0.0.1:%d/\n", dashboard.port);
    printf("Auto-kill enabled: CPU >= %.1f%% or MEM >= %ld KB for %d consecutive scans\n",
           AUTO_KILL_CPU_THRESHOLD,
           AUTO_KILL_MEM_THRESHOLD_KB,
           AUTO_KILL_CONSECUTIVE_LIMIT);

    while (!g_stop) {
        ProcessInfo *list = NULL;
        size_t count = 0;
        size_t i;

        if (monitor_scan(&monitor, &list, &count) == 0) {
            for (i = 0; i < count; ++i) {
                int idx = find_violation_record(&auto_kill, list[i].pid);
                int prev_count = (idx >= 0) ? auto_kill.records[idx].count : 0;
                int violating;

                list[i].over_cpu = (list[i].cpu_usage >= AUTO_KILL_CPU_THRESHOLD);
                list[i].over_mem = (list[i].memory_kb >= AUTO_KILL_MEM_THRESHOLD_KB);
                violating = list[i].over_cpu || list[i].over_mem;
                list[i].violation_count = violating ? (prev_count + 1) : 0;

                if (list[i].violation_count >= AUTO_KILL_CONSECUTIVE_LIMIT &&
                    !is_protected_process(self_pid, &list[i])) {
                    if (action_manual_terminate(&list[i]) == 0) {
                        list[i].violation_count = 0;
                    }
                }
            }

            if (update_violation_state(&auto_kill, list, count) != 0) {
                fprintf(stderr, "Warning: auto-kill tracking update failed\n");
            }
            dashboard_update(&dashboard, list, count);
        }

        free(list);

        if (!g_stop) {
            sleep(2);
        }
    }

    dashboard_stop(&dashboard);
    dashboard_cleanup(&dashboard);
    cleanup_violation_state(&auto_kill);
    monitor_cleanup(&monitor);

    return 0;
}

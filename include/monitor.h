#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>
#include <sys/types.h>

#include "utils.h"

typedef struct {
    pid_t pid;
    char name[PROC_NAME_MAX];
    double cpu_usage;
    long memory_kb;
    int violation_count;

    int over_cpu;
    int over_mem;

    unsigned long long proc_time;
} ProcessInfo;

typedef struct {
    pid_t pid;
    unsigned long long last_proc_time;
} PrevProcEntry;

typedef struct {
    PrevProcEntry *entries;
    size_t count;
    size_t capacity;
    unsigned long long prev_total_jiffies;
    int num_cpus;
} MonitorContext;

int monitor_init(MonitorContext *ctx);
void monitor_cleanup(MonitorContext *ctx);
int monitor_scan(MonitorContext *ctx, ProcessInfo **out_list, size_t *out_count);

#endif

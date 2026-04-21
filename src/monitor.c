#include "monitor.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Reads aggregate CPU jiffies from /proc/stat for delta-based CPU usage calculation. */
static unsigned long long read_total_jiffies(void) {
    FILE *fp;
    char line[1024];
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

    fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0;
    }

    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (sscanf(line,
               "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
               &user,
               &nice,
               &system,
               &idle,
               &iowait,
               &irq,
               &softirq,
               &steal) < 4) {
        return 0;
    }

    return user + nice + system + idle + iowait + irq + softirq + steal;
}

/* Looks up the previous sampled CPU time for a specific PID. */
static int get_prev_proc_time(MonitorContext *ctx, pid_t pid, unsigned long long *out) {
    size_t i;
    for (i = 0; i < ctx->count; ++i) {
        if (ctx->entries[i].pid == pid) {
            *out = ctx->entries[i].last_proc_time;
            return 0;
        }
    }
    return -1;
}

/* Replaces the previous PID->CPU-time table with the latest scan snapshot. */
static int update_prev_table(MonitorContext *ctx, const ProcessInfo *list, size_t n) {
    PrevProcEntry *new_entries;
    size_t i;

    new_entries = (PrevProcEntry *)malloc(n * sizeof(PrevProcEntry));
    if (!new_entries && n != 0) {
        return -1;
    }

    for (i = 0; i < n; ++i) {
        new_entries[i].pid = list[i].pid;
        new_entries[i].last_proc_time = list[i].proc_time;
    }

    free(ctx->entries);
    ctx->entries = new_entries;
    ctx->count = n;
    ctx->capacity = n;
    return 0;
}

/* Reads the process name from /proc/<pid>/comm. */
static int read_proc_name(pid_t pid, char *name, size_t len) {
    char path[64];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    if (!fgets(name, (int)len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    name[strcspn(name, "\n")] = '\0';
    return 0;
}

/* Reads resident memory (KB), first from status and then fallback to statm. */
static long read_proc_memory_kb(pid_t pid) {
    char path[64];
    FILE *fp;
    char key[64];
    long value;
    char unit[32];
    long total_pages;
    long rss_pages;
    long page_kb;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }

    while (fscanf(fp, "%63s %ld %31s", key, &value, unit) == 3) {
        if (strcmp(key, "VmRSS:") == 0) {
            fclose(fp);
            return value;
        }
    }

    fclose(fp);

    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }

    if (fscanf(fp, "%ld %ld", &total_pages, &rss_pages) == 2 && total_pages >= 0 && rss_pages >= 0) {
        page_kb = sysconf(_SC_PAGESIZE) / 1024L;
        if (page_kb <= 0) {
            page_kb = 4;
        }
        fclose(fp);
        return rss_pages * page_kb;
    }

    fclose(fp);
    return 0;
}

/* Reads user/system CPU times from /proc/<pid>/stat. */
static int read_proc_times(pid_t pid, unsigned long long *utime, unsigned long long *stime) {
    char path[64];
    FILE *fp;
    char buf[4096];
    char *rparen;
    char *rest;
    char *token;
    int field_index = 0;
    unsigned long long out_utime = 0;
    unsigned long long out_stime = 0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    rparen = strrchr(buf, ')');
    if (!rparen) {
        return -1;
    }

    rest = rparen + 2;
    token = strtok(rest, " ");
    while (token) {
        if (field_index == 11) {
            out_utime = strtoull(token, NULL, 10);
        } else if (field_index == 12) {
            out_stime = strtoull(token, NULL, 10);
            *utime = out_utime;
            *stime = out_stime;
            return 0;
        }
        token = strtok(NULL, " ");
        field_index++;
    }

    return -1;
}

/* Initializes monitor state and captures the CPU core count. */
int monitor_init(MonitorContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ctx->num_cpus <= 0) {
        ctx->num_cpus = 1;
    }
    return 0;
}

/* Releases monitor allocations and resets its state. */
void monitor_cleanup(MonitorContext *ctx) {
    free(ctx->entries);
    ctx->entries = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    ctx->prev_total_jiffies = 0;
}

/* Scans /proc, builds a process snapshot, and computes per-process CPU usage. */
int monitor_scan(MonitorContext *ctx, ProcessInfo **out_list, size_t *out_count) {
    DIR *proc_dir;
    struct dirent *entry;
    ProcessInfo *list = NULL;
    size_t count = 0;
    size_t cap = 0;
    unsigned long long total_jiffies;
    unsigned long long delta_total;

    *out_list = NULL;
    *out_count = 0;

    total_jiffies = read_total_jiffies();
    if (total_jiffies == 0) {
        return -1;
    }

    delta_total = (ctx->prev_total_jiffies > 0 && total_jiffies > ctx->prev_total_jiffies)
                      ? (total_jiffies - ctx->prev_total_jiffies)
                      : 0;

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        return -1;
    }

    while ((entry = readdir(proc_dir)) != NULL) {
        pid_t pid;
        ProcessInfo p;
        unsigned long long utime = 0;
        unsigned long long stime = 0;
        unsigned long long prev_proc = 0;
        unsigned long long delta_proc = 0;

        if (!is_numeric_str(entry->d_name)) {
            continue;
        }

        pid = (pid_t)atoi(entry->d_name);

        if (read_proc_name(pid, p.name, sizeof(p.name)) != 0) {
            continue;
        }

        if (read_proc_times(pid, &utime, &stime) != 0) {
            continue;
        }

        p.pid = pid;
        p.proc_time = utime + stime;
        p.memory_kb = read_proc_memory_kb(pid);
        p.cpu_usage = 0.0;
        p.violation_count = 0;
        p.over_cpu = 0;
        p.over_mem = 0;

        if (delta_total > 0 && get_prev_proc_time(ctx, pid, &prev_proc) == 0 && p.proc_time >= prev_proc) {
            delta_proc = p.proc_time - prev_proc;
            p.cpu_usage = ((double)delta_proc / (double)delta_total) * 100.0 * (double)ctx->num_cpus;
            if (p.cpu_usage < 0.0) {
                p.cpu_usage = 0.0;
            }
        }

        if (count == cap) {
            size_t new_cap = (cap == 0) ? 128 : cap * 2;
            ProcessInfo *tmp = (ProcessInfo *)realloc(list, new_cap * sizeof(ProcessInfo));
            if (!tmp) {
                free(list);
                closedir(proc_dir);
                return -1;
            }
            list = tmp;
            cap = new_cap;
        }

        list[count++] = p;
    }

    closedir(proc_dir);

    if (update_prev_table(ctx, list, count) != 0) {
        free(list);
        return -1;
    }

    ctx->prev_total_jiffies = total_jiffies;
    *out_list = list;
    *out_count = count;
    return 0;
}

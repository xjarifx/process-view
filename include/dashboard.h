#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stddef.h>
#include <pthread.h>

#include "monitor.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    int running;
    int listen_fd;
    int port;

    ProcessInfo *processes;
    size_t count;
    size_t capacity;
} DashboardState;

int dashboard_init(DashboardState *state);
void dashboard_cleanup(DashboardState *state);
int dashboard_start(DashboardState *state);
void dashboard_stop(DashboardState *state);
void dashboard_update(DashboardState *state, const ProcessInfo *list, size_t count);

#endif

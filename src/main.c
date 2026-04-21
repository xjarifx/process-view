#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dashboard.h"
#include "monitor.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(void) {
    MonitorContext monitor;
    DashboardState dashboard;

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

    while (!g_stop) {
        ProcessInfo *list = NULL;
        size_t count = 0;

        if (monitor_scan(&monitor, &list, &count) == 0) {
            dashboard_update(&dashboard, list, count);
        }

        free(list);

        if (!g_stop) {
            sleep(2);
        }
    }

    dashboard_stop(&dashboard);
    dashboard_cleanup(&dashboard);
    monitor_cleanup(&monitor);

    return 0;
}

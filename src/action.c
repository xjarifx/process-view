#include "action.h"

#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int process_alive(pid_t pid) {
    if (kill(pid, 0) == 0) {
        return 1;
    }
    if (errno == EPERM) {
        return 1;
    }
    return 0;
}

int action_manual_terminate(const ProcessInfo *proc) {
    struct timespec ts;
    int i;

    if (!proc) {
        return -1;
    }

    if (kill(proc->pid, SIGTERM) != 0) {
        return -1;
    }

    ts.tv_sec = 0;
    ts.tv_nsec = 200000000L;
    for (i = 0; i < 5; ++i) {
        nanosleep(&ts, NULL);
        if (!process_alive(proc->pid)) {
            return 0;
        }
    }

    if (kill(proc->pid, SIGKILL) != 0) {
        return -1;
    }

    return 0;
}

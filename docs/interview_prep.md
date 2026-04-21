# Process Monitor Project Prep

This document explains how the codebase works so you can answer questions about it in a review, demo, or viva.

## 1. High-Level Flow

The program has three main parts:

- `monitor`: reads process and CPU/memory information from `/proc`
- `dashboard`: serves the web GUI and keeps a snapshot of the latest process list
- `action`: terminates a selected process manually from the dashboard

The execution flow is:

1. `main` initializes the monitor and dashboard.
2. `monitor_scan()` reads the current process table from `/proc`.
3. `dashboard_update()` stores the latest snapshot.
4. The browser UI fetches `/api/state` every second.
5. When you click Terminate, the UI calls `/api/terminate?pid=<pid>`.

## 2. How Process Information Is Collected

The process information is collected directly from Linux procfs.

### What is read

For each PID under `/proc`:

- `/proc/<pid>/comm` gives the process name
- `/proc/<pid>/stat` gives CPU time fields
- `/proc/<pid>/status` or `/proc/<pid>/statm` gives memory usage
- `/proc/stat` gives total system CPU jiffies

### How CPU usage is computed

The monitor does not read a CPU percentage directly from the kernel. It calculates it from deltas:

- store the previous total system jiffies
- store the previous process CPU time for each PID
- on the next scan, subtract old values from new values
- convert the ratio into a percentage

### Relevant code

```c
int monitor_scan(MonitorContext *ctx, ProcessInfo **out_list, size_t *out_count) {
    total_jiffies = read_total_jiffies();
    delta_total = (ctx->prev_total_jiffies > 0 && total_jiffies > ctx->prev_total_jiffies)
                      ? (total_jiffies - ctx->prev_total_jiffies)
                      : 0;

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_numeric_str(entry->d_name)) {
            continue;
        }

        pid = (pid_t)atoi(entry->d_name);
        read_proc_name(pid, p.name, sizeof(p.name));
        read_proc_times(pid, &utime, &stime);

        p.proc_time = utime + stime;
        p.memory_kb = read_proc_memory_kb(pid);

        if (delta_total > 0 && get_prev_proc_time(ctx, pid, &prev_proc) == 0 && p.proc_time >= prev_proc) {
            delta_proc = p.proc_time - prev_proc;
            p.cpu_usage = ((double)delta_proc / (double)delta_total) * 100.0 * (double)ctx->num_cpus;
        }
    }
}
```

### What to say in an interview

You can say: the app scans procfs on each cycle, converts raw CPU ticks into a percentage using previous snapshots, and computes memory directly from process status/statm.

## 3. How the GUI Works

The GUI is not a desktop app. It is a small local web server that serves HTML and JSON on `127.0.0.1:7878`.

### Server side

The dashboard creates a listening socket, accepts HTTP requests, and returns either:

- `/` or `/index.html` for the HTML page
- `/api/state` for JSON process data
- `/api/terminate?pid=<pid>` for manual termination

### Client side

The page uses JavaScript `fetch()` to refresh the process table every second.

### Relevant code

```js
async function refresh() {
  const res = await fetch("/api/state", { cache: "no-store" });
  const data = await res.json();
  const rows = document.getElementById("rows");
  rows.innerHTML = "";

  for (const p of data.processes) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${p.pid}</td><td>${p.name}</td><td>${p.cpu_usage.toFixed(2)}</td><td>${fmtKb(p.memory_kb)}</td>`;
    rows.appendChild(tr);
  }
}

refresh();
setInterval(refresh, 1000);
```

### What to say in an interview

You can say: the browser is just a frontend for a local HTTP server, and the table refreshes via polling, not WebSockets.

## 4. How Process Termination Works

Termination is manual only.

When the user clicks the Terminate button:

1. The browser sends a POST request to `/api/terminate?pid=<pid>`.
2. The dashboard looks up the process in the current snapshot.
3. `action_manual_terminate()` sends `SIGTERM` first.
4. If the process is still alive after a short wait, it sends `SIGKILL`.

### Relevant code

```c
int action_manual_terminate(const ProcessInfo *proc) {
    if (kill(proc->pid, SIGTERM) != 0) {
        return -1;
    }

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
```

### Why SIGTERM first

SIGTERM is the polite request to exit. SIGKILL is the fallback if the process does not stop in time.

### What to say in an interview

You can say: manual termination is implemented as a two-step shutdown, giving the target process a chance to exit cleanly before forcing it down.

## 5. How the Data Is Updated Frequently

There are two refresh loops working together:

- the backend scan loop in `main` runs every 2 seconds
- the frontend polls `/api/state` every 1 second

That means the dashboard keeps showing a recent snapshot even though the scanner runs at a slightly slower pace.

### Relevant code

```c
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
```

### Important detail

`dashboard_update()` copies the latest process list into shared dashboard memory under a mutex, so the HTTP handler can read it safely while the scan loop continues.

## 6. Good Questions You May Be Asked

### Q: Why did you use procfs instead of a library?

A: Because procfs is built into Linux, gives direct access to process data, and keeps the project simple and transparent.

### Q: How do you compute CPU usage correctly?

A: By comparing two samples of process CPU time and total system jiffies, then converting the delta ratio into a percentage.

### Q: Why does the GUI use polling instead of WebSockets?

A: Polling was simpler to implement and is enough for a local dashboard that refreshes once per second.

### Q: Why is termination manual only?

A: The requirement is to let the user choose when to terminate a process. The UI provides a button and the backend only acts on user request.

### Q: What happens if a process exits before it is terminated?

A: The next scan skips missing `/proc/<pid>` entries, and the dashboard simply stops showing that process.

### Q: Why copy the process list into the dashboard state?

A: So the HTTP thread can serve a stable snapshot while the scan loop updates the next one.

### Q: Is the GUI a separate application?

A: No. It is an embedded local web server opened in the browser with `xdg-open`.

### Q: How does the app stop cleanly?

A: `SIGINT` and `SIGTERM` set a stop flag, the loop exits, the dashboard thread is stopped, and allocated memory is freed.

## 7. Short Summary You Can Memorize

The project is a Linux process monitor that reads live process data from procfs, stores a snapshot in a local web server, and refreshes the UI automatically. Users can terminate a process manually from the dashboard, and the backend uses SIGTERM first with SIGKILL as a fallback.

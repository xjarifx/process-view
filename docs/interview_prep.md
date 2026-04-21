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
# Process Monitor Project Prep

This is the long-form study guide for the project. It is written so you can answer questions about the architecture, the data flow, the browser UI, the process scanning logic, and the termination behavior without having to re-read the code every time.

## 1. What The Project Is

At a high level, this project is a Linux process monitor with a local web UI.

It does four things:

1. Reads live process data from `/proc`.
2. Calculates CPU usage from sampled deltas.
3. Serves a browser dashboard on `127.0.0.1:7878`.
4. Lets you terminate a process manually, and also auto-terminates processes when they exceed the configured threshold for multiple scans.

It is not a desktop GUI and it is not a browser app backed by a separate frontend framework. The GUI is embedded directly inside the C server as HTML, CSS, and JavaScript strings.

## 2. Overall Architecture

The codebase is small, but each file has a specific role:

- [src/main.c](src/main.c) owns the program lifecycle, the scan loop, and the auto-kill policy.
- [src/monitor.c](src/monitor.c) walks `/proc` and builds a fresh snapshot of processes.
- [src/dashboard.c](src/dashboard.c) serves the web page, JSON API, and terminate endpoint.
- [src/action.c](src/action.c) performs the actual kill sequence with `SIGTERM` and fallback `SIGKILL`.
- [src/utils.c](src/utils.c) currently contains a small helper for identifying numeric `/proc` directory names.

The important mental model is this:

monitor produces data, main decides what to do with that data, dashboard exposes the data to the browser, and action performs termination.

## 3. End-To-End Data Flow

The control flow looks like this:

1. `main()` starts the monitor and dashboard.
2. Every 2 seconds, `monitor_scan()` reads a new snapshot from `/proc`.
3. `main()` evaluates thresholds and keeps per-PID violation counts.
4. If a process exceeds the auto-kill rule, `action_manual_terminate()` is called.
5. `dashboard_update()` stores the latest snapshot in shared memory.
6. The browser polls `/api/state` every 1 second.
7. The browser renders the table from JSON.
8. If you click Terminate, the browser calls `/api/terminate?pid=<pid>`.

This means the system has two loops running at different rates:

- backend scan loop: every 2 seconds
- frontend refresh loop: every 1 second

The frontend may refresh more often than the backend scans, but that is okay because it is just reading the latest cached snapshot.

## 4. How Process Information Is Gathered

The process data comes from Linux procfs. The monitor does not use an external process library.

### 4.1 Files read from `/proc`

For each PID, the scanner reads:

- `/proc/<pid>/comm` for the process name
- `/proc/<pid>/stat` for user time and system time
- `/proc/<pid>/status` for resident memory, with a fallback to `/proc/<pid>/statm`
- `/proc/stat` for total system CPU jiffies

### 4.2 Why the code checks for numeric directory names

`/proc` contains both numeric process directories and non-process entries such as `self`, `cpuinfo`, and `meminfo`.

The helper `is_numeric_str()` is used to keep only PIDs:

```c
int is_numeric_str(const char *s) {
    size_t i;

    if (!s || !*s) {
        return 0;
    }

    for (i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }

    return 1;
}
```

This is a very common procfs pattern. If the entry name is not purely numeric, it is not a process ID.

### 4.3 How memory is read

The code tries `/proc/<pid>/status` first and looks for `VmRSS:`. That gives resident memory in KB.

If that fails, it falls back to `/proc/<pid>/statm` and multiplies the resident page count by the system page size.

This fallback exists because procfs fields can vary slightly depending on process state and permissions.

### 4.4 How CPU usage is calculated

The monitor does not get CPU percentage directly from the kernel. Instead, it computes it from deltas.

The logic is:

1. Read total system jiffies from `/proc/stat`.
2. Read each process's `utime + stime` from `/proc/<pid>/stat`.
3. Remember the previous sample for each PID.
4. On the next scan, subtract previous from current.
5. Divide the process delta by the total delta.
6. Multiply by 100 and by the CPU count.

That is why the monitor stores `prev_total_jiffies` and the per-process `last_proc_time` table.

### 4.5 Why the CPU formula multiplies by CPU count

The formula in the code is:

```c
p.cpu_usage = ((double)delta_proc / (double)delta_total) * 100.0 * (double)ctx->num_cpus;
```

The extra `num_cpus` factor scales the usage so a single busy process can exceed 100% on multi-core systems.

That is important because process CPU usage on Linux is often reported as:

- around `100%` when it fully uses one core
- above `100%` when it spans multiple cores

### 4.6 Why the first scan may show `0.0`

On the first scan there is no prior sample to compare against, so delta-based CPU usage cannot be computed yet.

That is why the first snapshot often shows `0.0` or a very low CPU value. The next scan has the previous sample and produces a meaningful value.

## 5. The Monitor Module In Detail

### `read_total_jiffies()`

This function reads the first line of `/proc/stat` and parses the aggregate CPU counters.

It matters because those counters are the baseline used to calculate process CPU percentages.

### `get_prev_proc_time()`

This function searches the previous sample table for a specific PID.

It is part of the delta logic. Without a previous value, CPU usage cannot be computed for that process.

### `update_prev_table()`

This function replaces the previous PID/time table with the newly sampled list.

The reason it creates a fresh table each time is simplicity. Since the process list changes all the time, rebuilding the table is easier than trying to keep it fully incremental.

### `read_proc_name()`

This reads `/proc/<pid>/comm`, which is the short process name.

This is what the dashboard displays in the Name column.

### `read_proc_memory_kb()`

This reads memory usage in KB.

The fallback from `status` to `statm` is important because it makes the code more robust across different process states.

### `read_proc_times()`

This reads the process's CPU time from `/proc/<pid>/stat`.

It parses the `utime` and `stime` fields, then combines them into `proc_time`.

### `monitor_init()`

This zeroes the state and stores the CPU count from `sysconf(_SC_NPROCESSORS_ONLN)`.

That CPU count is needed for the CPU percentage calculation.

### `monitor_cleanup()`

This frees the previous-process table and resets the fields.

This matters because the monitor allocates memory every scan and must release it on shutdown.

### `monitor_scan()`

This is the main scanner.

It:

1. Reads total CPU jiffies.
2. Opens `/proc`.
3. Iterates through numeric entries.
4. Reads name, CPU time, and memory for each process.
5. Computes CPU usage if a previous sample exists.
6. Appends each result to a dynamically growing array.
7. Replaces the previous sample table with the new one.
8. Returns the process array to the caller.

This is the core data collection path of the project.

### Relevant code snippet

```c
if (delta_total > 0 && get_prev_proc_time(ctx, pid, &prev_proc) == 0 && p.proc_time >= prev_proc) {
    delta_proc = p.proc_time - prev_proc;
    p.cpu_usage = ((double)delta_proc / (double)delta_total) * 100.0 * (double)ctx->num_cpus;
}
```

### What to say in an interview

You can say:

> The monitor samples `/proc` periodically, keeps the previous CPU-time snapshot per PID, and computes usage from the delta between samples.

## 6. The Dashboard In Detail

The dashboard is a local HTTP server, but it is also the place where the UI lives.

### 6.1 Why the GUI is inside `dashboard.c`

There are no separate frontend files because the project embeds the page directly as a C string.

This keeps the project small and self-contained, which is useful for a lab or coursework project.

### 6.2 What `dashboard_init()` does

It initializes the mutex, sets the server port to `7878`, and marks the listening socket as not yet opened.

### 6.3 What `dashboard_start()` does

It:

1. Creates a TCP socket.
2. Enables `SO_REUSEADDR`.
3. Binds to `127.0.0.1:7878`.
4. Starts listening.
5. Spawns the server thread.
6. Launches the browser with `xdg-open`.

### 6.4 Why the browser is opened automatically

The helper `launch_browser()` uses `fork()` and `execlp("xdg-open", ...)`.

That means the program tries to open the system's default browser for the local dashboard URL.

### 6.5 What `/api/state` returns

The `/api/state` route returns a JSON object with:

- `count`
- `processes`

Each process object contains:

- `pid`
- `name`
- `cpu_usage`
- `memory_kb`

### 6.6 Why the dashboard copies the process list

The dashboard thread must not read the process array while `main` is updating it.

So `dashboard_update()` copies the scan result into shared memory behind a mutex.

That makes the snapshot stable while the HTTP handler serves it.

### 6.7 Why there is a mutex

The dashboard has one thread that serves HTTP and one thread-like scan loop that updates data from `main`.

The mutex prevents races between:

- writing the latest snapshot
- reading the snapshot for `/api/state`

### 6.8 How the UI refreshes

The embedded JavaScript does a `fetch('/api/state')` every second.

That is simple polling. It is not using WebSockets, SSE, or any external frontend framework.

### 6.9 Relevant frontend code

```js
async function refresh(){
  const res = await fetch('/api/state',{cache:'no-store'});
  const data = await res.json();
  const rows = document.getElementById('rows');
  rows.innerHTML='';
  for(const p of data.processes){
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${p.pid}</td><td>${p.name}</td><td>${p.cpu_usage.toFixed(2)}</td><td>${fmtKb(p.memory_kb)}</td>`;
    rows.appendChild(tr);
  }
}

refresh();
setInterval(refresh,1000);
```

### What to say in an interview

You can say:

> The browser is just a thin client. The real data source is the local HTTP server, and the page polls it once per second.

## 7. Manual Termination In Detail

Manual termination is triggered by the Terminate button in the table.

### 7.1 Request path

1. The browser confirms the action with a dialog.
2. It sends `POST /api/terminate?pid=<pid>`.
3. The server parses the PID from the query string.
4. The server looks up that PID in the current snapshot.
5. `action_manual_terminate()` is called.

### 7.2 Why the code checks the dashboard snapshot first

The terminate endpoint does not kill an arbitrary PID blindly.

It only allows termination of a process that is currently in the dashboard snapshot. That gives you a sanity check and keeps the UI consistent with what the user sees.

### 7.3 `action_manual_terminate()` behavior

The terminate helper is intentionally conservative:

1. Send `SIGTERM`.
2. Wait briefly in a loop.
3. Check whether the process is still alive.
4. If still alive after the wait, send `SIGKILL`.

### 7.4 Relevant code

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

## 8. How The Data Is Updated Frequently

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

## 9. Auto-Kill Logic In Detail

Auto-kill lives in `main.c` because it is a policy decision, not a data collection job.

### 9.1 Current thresholds

The constants are:

- CPU threshold: `80.0%`
- Memory threshold: `2 GB` (`2L * 1024L * 1024L` KB)
- Consecutive violation limit: `3` scans

### 9.2 How it works

For each scan:

1. `monitor_scan()` returns the current process list.
2. `main()` checks each process against the CPU and memory thresholds.
3. If a process is over either threshold, its consecutive violation count increases.
4. If it is below both thresholds, the count resets to 0.
5. When the count reaches 3, the process is auto-terminated.

### 9.3 What counts as a violation

A process violates the rule when either condition is true:

- `cpu_usage >= 80.0`
- `memory_kb >= 2 * 1024 * 1024`

This is an OR condition, not an AND condition.

### 9.4 Safety checks

The code skips auto-kill for:

- the monitor process itself
- `systemd`
- `init`
- `bash`
- `sshd`
- `process_monitor`

That prevents the tool from killing itself or core system/session processes.

### 9.5 Why auto-kill uses the same termination helper

The code calls `action_manual_terminate()` for auto-kill too.

That means the kill sequence is still the same:

1. try `SIGTERM`
2. wait a little
3. fall back to `SIGKILL` if needed

The difference is the trigger. Manual termination comes from a user click. Auto-kill comes from threshold logic in `main`.

### 9.6 Relevant code

```c
if (list[i].violation_count >= AUTO_KILL_CONSECUTIVE_LIMIT &&
    !is_protected_process(self_pid, &list[i])) {
    if (action_manual_terminate(&list[i]) == 0) {
        list[i].violation_count = 0;
    }
}
```

### What to say in an interview

You can say:

> Auto-kill is a policy in the main loop. A process must stay above CPU or memory thresholds for three consecutive scans before it is terminated, and protected processes are skipped.

## 10. Why The Project Uses This Structure

### Why not use a process library?

Because procfs is the standard Linux source of truth for process data, and using it directly makes the implementation easier to explain and debug.

### Why use a local HTTP server instead of a desktop toolkit?

Because the browser is a universal UI and the server approach keeps the project portable within Linux.

### Why embed the frontend instead of using separate files?

Because the project is small. Embedding HTML/CSS/JS avoids build complexity and keeps everything in one binary.

### Why use polling instead of push-based updates?

Because polling is simple and sufficient for a dashboard that refreshes every second.

### Why copy the process snapshot instead of reading it directly from the scan buffer?

Because the scan buffer is temporary. The dashboard needs a stable snapshot while it serves requests concurrently.

## 11. What Happens On Shutdown

When the program receives `SIGINT` or `SIGTERM`:

1. The signal handler sets `g_stop = 1`.
2. The scan loop exits after the current cycle.
3. The dashboard server is stopped.
4. The dashboard memory is freed.
5. The monitor tables are freed.

This is a clean shutdown path. There is no hard process exit inside the signal handler itself.

## 12. Questions You May Be Asked

### Q: Why does the first scan often show zero CPU values?

A: Because CPU usage is computed from deltas between two samples. The first sample has no previous baseline.

### Q: Why do you multiply by the CPU count?

A: To reflect that a process can use more than one core and therefore exceed 100% on multi-core machines.

### Q: Why do you need both `status` and `statm` for memory?

A: `status` is the primary source for RSS, and `statm` is a fallback if `status` is unavailable or incomplete.

### Q: Why is the dashboard thread protected by a mutex?

A: The snapshot is shared between the update path and the HTTP handler, so the mutex prevents races.

### Q: Why is auto-kill implemented in `main` instead of `action.c`?

A: Because auto-kill is policy. `action.c` is only responsible for the actual termination sequence.

### Q: Why do you skip `process_monitor` and `bash` from auto-kill?

A: To avoid killing the monitor itself and to avoid disrupting the shell/session used to run it.

### Q: Is the GUI truly separate from the backend?

A: No. The backend serves the GUI as embedded HTML/CSS/JS.

### Q: What happens if a PID disappears before terminate is clicked?

A: The next refresh drops it from the snapshot, and the terminate endpoint returns an error because the PID is no longer present.

## 13. Short Summary You Can Memorize

The project is a Linux process monitor that reads live process data from procfs, stores a snapshot in a local web server, and refreshes the UI automatically. Users can terminate a process manually from the dashboard, and the backend uses SIGTERM first with SIGKILL as a fallback. Auto-kill is also built into the main scan loop: if a process stays above the CPU or memory threshold for three consecutive scans, it is terminated unless it is protected.

        if (!g_stop) {
            sleep(2);
        }
    }
    ```

    ### Why the loop sleeps

    The sleep reduces CPU overhead and sets the scan cadence.

    Without the sleep, the monitor would constantly scan `/proc` and consume unnecessary resources.

    ### Why `SIGINT` and `SIGTERM` are handled

    Those signals let the program stop cleanly.

    Instead of dying immediately, the signal handler sets a flag and the main loop exits naturally, which allows cleanup code to run.

    ## 10. Threading And Synchronization

    This project uses a mix of a main loop and a server thread.

    ### Why there is a thread

    The dashboard needs to keep serving requests while the monitor keeps scanning.

    The simplest way to do that in C is to run the HTTP server in a separate thread.

    ### What is protected by the mutex

    The dashboard mutex protects:

    - the stored process snapshot
    - the count of rows currently being served
    - the capacity of the buffer

    This makes the snapshot safe to read while it is being replaced.

    ### Important interview point

    If asked about concurrency, you can say:

    > The design is intentionally simple: the scanner runs in the main thread and the HTTP server runs in a background thread, with a mutex protecting the shared snapshot.

    ## 11. Why The Code Uses Inline HTML Instead Of Separate Frontend Files

    This is a common question.

    The answer is that the project keeps everything self-contained.

    Advantages:

    - fewer files
    - easier to grade or demo
    - no build pipeline for the frontend
    - no dependency on a JS framework

    Tradeoff:

    - the HTML string is long and harder to maintain

    That tradeoff is acceptable for a small systems project, but a larger real-world project would usually split the frontend into separate files.

    ## 12. Common Edge Cases

    ### 12.1 A process exits while you are scanning it

    This can happen all the time on Linux.

    The code handles it by skipping entries whose procfs files disappear while reading.

    ### 12.2 A process exits between refreshes

    The next scan simply does not include it, so the dashboard stops showing it.

    ### 12.3 CPU usage is zero on the first pass

    That is expected because there is no previous sample to compare against.

    ### 12.4 The browser shows stale data for up to one second

    That is normal because the frontend polls every second.

    ### 12.5 A process is protected but still appears in the table

    That is also expected. Protection only affects auto-kill. The dashboard still displays it.

    ## 13. Good Interview Questions And Answers

    ### Q: Why did you choose procfs?

    A: Because procfs is built into Linux, easy to access from C, and gives direct access to the process data needed for the monitor.

    ### Q: How is CPU usage computed?

    A: By comparing two samples of process CPU time and total system jiffies, then converting that delta into a percentage.

    ### Q: Why is the GUI implemented with raw HTTP strings?

    A: Because the project is small and self-contained. Raw strings are enough to serve one page and one JSON endpoint without adding dependencies.

    ### Q: Why not use WebSockets?

    A: Polling is simpler and sufficient for a dashboard that refreshes once per second.

    ### Q: Why is there both manual kill and auto-kill?

    A: Manual kill is for user control. Auto-kill is for enforcing thresholds when a process stays over the limit for multiple scans.

    ### Q: What prevents the program from killing itself?

    A: The auto-kill path explicitly excludes the monitor process and several protected system/session process names.

    ### Q: Why does termination use SIGTERM first?

    A: Because it gives the process a chance to exit cleanly before forcing it with SIGKILL.

    ### Q: Why does the dashboard copy the process list instead of sharing it directly?

    A: To avoid data races between the scan loop and the HTTP server thread.

    ### Q: What happens if a kill fails?

    A: The helper returns an error, and the calling code can log or report that the process was not terminated.

    ### Q: How does the app exit cleanly?

    A: The signal handler sets a stop flag, the scan loop exits, the dashboard thread is stopped, and all allocated memory is freed.

    ## 14. Small Code Examples To Memorize

    ### Reading a process name

    ```c
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fp = fopen(path, "r");
    fgets(name, (int)len, fp);
    ```

    ### Computing CPU usage from deltas

    ```c
    p.cpu_usage = ((double)delta_proc / (double)delta_total) * 100.0 * (double)ctx->num_cpus;
    ```

    ### Sending a terminate request from the browser

    ```js
    fetch('/api/terminate?pid=' + pid, { method: 'POST' })
    ```

    ### Graceful then forceful termination

    ```c
    kill(proc->pid, SIGTERM);
    nanosleep(&ts, NULL);
    kill(proc->pid, SIGKILL);
    ```

    ## 15. Short Summary You Can Memorize

    This project is a Linux process monitor that scans `/proc`, computes CPU and memory usage, stores a snapshot for a local web dashboard, and lets the user terminate processes manually. It also has an auto-kill policy in `main.c` that terminates a process after it stays above the CPU or memory threshold for three consecutive scans, while skipping protected processes.

    ## 16. One-Sentence Answer If You Freeze In A Demo

    > The monitor reads live data from procfs, the dashboard serves a local browser UI, and the termination logic is implemented in the backend with both manual and threshold-based auto-kill paths.

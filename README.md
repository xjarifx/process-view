# Process Monitor Dashboard

A Linux process monitor written in C that shows running processes and resource usage in a web GUI and lets you terminate a selected process manually.

## What It Does

- Scans `/proc` for running processes
- Displays PID, name, CPU usage, and memory usage
- Serves a local web dashboard
- Lets you terminate a process from the dashboard

## Build and Run

```bash
make
./bin/process_monitor
```

The dashboard opens automatically at `http://127.0.0.1:7878/`.

## Notes

- Linux only.
- If a process exits between refreshes, it is skipped on the next scan.

#include "dashboard.h"

#include "action.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DASHBOARD_PORT 7878
#define DASHBOARD_BACKLOG 8
#define REQUEST_BUF_SIZE 2048

static int cmp_cpu_desc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;

    if (pb->cpu_usage > pa->cpu_usage) {
        return 1;
    }
    if (pb->cpu_usage < pa->cpu_usage) {
        return -1;
    }
    return 0;
}

static int ensure_capacity(DashboardState *state, size_t needed) {
    ProcessInfo *tmp;
    size_t new_cap;

    if (needed <= state->capacity) {
        return 0;
    }

    new_cap = (state->capacity == 0) ? needed : state->capacity;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap == 0) {
            new_cap = needed;
            break;
        }
    }

    tmp = (ProcessInfo *)realloc(state->processes, new_cap * sizeof(ProcessInfo));
    if (!tmp) {
        return -1;
    }

    state->processes = tmp;
    state->capacity = new_cap;
    return 0;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            return;
        }
        off += (size_t)n;
    }
}

static void write_str(int fd, const char *s) {
    write_all(fd, s, strlen(s));
}

static void write_json_string(int fd, const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    write_str(fd, "\"");
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
            case '\\':
                write_str(fd, "\\\\");
                break;
            case '"':
                write_str(fd, "\\\"");
                break;
            case '\b':
                write_str(fd, "\\b");
                break;
            case '\f':
                write_str(fd, "\\f");
                break;
            case '\n':
                write_str(fd, "\\n");
                break;
            case '\r':
                write_str(fd, "\\r");
                break;
            case '\t':
                write_str(fd, "\\t");
                break;
            default:
                if (c < 0x20) {
                    char tmp[7];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    write_str(fd, tmp);
                } else {
                    char ch = (char)c;
                    write_all(fd, &ch, 1);
                }
                break;
        }
    }
    write_str(fd, "\"");
}

static void send_response(int fd, const char *status, const char *content_type, const char *body) {
    char header[256];
    size_t body_len = strlen(body);

    snprintf(header,
             sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
             status,
             content_type,
             body_len);
    write_str(fd, header);
    write_str(fd, body);
}

static void send_html(int fd) {
    static const char *html =
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Process Monitor</title>"
        "<style>"
        ":root{color-scheme:dark;--bg:#081120;--panel:#111b33;--panel2:#0d1629;--text:#edf2ff;--muted:#93a4c3;--accent:#66e0d6;--bad:#ff7b8a;--good:#58d68d;--line:rgba(147,164,195,.18)}"
        "*{box-sizing:border-box}"
        "body{margin:0;font-family:'IBM Plex Sans','Segoe UI',sans-serif;background:radial-gradient(circle at top left,#20305d 0%,#081120 44%,#030711 100%);color:var(--text)}"
        ".wrap{max-width:1240px;margin:0 auto;padding:28px 18px 36px}"
        ".hero{display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap;align-items:flex-end;margin-bottom:18px}"
        "h1{margin:0;font-size:clamp(30px,4vw,48px);letter-spacing:-.04em}"
        ".sub{margin-top:8px;max-width:760px;line-height:1.5;color:var(--muted)}"
        ".pill{padding:10px 14px;border-radius:999px;border:1px solid var(--line);background:rgba(13,22,41,.76);backdrop-filter:blur(10px)}"
        ".panel{border:1px solid var(--line);border-radius:24px;overflow:hidden;background:rgba(9,16,31,.84);box-shadow:0 20px 46px rgba(0,0,0,.3)}"
        ".head{display:flex;justify-content:space-between;gap:12px;align-items:center;padding:18px 20px;border-bottom:1px solid var(--line)}"
        ".head h2{margin:0;font-size:20px}"
        ".head span{color:var(--muted)}"
        "table{width:100%;border-collapse:collapse}"
        "th,td{padding:14px 20px;text-align:left;border-bottom:1px solid rgba(147,164,195,.12);font-size:14px}"
        "th{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.12em;background:rgba(255,255,255,.02)}"
        "tbody tr:hover{background:rgba(255,255,255,.03)}"
        ".btn{background:#1d2a4a;color:#edf2ff;border:1px solid rgba(147,164,195,.3);border-radius:10px;padding:7px 10px;font-size:12px;cursor:pointer}"
        ".btn:hover{background:#27365f}"
        ".btn:disabled{opacity:.45;cursor:not-allowed}"
        ".ok{color:var(--good)}"
        ".hot{color:#ffd166}"
        ".vio{color:var(--bad)}"
        ".foot{margin-top:14px;color:var(--muted);font-size:13px}"
        "@media (max-width:900px){table,thead,tbody,th,td,tr{display:block}thead{display:none}tbody tr{padding:10px 0;border-bottom:1px solid rgba(147,164,195,.12)}td{border:0;padding:8px 20px;display:flex;justify-content:space-between;gap:12px}td::before{content:attr(data-label);color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"wrap\">"
        "<div class=\"hero\">"
        "<div>"
        "<h1>Process Monitor</h1>"
        "<div class=\"sub\">A web GUI for viewing running processes and terminating them manually.</div>"
        "</div>"
        "<div class=\"pill\" id=\"status\">Waiting for data...</div>"
        "</div>"
        "<div class=\"panel\">"
        "<div class=\"head\"><h2>Processes</h2><span>Refreshes every second</span></div>"
        "<table>"
        "<thead><tr><th>PID</th><th>Name</th><th>CPU %</th><th>Memory</th><th>Action</th></tr></thead>"
        "<tbody id=\"rows\"></tbody>"
        "</table>"
        "</div>"
        "<div class=\"foot\">Use the terminate button only for a process you want to stop.</div>"
        "</div>"
        "<script>"
        "function fmtKb(kb){if(kb>=1048576)return (kb/1048576).toFixed(2)+' GB';if(kb>=1024)return (kb/1024).toFixed(2)+' MB';return kb+' KB';}"
        "async function terminatePid(pid){if(!confirm('Terminate PID '+pid+'?'))return;const res=await fetch('/api/terminate?pid='+pid,{method:'POST'});const data=await res.json();if(!res.ok){alert(data.message||'Terminate failed');}refresh();}"
        "async function refresh(){try{const res=await fetch('/api/state',{cache:'no-store'});const data=await res.json();document.getElementById('status').textContent='Tracked '+data.count+' processes';const rows=document.getElementById('rows');rows.innerHTML='';for(const p of data.processes){const tr=document.createElement('tr');tr.innerHTML=`<td data-label='PID'>${p.pid}</td><td data-label='Name'>${p.name}</td><td data-label='CPU %' class='${p.cpu_usage>=75?'hot':(p.cpu_usage>=50?'ok':'')}'>${p.cpu_usage.toFixed(2)}</td><td data-label='Memory'>${fmtKb(p.memory_kb)}</td><td data-label='Action'><button class='btn' onclick='terminatePid(${p.pid})'>Terminate</button></td>`;rows.appendChild(tr);}}catch(err){document.getElementById('status').textContent='Disconnected';}}refresh();setInterval(refresh,1000);"
        "</script>"
        "</body></html>";

    send_response(fd, "200 OK", "text/html; charset=utf-8", html);
}

static void send_json(DashboardState *state, int fd) {
    ProcessInfo *processes = NULL;
    size_t count;
    size_t i;

    pthread_mutex_lock(&state->mutex);
    count = state->count;
    if (count > 0) {
        processes = (ProcessInfo *)malloc(count * sizeof(ProcessInfo));
        if (processes) {
            memcpy(processes, state->processes, count * sizeof(ProcessInfo));
        } else {
            count = 0;
        }
    }
    pthread_mutex_unlock(&state->mutex);

    write_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n{");
    write_str(fd, "\"count\":");
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", count);
        write_str(fd, buf);
    }
    write_str(fd, ",\"processes\":[");

    for (i = 0; i < count; ++i) {
        const ProcessInfo *p = &processes[i];
        char buf[128];

        if (i > 0) {
            write_str(fd, ",");
        }

        write_str(fd, "{");
        snprintf(buf, sizeof(buf), "\"pid\":%d,\"name\":", p->pid);
        write_str(fd, buf);
        write_json_string(fd, p->name);
        snprintf(buf, sizeof(buf), ",\"cpu_usage\":%.2f,\"memory_kb\":%ld", p->cpu_usage, p->memory_kb);
        write_str(fd, buf);
        write_str(fd, "}");
    }

    write_str(fd, "]}");
    free(processes);
}

static int get_process_by_pid(DashboardState *state, pid_t pid, ProcessInfo *out) {
    size_t i;
    int found = 0;

    pthread_mutex_lock(&state->mutex);
    for (i = 0; i < state->count; ++i) {
        if (state->processes[i].pid == pid) {
            *out = state->processes[i];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&state->mutex);

    return found;
}

static void send_json_result(int fd, const char *status, const char *result, const char *message) {
    char body[256];

    snprintf(body,
             sizeof(body),
             "{\"result\":\"%s\",\"message\":\"%s\"}",
             result,
             message ? message : "");
    send_response(fd, status, "application/json; charset=utf-8", body);
}

static void handle_terminate(DashboardState *state, int client_fd, const char *path) {
    const char *pid_str = strstr(path, "pid=");
    char *end = NULL;
    long pid_val;
    ProcessInfo proc;

    if (!pid_str) {
        send_json_result(client_fd, "400 Bad Request", "error", "missing pid parameter");
        return;
    }

    pid_str += 4;
    pid_val = strtol(pid_str, &end, 10);
    if (pid_val <= 0 || (end && *end != '\0' && *end != '&')) {
        send_json_result(client_fd, "400 Bad Request", "error", "invalid pid value");
        return;
    }

    if (!get_process_by_pid(state, (pid_t)pid_val, &proc)) {
        send_json_result(client_fd, "404 Not Found", "error", "pid not present in current dashboard snapshot");
        return;
    }

    if (action_manual_terminate(&proc) == 0) {
        send_json_result(client_fd, "200 OK", "ok", "termination signal sent");
    } else {
        send_json_result(client_fd, "500 Internal Server Error", "error", "failed to terminate process");
    }
}

static void handle_client(DashboardState *state, int client_fd) {
    char request[REQUEST_BUF_SIZE];
    char method[8];
    char path[256];
    ssize_t n;

    n = recv(client_fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        return;
    }
    request[n] = '\0';

    method[0] = '\0';
    path[0] = '\0';
    (void)sscanf(request, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/state") == 0) {
        send_json(state, client_fd);
    } else if ((strcmp(method, "POST") == 0 || strcmp(method, "GET") == 0) &&
               strncmp(path, "/api/terminate?pid=", 19) == 0) {
        handle_terminate(state, client_fd, path);
    } else if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        send_html(client_fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
        send_response(client_fd, "204 No Content", "text/plain", "");
    } else {
        send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8", "Not found");
    }
}

static void *server_thread(void *arg) {
    DashboardState *state = (DashboardState *)arg;
    struct pollfd pfd;

    while (state->running) {
        int client_fd;

        pfd.fd = state->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 500) <= 0) {
            continue;
        }

        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        client_fd = accept(state->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        handle_client(state, client_fd);
        close(client_fd);
    }

    return NULL;
}

static void launch_browser(int port) {
    char url[64];
    pid_t pid = fork();

    if (pid != 0) {
        return;
    }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
    _exit(0);
}

int dashboard_init(DashboardState *state) {
    memset(state, 0, sizeof(*state));
    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        return -1;
    }

    state->port = DASHBOARD_PORT;
    state->listen_fd = -1;
    return 0;
}

void dashboard_cleanup(DashboardState *state) {
    free(state->processes);
    state->processes = NULL;
    state->count = 0;
    state->capacity = 0;
    pthread_mutex_destroy(&state->mutex);
}

int dashboard_start(DashboardState *state) {
    int opt = 1;
    struct sockaddr_in addr;

    state->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (state->listen_fd < 0) {
        return -1;
    }

    if (setsockopt(state->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(state->listen_fd);
        state->listen_fd = -1;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)state->port);

    if (bind(state->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(state->listen_fd);
        state->listen_fd = -1;
        return -1;
    }

    if (listen(state->listen_fd, DASHBOARD_BACKLOG) != 0) {
        close(state->listen_fd);
        state->listen_fd = -1;
        return -1;
    }

    state->running = 1;
    if (pthread_create(&state->thread, NULL, server_thread, state) != 0) {
        state->running = 0;
        close(state->listen_fd);
        state->listen_fd = -1;
        return -1;
    }

    launch_browser(state->port);
    return 0;
}

void dashboard_stop(DashboardState *state) {
    if (!state->running) {
        return;
    }

    state->running = 0;
    if (state->listen_fd >= 0) {
        shutdown(state->listen_fd, SHUT_RDWR);
        close(state->listen_fd);
        state->listen_fd = -1;
    }
    pthread_join(state->thread, NULL);
}

void dashboard_update(DashboardState *state, const ProcessInfo *list, size_t count) {
    ProcessInfo *copy = NULL;

    if (count > 0) {
        copy = (ProcessInfo *)malloc(count * sizeof(ProcessInfo));
        if (!copy) {
            return;
        }
        memcpy(copy, list, count * sizeof(ProcessInfo));
        qsort(copy, count, sizeof(ProcessInfo), cmp_cpu_desc);
    }

    pthread_mutex_lock(&state->mutex);
    if (ensure_capacity(state, count) != 0) {
        pthread_mutex_unlock(&state->mutex);
        free(copy);
        return;
    }

    if (count > 0) {
        memcpy(state->processes, copy, count * sizeof(ProcessInfo));
    }
    state->count = count;
    pthread_mutex_unlock(&state->mutex);

    free(copy);
}

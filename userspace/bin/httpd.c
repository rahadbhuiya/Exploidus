/*
 * httpd.c — Exploidus HTTP Server
 *
 * Listens on port 80, serves:
 *   GET /          → system status page
 *   GET /status    → JSON system info
 *   GET /audit     → last audit log entries
 *   GET /cnsl      → CNSL security alerts
 *   GET /proc      → process list
 *   GET /huddlecluster → HuddleCluster load balancer
 *
 * Single-threaded, handles one connection at a time.
 * Uses Exploidus capability-based socket API.
 */

#include "../libc/syscall.h"

#define HTTP_PORT    80
#define BUF_SIZE     2048
#define RESP_SIZE    4096

/*  Helpers  */
static void println(const char *s) { puts(s); putc('\n'); }

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static void uint_to_str(uint64_t n, char *buf) {
    if (n == 0) { buf[0]='0'; buf[1]=0; return; }
    char tmp[21]; int i=0;
    while (n) { tmp[i++]='0'+(n%10); n/=10; }
    int j=0;
    while (i--) buf[j++]=tmp[i];
    buf[j]=0;
}

static int append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max-1) buf[pos++] = *s++;
    buf[pos] = 0;
    return pos;
}

/*  HTML helpers  */
static int html_header(char *buf, int pos, const char *title) {
    pos = append(buf, pos, RESP_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>");
    pos = append(buf, pos, RESP_SIZE, title);
    pos = append(buf, pos, RESP_SIZE,
        "</title>"
        "<style>"
        "body{background:#0d1117;color:#c9d1d9;font-family:monospace;margin:0;padding:20px}"
        "h1{color:#58a6ff;border-bottom:1px solid #30363d;padding-bottom:10px}"
        "h2{color:#79c0ff;margin-top:20px}"
        ".box{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:15px;margin:10px 0}"
        ".ok{color:#3fb950}.warn{color:#f0883e}.err{color:#f85149}"
        ".badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:12px}"
        ".badge-ok{background:#1f6feb33;color:#58a6ff}"
        ".badge-warn{background:#f0883e33;color:#f0883e}"
        "table{width:100%;border-collapse:collapse}"
        "th{text-align:left;color:#8b949e;padding:4px 8px;border-bottom:1px solid #30363d}"
        "td{padding:4px 8px;border-bottom:1px solid #21262d}"
        "a{color:#58a6ff;text-decoration:none}"
        "a:hover{text-decoration:underline}"
        "nav{margin-bottom:20px}"
        "nav a{margin-right:15px;padding:6px 12px;background:#161b22;border:1px solid #30363d;border-radius:4px}"
        "</style></head><body>"
        "<h1>&#x1F512; Exploidus OS</h1>"
        "<nav>"
        "<a href='/'>Dashboard</a>"
        "<a href='/status'>Status</a>"
        "<a href='/proc'>Processes</a>"
        "<a href='/audit'>Audit Log</a>"
        "<a href='/cnsl'>Security</a>"
        "<a href='/huddlecluster'>Load Balancer</a>"
        "</nav>");
    return pos;
}

static int html_footer(char *buf, int pos) {
    pos = append(buf, pos, RESP_SIZE,
        "<div class='box' style='margin-top:30px;color:#8b949e;font-size:12px'>"
        "Exploidus v0.1.0 &mdash; Reactive Capability Kernel &mdash; "
        "<span class='ok'>&#x25CF;</span> System Online"
        "</div></body></html>");
    return pos;
}

/*  Route handlers  */

static int route_dashboard(char *buf) {
    int pos = 0;
    pos = html_header(buf, pos, "Exploidus Dashboard");

    /* System info box */
    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'>"
        "<h2>System Status</h2>"
        "<table>"
        "<tr><th>Component</th><th>Status</th></tr>"
        "<tr><td>Kernel</td><td><span class='badge badge-ok'>Running</span></td></tr>"
        "<tr><td>Intent Scheduler</td><td><span class='badge badge-ok'>Active</span></td></tr>"
        "<tr><td>Capability System</td><td><span class='badge badge-ok'>Active (BLAKE3)</span></td></tr>"
        "<tr><td>Audit Ring</td><td><span class='badge badge-ok'>Online</span></td></tr>"
        "<tr><td>CNSL</td><td><span class='badge badge-ok'>Monitoring</span></td></tr>"
        "<tr><td>FIM</td><td><span class='badge badge-ok'>Watching 13 paths</span></td></tr>"
        "<tr><td>Network</td><td><span class='badge badge-ok'>10.0.2.15</span></td></tr>"
        "<tr><td>Filesystem</td><td><span class='badge badge-ok'>ExFS mounted</span></td></tr>"
        "</table></div>");

    /* Uptime */
    uint64_t ticks = uptime();
    uint64_t secs = ticks / 100;
    uint64_t mins = secs / 60;
    uint64_t hrs  = mins / 60;
    secs %= 60; mins %= 60;

    pos = append(buf, pos, RESP_SIZE, "<div class='box'><h2>Uptime</h2><p>");
    char tmp[32];
    uint_to_str(hrs, tmp);
    pos = append(buf, pos, RESP_SIZE, tmp);
    pos = append(buf, pos, RESP_SIZE, "h ");
    uint_to_str(mins, tmp);
    pos = append(buf, pos, RESP_SIZE, tmp);
    pos = append(buf, pos, RESP_SIZE, "m ");
    uint_to_str(secs, tmp);
    pos = append(buf, pos, RESP_SIZE, tmp);
    pos = append(buf, pos, RESP_SIZE, "s</p></div>");

    /* Quick links */
    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'><h2>Quick Links</h2>"
        "<p><a href='/proc'>&#x1F4CB; Process List</a></p>"
        "<p><a href='/audit'>&#x1F4DC; Audit Log</a></p>"
        "<p><a href='/cnsl'>&#x1F6E1; Security Monitor</a></p>"
        "<p><a href='/status'>&#x1F4CA; System Stats (JSON)</a></p>"
        "</div>");

    pos = html_footer(buf, pos);
    return pos;
}

static int route_status(char *buf) {
    int pos = 0;

    uint64_t ticks = uptime();

    pos = append(buf, pos, RESP_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{");

    char tmp[32];
    pos = append(buf, pos, RESP_SIZE, "\"os\":\"Exploidus\",");
    pos = append(buf, pos, RESP_SIZE, "\"version\":\"0.1.0\",");
    pos = append(buf, pos, RESP_SIZE, "\"kernel\":\"Reactive-Capability-Kernel\",");
    pos = append(buf, pos, RESP_SIZE, "\"ip\":\"10.0.2.15\",");
    pos = append(buf, pos, RESP_SIZE, "\"uptime_ticks\":");
    uint_to_str(ticks, tmp);
    pos = append(buf, pos, RESP_SIZE, tmp);
    pos = append(buf, pos, RESP_SIZE, ",");
    pos = append(buf, pos, RESP_SIZE,
        "\"scheduler\":\"intent-based\","
        "\"security\":\"BLAKE3-capability\","
        "\"cnsl\":\"active\","
        "\"fim\":\"active\","
        "\"status\":\"nominal\""
        "}");

    return pos;
}

static int route_processes(char *buf) {
    int pos = 0;
    pos = html_header(buf, pos, "Exploidus Processes");

    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'><h2>Process List</h2>"
        "<table>"
        "<tr><th>PID</th><th>Parent</th><th>State</th><th>Intent</th><th>CPU Ticks</th></tr>");

    static proc_info_t procs[64];
    int64_t n = getprocs(procs, 64);

    static const char *states[] = {"UNUSED","READY","RUNNING","BLOCKED","ZOMBIE"};
    static const char *intents[] = {"AUDIT","INTERACTIVE","IO","NETWORK","COMPUTE"};

    if (n > 0) {
        for (int64_t i = 0; i < n; i++) {
            if (procs[i].state == 0) continue; /* skip UNUSED */
            pos = append(buf, pos, RESP_SIZE, "<tr><td>");
            char tmp[32];
            uint_to_str(procs[i].pid, tmp);
            pos = append(buf, pos, RESP_SIZE, tmp);
            pos = append(buf, pos, RESP_SIZE, "</td><td>");
            uint_to_str(procs[i].parent_pid, tmp);
            pos = append(buf, pos, RESP_SIZE, tmp);
            pos = append(buf, pos, RESP_SIZE, "</td><td>");
            uint32_t st = procs[i].state < 5 ? procs[i].state : 0;
            pos = append(buf, pos, RESP_SIZE, states[st]);
            pos = append(buf, pos, RESP_SIZE, "</td><td>");
            uint32_t in = procs[i].intent < 5 ? procs[i].intent : 0;
            pos = append(buf, pos, RESP_SIZE, intents[in]);
            pos = append(buf, pos, RESP_SIZE, "</td><td>");
            uint_to_str(procs[i].ticks_used, tmp);
            pos = append(buf, pos, RESP_SIZE, tmp);
            pos = append(buf, pos, RESP_SIZE, "</td></tr>");
        }
    }

    pos = append(buf, pos, RESP_SIZE, "</table></div>");
    pos = html_footer(buf, pos);
    return pos;
}

static int route_audit(char *buf) {
    int pos = 0;
    pos = html_header(buf, pos, "Exploidus Audit Log");

    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'><h2>Audit Ring</h2>"
        "<p class='warn'>Note: Requires CAP_RIGHT_AUDIT capability. "
        "Connect auditd for full log access.</p>"
        "<table>"
        "<tr><th>Event</th><th>Info</th></tr>"
        "<tr><td>Kernel Boot</td><td class='ok'>All subsystems nominal</td></tr>"
        "<tr><td>CNSL Init</td><td class='ok'>Correlation engine online</td></tr>"
        "<tr><td>FIM Init</td><td class='ok'>13 critical paths monitored</td></tr>"
        "<tr><td>Network</td><td class='ok'>e1000 ready, IP=10.0.2.15</td></tr>"
        "<tr><td>Filesystem</td><td class='ok'>ExFS mounted at /</td></tr>"
        "</table>"
        "<p style='color:#8b949e;font-size:12px'>Full audit log: /var/log/audit.log</p>"
        "</div>");

    pos = html_footer(buf, pos);
    return pos;
}

static int route_cnsl(char *buf) {
    int pos = 0;
    pos = html_header(buf, pos, "CNSL Security Monitor");

    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'><h2>Correlated Network Security Layer</h2>"
        "<table>"
        "<tr><th>Rule</th><th>Description</th><th>Status</th></tr>"
        "<tr><td>honeypot_then_ssh</td><td>Honeypot port + SSH = scanner</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "<tr><td>multi_service_brute_force</td><td>SSH + DB credential spray</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "<tr><td>web_recon_then_ssh</td><td>Web scan + SSH attack</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "<tr><td>privilege_escalation</td><td>SSH login + sudo fail</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "<tr><td>web_auth_flood</td><td>Web brute force</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "</table></div>"

        "<div class='box'><h2>File Integrity Monitor</h2>"
        "<table>"
        "<tr><th>Path</th><th>Severity</th></tr>"
        "<tr><td>/etc/passwd</td><td><span class='badge badge-warn'>CRITICAL</span></td></tr>"
        "<tr><td>/etc/shadow</td><td><span class='badge badge-warn'>CRITICAL</span></td></tr>"
        "<tr><td>/etc/sudoers</td><td><span class='badge badge-warn'>CRITICAL</span></td></tr>"
        "<tr><td>/root/.ssh/</td><td><span class='badge badge-warn'>CRITICAL</span></td></tr>"
        "<tr><td>/etc/crontab</td><td><span class='badge badge-ok'>HIGH</span></td></tr>"
        "<tr><td>/etc/hosts</td><td><span class='badge badge-ok'>HIGH</span></td></tr>"
        "</table></div>");

    pos = html_footer(buf, pos);
    return pos;
}

static int route_huddlecluster(char *buf) {
    int pos = 0;
    pos = html_header(buf, pos, "HuddleCluster Load Balancer");

    pos = append(buf, pos, RESP_SIZE,
        "<div class='box'><h2>&#x1F427; HuddleCluster — Penguin Load Balancer</h2>"
        "<p>Inspired by emperor penguin huddle behavior. "
        "Active servers rotate out when overheated, resting servers rotate in when cool.</p>"
        "<table>"
        "<tr><th>Concept</th><th>Description</th></tr>"
        "<tr><td>Inner Ring</td><td>Active servers handling requests (round-robin)</td></tr>"
        "<tr><td>Outer Ring</td><td>Resting servers cooling down (sorted by temperature)</td></tr>"
        "<tr><td>Temperature</td><td>EMA(cpu×0.10 + mem×0.05 + conns×0.10 + latency×0.70 + errors×0.05)</td></tr>"
        "<tr><td>Heat Threshold</td><td>55 — server rotates out to outer ring</td></tr>"
        "<tr><td>Cool Threshold</td><td>30 — server rotates in to inner ring</td></tr>"
        "</table></div>"

        "<div class='box'><h2>Cluster Status</h2>"
        "<table>"
        "<tr><th>Server</th><th>Ring</th><th>Temperature</th><th>Status</th></tr>"
        "<tr><td>exploidus-0</td><td>Inner</td><td>~15</td>"
        "<td><span class='badge badge-ok'>Active</span></td></tr>"
        "</table>"
        "<p style='color:#8b949e;font-size:12px'>"
        "Add backends via: hc_add_server(&amp;g_cluster, id, ip, port, weight)</p>"
        "</div>"

        "<div class='box'><h2>Algorithm</h2>"
        "<p>Reference: Zitterbart et al. (2011). "
        "\"Coordinated movements prevent jamming in an emperor penguin huddle.\" PLOS ONE.</p>"
        "</div>");

    pos = html_footer(buf, pos);
    return pos;
}

static int route_404(char *buf) {
    int pos = 0;
    pos = append(buf, pos, RESP_SIZE,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<h1>404 — Not Found</h1>"
        "<p><a href='/'>Back to dashboard</a></p>");
    return pos;
}

/*  Request parser  */
static void handle_request(cap_token_t cap, int conn_fd,
                            char *req_buf, char *resp_buf)
{
    /* Read request */
    int n = xrecv(cap, conn_fd, req_buf, BUF_SIZE - 1);
    if (n <= 0) return;
    req_buf[n] = 0;

    /* Parse method + path */
    int resp_len = 0;

    if (str_starts(req_buf, "GET / ") || str_starts(req_buf, "GET /\r")) {
        resp_len = route_dashboard(resp_buf);
    } else if (str_starts(req_buf, "GET /status")) {
        resp_len = route_status(resp_buf);
    } else if (str_starts(req_buf, "GET /proc")) {
        resp_len = route_processes(resp_buf);
    } else if (str_starts(req_buf, "GET /audit")) {
        resp_len = route_audit(resp_buf);
    } else if (str_starts(req_buf, "GET /cnsl")) {
        resp_len = route_cnsl(resp_buf);
    } else if (str_starts(req_buf, "GET /huddlecluster")) {
        resp_len = route_huddlecluster(resp_buf);
    } else {
        resp_len = route_404(resp_buf);
    }

    /* Send response in chunks */
    int sent = 0;
    while (sent < resp_len) {
        int chunk = resp_len - sent;
        if (chunk > 1400) chunk = 1400;
        int r = xsend(cap, conn_fd, resp_buf + sent, (uint16_t)chunk);
        if (r <= 0) break;
        sent += r;
    }
}

/*  Main  */
int main(void)
{
    println("=== Exploidus HTTP Server ===");
    println("Listening on port 80...");

    cap_token_t null_cap = {0, 0};

    /* Create TCP socket */
    int srv = xsocket(null_cap, SOCK_TCP);
    if (srv < 0) {
        println("[HTTPD] Error: socket failed");
        exit(1);
    }

    /* Bind to port 80 */
    if (xbind(null_cap, srv, HTTP_PORT) < 0) {
        println("[HTTPD] Error: bind failed");
        exit(1);
    }

    /* Listen */
    if (xlisten(null_cap, srv) < 0) {
        println("[HTTPD] Error: listen failed");
        exit(1);
    }

    println("[HTTPD] Ready. Visit http://10.0.2.15/");

    static char req_buf[BUF_SIZE];
    static char resp_buf[RESP_SIZE];

    /* Accept loop */
    while (1) {
        int conn = xaccept(null_cap, srv);
        if (conn < 0) {
            sleep_ticks(5);
            continue;
        }

        handle_request(null_cap, conn, req_buf, resp_buf);
        xclose(conn);
    }

    return 0;
}

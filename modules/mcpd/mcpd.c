#include "../../common/ubiqos_abi.h"

// mcpd -- the board, as tools an AI agent can call.
//
//   mcpd                answer on port 8080, to the USB cable only
//   mcpd 9000           another port
//   mcpd -a             answer on every interface, WiFi included
//
// The Model Context Protocol is JSON-RPC over HTTP POST: an agent asks which
// tools there are, and calls one by name. An MCP server is only ever asked, so
// nothing here runs unless somebody calls, and a board with no agent in the
// room is not waiting for one.
//
// A TOOL IS A PROGRAM THAT ALREADY EXISTS. Each one names a module and its
// arguments; calling it starts that module with a pipe on its descriptors, the
// way netcon starts the shell, and what it printed is the answer. So `ps` over
// MCP cannot differ from `ps` at the prompt, and adding a tool is adding a line.
//
// WHICH PROGRAMS IS THE POINT. The list below is read-only, and /sd/mcp.txt
// replaces it on a board that wants another. This is not the shell: there is
// no sh in the path, so a pipe or a redirection in an argument is just text,
// and an agent can reach exactly what the list names.
//
// WHAT IT IS NOT: encrypted, or authenticated. That is why the default is the
// cable, as for netcon, and why `-a` is a decision. A tool that changes
// anything does not belong on the list of a server open to the WiFi.
//
// Both eras of the protocol are served on one endpoint. 2026-07-28 made it
// stateless, each request carrying its version; up to 2025-11-25 a client opens
// with `initialize`. Answering initialize and issuing no session is all the
// second one takes, since nothing here remembers anything between requests.

UBIQOS_MEM_SIZE(12288);

#define DEFAULT_PORT  8080
#define NET_WAIT_S    30
#define USB_MASK      0xFFFFFF00u

#define REQ_MAX       4096u    // headers and body together
#define OUT_MAX       8192u    // what one tool may say; the rest is dropped
#define TOOLS_MAX     2048u    // the size of /sd/mcp.txt
#define MAX_TOOLS     24u
#define ARG_MAX       128u
#define TX_MAX        512u
#define TOOL_MS       5000u    // a tool that has not finished by then is killed
#define READ_MS       3000u    // how long a client may take to send its request
#define CLOSE_WAIT_MS 250u      // how long to leave the first close to the client

#define MODERN        "2026-07-28"
#define LEGACY_LATEST "2025-11-25"
#define TOOLS_PATH    "/sd/mcp.txt"
#define SERVER_NAME   "ubiqos-mcpd"

static const char *const VERSIONS[] = { MODERN, LEGACY_LATEST, "2025-06-18", "2025-03-26" };
#define NVERSIONS 4u

// name: command: what to tell the agent
//
// A star in the command is where the one argument goes. Reads of the card are
// held under /sd, and the file system itself refuses the files with secrets in.
static const char DEFAULT_TOOLS[] =
    "ps: ps: The processes running: pid, priority, state, memory, name\n"
    "free: free: Free memory, pipe rings, sockets and how many processes are alive\n"
    "uptime: uptime: How long the board has been up, and since when\n"
    "date: date: The wall clock\n"
    "uname: uname -a: The system's name and version\n"
    "lsmod: lsmod: The modules loaded and how many processes use each\n"
    "sockstat: sockstat: The sockets in use\n"
    "usbstat: usbstat: The USB counters\n"
    "dmesg: cat /var/dmesg: The kernel log, oldest first\n"
    "sensors: cat /tmp/sensors.json: The HibouAir sensors' last readings, as JSON\n"
    "list_files: ls /sd/*: List a directory on the SD card; args is its path under /sd, empty for the top\n"
    "read_file: cat /sd/*: Print a file from the SD card; args is its path under /sd\n";

typedef struct {
    const char *name;
    const char *command;
    const char *about;
} tool_t;

typedef struct {
    const char *p;
    uint32_t n;
} span_t;

static tool_t tools[MAX_TOOLS];
static uint32_t ntools;
static char *request_buf;
static char *out_buf;
static char hostname[32];
static char version[32];

static uint32_t len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

static uint32_t to_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static bool span_is(span_t s, const char *text) {
    uint32_t i = 0;
    for (; i < s.n; i++)
        if (!text[i] || text[i] != s.p[i]) return false;
    return !text[i];
}

static bool span_eq(span_t a, span_t b) {
    if (a.n != b.n) return false;
    for (uint32_t i = 0; i < a.n; i++)
        if (a.p[i] != b.p[i]) return false;
    return true;
}

// A module has no C library, so no memcpy, and a plain loop in the open is one
// gcc turns back into a call to it. See netcon's shift_down.
static __attribute__((noinline)) void copy(char *to, const char *from, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) to[i] = from[i];
}

// Volatile for the same reason: an ordinary loop here becomes a call to memset,
// and so does `= { 0 }` on a struct this size.
static __attribute__((noinline)) void clear(void *to, uint32_t n) {
    volatile char *p = to;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

// --- THE TOOL LIST ----------------------------------------------------------

static char *trim(char *s, char *end) {
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = 0;
    return s;
}

// Cuts the text up where it lies: the table points into it. A line that does
// not have three fields, or that begins with #, is passed over.
static void parse_tools(char *t) {
    ntools = 0;
    while (*t && ntools < MAX_TOOLS) {
        char *eol = t;
        while (*eol && *eol != '\n') eol++;
        const bool last = !*eol;
        char *bar1 = t;
        while (bar1 < eol && *bar1 != ':') bar1++;
        char *bar2 = bar1 < eol ? bar1 + 1 : eol;
        while (bar2 < eol && *bar2 != ':') bar2++;
        if (*t != '#' && bar2 < eol) {
            tools[ntools].name = trim(t, bar1);
            tools[ntools].command = trim(bar1 + 1, bar2);
            tools[ntools].about = trim(bar2 + 1, eol);
            if (tools[ntools].name[0] && tools[ntools].command[0]) ntools++;
        }
        if (last) break;
        t = eol + 1;
    }
}

// The card's list if there is one, and otherwise the one compiled in. The
// compiled one is copied first because parsing writes into what it reads.
static void load_tools(char *space) {
    const int32_t fd = ubiqos_open(TOOLS_PATH);
    int32_t n = 0;
    if (fd >= 0) {
        n = ubiqos_read(fd, space, TOOLS_MAX - 1);
        ubiqos_close(fd);
    }
    if (n <= 0) {
        n = (int32_t)sizeof DEFAULT_TOOLS - 1;
        copy(space, DEFAULT_TOOLS, (uint32_t)n);
    }
    space[n] = 0;
    parse_tools(space);
}

// Is a program of this name to be had? Flash first, then the same search the
// loader makes along the path, as the name and as the name with .mod after it.
// Only asking: loading it to see would leave it in memory.
static bool on_path(const char *dir, uint32_t dir_n, const char *name, const char *suffix) {
    char file[UBIQOS_PATH_MAX + 32];
    const uint32_t name_n = len(name), suffix_n = len(suffix);
    if (dir_n + 1 + name_n + suffix_n + 1 > sizeof file) return false;
    copy(file, dir, dir_n);
    file[dir_n] = '/';
    copy(file + dir_n + 1, name, name_n);
    copy(file + dir_n + 1 + name_n, suffix, suffix_n + 1);
    uint32_t size;
    const int32_t attr = ubiqos_fs_stat(file, &size);
    return attr >= 0 && !(attr & 0x10);          // 0x10: it is a directory
}

static bool program_exists(const char *name) {
    ubiqos_modinfo_t m;
    for (uint32_t i = 0; ubiqos_moddir_get(i, &m) == 0; i++)
        if (is(m.name, name)) return true;

    char path[UBIQOS_PATH_MAX];
    if (ubiqos_path_get(path, sizeof path) < 0) return false;
    for (const char *dir = path; *dir; ) {
        uint32_t n = 0;
        while (dir[n] && dir[n] != ':') n++;
        if (n && (on_path(dir, n, name, "") || on_path(dir, n, name, ".mod"))) return true;
        dir += n;
        if (*dir == ':') dir++;
    }
    return false;
}

// The first word of a command, which is the program.
static const char *first_word(const char *command, char *word, uint32_t cap) {
    uint32_t n = 0;
    while (*command && *command != ' ') {
        if (n + 1 >= cap) return 0;
        word[n++] = *command++;
    }
    word[n] = 0;
    return command;
}

// A tool that could only ever answer that it does not exist is no use to an
// agent, and it would be told about it anyway. The list is written for a Fruit
// Jam and the NUCLEO has no usbstat. Said at the start, or a line in /sd/mcp.txt
// with a mistake in it would simply be gone.
static void drop_missing(void) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ntools; i++) {
        char program[16];
        if (first_word(tools[i].command, program, sizeof program) && program_exists(program)) {
            tools[kept++] = tools[i];
            continue;
        }
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "mcpd: no tool ");
        ubiqos_line_str(&l, tools[i].name);
        ubiqos_line_str(&l, ": this board has no such program\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
    ntools = kept;
}

static const tool_t *find_tool(const char *name) {
    for (uint32_t i = 0; i < ntools; i++)
        if (is(tools[i].name, name)) return &tools[i];
    return 0;
}

static bool takes_argument(const tool_t *t) {
    for (const char *c = t->command; *c; c++)
        if (*c == '*') return true;
    return false;
}

// --- JSON, READING ----------------------------------------------------------
//
// Nothing is built from it: a request is walked where it lies, and the few
// values wanted are found by key. Every step checks the end of the buffer.

static const char *skip_space(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *skip_string(const char *p, const char *end) {
    for (p++; p < end; p++) {
        if (*p == '\\') p++;
        else if (*p == '"') return p + 1;
    }
    return 0;
}

// Past one value, or 0 if it does not end. Brackets are counted, not matched in
// kind: this has to find where a value stops, not to judge it.
static const char *skip_value(const char *p, const char *end) {
    if (p >= end) return 0;
    if (*p == '"') return skip_string(p, end);
    if (*p != '{' && *p != '[') {
        const char *q = p;
        while (q < end && *q != ',' && *q != '}' && *q != ']' &&
               *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
        return q > p ? q : 0;
    }
    uint32_t depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (!p) return 0;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p + 1;
        }
        p++;
    }
    return 0;
}

static bool get(span_t object, const char *key, span_t *value) {
    const char *end = object.p + object.n;
    const char *p = skip_space(object.p, end);
    if (p >= end || *p != '{') return false;
    p = skip_space(p + 1, end);
    while (p < end && *p == '"') {
        const char *q = skip_string(p, end);
        if (!q) return false;
        const span_t name = { p + 1, (uint32_t)(q - p) - 2 };
        p = skip_space(q, end);
        if (p >= end || *p != ':') return false;
        p = skip_space(p + 1, end);
        q = skip_value(p, end);
        if (!q) return false;
        if (span_is(name, key)) {
            value->p = p;
            value->n = (uint32_t)(q - p);
            return true;
        }
        p = skip_space(q, end);
        if (p >= end || *p != ',') return false;
        p = skip_space(p + 1, end);
    }
    return false;
}

static int32_t hex4(const char *p, const char *end) {
    if (end - p < 4) return -1;
    int32_t v = 0;
    for (uint32_t i = 0; i < 4; i++) {
        const char c = p[i];
        const int32_t d = c >= '0' && c <= '9' ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

// A JSON string into plain text. False for anything else, for one that does not
// fit, and for an embedded NUL, which would quietly end the text early.
static bool get_text(span_t value, char *out, uint32_t cap) {
    if (value.n < 2 || value.p[0] != '"') return false;
    const char *p = value.p + 1;
    const char *end = value.p + value.n - 1;
    uint32_t n = 0;
    while (p < end) {
        if (n + 4 >= cap) return false;
        char c = *p++;
        if (c != '\\') { out[n++] = c; continue; }
        if (p >= end) return false;
        c = *p++;
        if (c == 'u') {
            const int32_t v = hex4(p, end);
            if (v <= 0) return false;
            p += 4;
            if (v < 0x80) {
                out[n++] = (char)v;
            } else if (v < 0x800) {
                out[n++] = (char)(0xC0 | (v >> 6));
                out[n++] = (char)(0x80 | (v & 0x3F));
            } else {
                out[n++] = (char)(0xE0 | (v >> 12));
                out[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (v & 0x3F));
            }
            continue;
        }
        out[n++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' :
                   c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    out[n] = 0;
    return true;
}

// --- JSON, WRITING ----------------------------------------------------------
//
// Content-Length goes out before the body and a tool's output can be anything,
// so every answer is produced twice by the same code: once to count it and once
// to send it. That needs no buffer as large as the answer.

typedef struct {
    int32_t sock;
    bool sending;
    bool gone;
    uint32_t count;
    uint32_t fill;
    uint8_t tx[TX_MAX];
} sink_t;

// ZERO IS NOT AN ERROR: lwIP answers it when its send buffer is full. See httpd.
static bool send_all(int32_t sock, const uint8_t *p, uint32_t n) {
    uint32_t stalled = 0;
    for (uint32_t done = 0; done < n; ) {
        const int32_t sent = ubiqos_sock_send(sock, p + done, n - done);
        if (sent < 0) return false;
        if (sent == 0) {
            if (++stalled > 1000) return false;
            ubiqos_sleep(1);
            continue;
        }
        stalled = 0;
        done += (uint32_t)sent;
    }
    return true;
}

static void flush(sink_t *s) {
    if (s->fill && !s->gone && !send_all(s->sock, s->tx, s->fill)) s->gone = true;
    s->fill = 0;
}

static void put_bytes(sink_t *s, const char *p, uint32_t n) {
    s->count += n;
    if (!s->sending) return;
    while (n) {
        uint32_t k = TX_MAX - s->fill;
        if (k > n) k = n;
        copy((char *)s->tx + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == TX_MAX) flush(s);
    }
}

static void put(sink_t *s, const char *text) { put_bytes(s, text, len(text)); }

static void put_u32(sink_t *s, uint32_t v) {
    char t[11];
    uint32_t i = sizeof t;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put_bytes(s, t + i, (uint32_t)sizeof t - i);
}

// How many bytes of well-formed UTF-8 start here: 0 if none do. A tool can print
// anything and JSON has to be UTF-8, and cutting the output off can split a
// character.
static uint32_t utf8_at(const uint8_t *p, uint32_t n) {
    const uint8_t c = p[0];
    const uint32_t need = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c < 0xF0 ? 3
                        : c >= 0xC2 && c < 0xE0 ? 2 : 0;
    if (!need || need > n) return 0;
    for (uint32_t i = 1; i < need; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return need;
}

static void put_escaped(sink_t *s, const char *text, uint32_t n) {
    static const char HEX[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)text;
    for (uint32_t i = 0; i < n; ) {
        const uint8_t c = p[i];
        if (c == '"' || c == '\\') {
            const char e[2] = { '\\', (char)c };
            put_bytes(s, e, 2);
            i++;
        } else if (c == '\n') {
            put(s, "\\n");
            i++;
        } else if (c == '\r') {
            put(s, "\\r");
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            const char e[6] = { '\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15] };
            put_bytes(s, e, 6);
            i++;
        } else if (c < 0x80) {
            put_bytes(s, (const char *)p + i, 1);
            i++;
        } else {
            const uint32_t k = utf8_at(p + i, n - i);
            if (k) put_bytes(s, (const char *)p + i, k);
            else put(s, "?");
            i += k ? k : 1;
        }
    }
}

static void put_text(sink_t *s, const char *text) { put_escaped(s, text, len(text)); }

// --- ONE ANSWER -------------------------------------------------------------

typedef enum {
    KIND_RESULT_EMPTY,
    KIND_INITIALIZE,
    KIND_DISCOVER,
    KIND_TOOLS,
    KIND_CALL,
    KIND_ERROR,
    KIND_UNSUPPORTED
} kind_t;

typedef struct {
    kind_t kind;
    span_t id;                 // as it came; empty when there is none to echo
    const char *status;
    int32_t code;              // KIND_ERROR, KIND_UNSUPPORTED
    const char *message;
    span_t requested;          // KIND_UNSUPPORTED
    const char *agreed;        // KIND_INITIALIZE
    uint32_t out_n;            // KIND_CALL
    bool truncated;
    bool timed_out;
    bool failed;
} answer_t;

static void put_server_meta(sink_t *s) {
    put(s, "\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":\"" SERVER_NAME
           "\",\"version\":\"");
    put_text(s, version);
    put(s, "\"}}");
}

static void put_versions(sink_t *s) {
    put(s, "[");
    for (uint32_t i = 0; i < NVERSIONS; i++) {
        if (i) put(s, ",");
        put(s, "\"");
        put(s, VERSIONS[i]);
        put(s, "\"");
    }
    put(s, "]");
}

static void put_instructions(sink_t *s) {
    put(s, "\"instructions\":\"The UbiqOS board ");
    put_text(s, hostname);
    put(s, ". Each tool runs one of the board's own programs and returns what it "
           "printed.\"");
}

static void put_tools(sink_t *s) {
    put(s, "\"tools\":[");
    for (uint32_t i = 0; i < ntools; i++) {
        if (i) put(s, ",");
        put(s, "{\"name\":\"");
        put_text(s, tools[i].name);
        put(s, "\",\"description\":\"");
        put_text(s, tools[i].about);
        put(s, "\",\"inputSchema\":{\"type\":\"object\"");
        if (takes_argument(&tools[i]))
            put(s, ",\"properties\":{\"args\":{\"type\":\"string\","
                   "\"description\":\"One word, no spaces\"}}");
        put(s, "}}");
    }
    put(s, "]");
}

// The body, to a sink that is counting or to one that is sending.
static void render(sink_t *s, const answer_t *a) {
    put(s, "{\"jsonrpc\":\"2.0\"");
    if (a->id.n) {
        put(s, ",\"id\":");
        put_bytes(s, a->id.p, a->id.n);
    }
    if (a->kind == KIND_ERROR || a->kind == KIND_UNSUPPORTED) {
        put(s, ",\"error\":{\"code\":-");
        put_u32(s, (uint32_t)-a->code);
        put(s, ",\"message\":\"");
        put_text(s, a->message);
        put(s, "\"");
        if (a->kind == KIND_UNSUPPORTED) {
            put(s, ",\"data\":{\"supported\":");
            put_versions(s);
            put(s, ",\"requested\":\"");
            put_escaped(s, a->requested.p, a->requested.n);
            put(s, "\"}");
        }
        put(s, "}}");
        return;
    }

    put(s, ",\"result\":{\"resultType\":\"complete\",");
    switch (a->kind) {
    case KIND_INITIALIZE:
        put(s, "\"protocolVersion\":\"");
        put(s, a->agreed);
        put(s, "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" SERVER_NAME
               "\",\"version\":\"");
        put_text(s, version);
        put(s, "\"},");
        put_instructions(s);
        put(s, ",");
        break;
    case KIND_DISCOVER:
        put(s, "\"supportedVersions\":");
        put_versions(s);
        put(s, ",\"capabilities\":{\"tools\":{}},");
        put_instructions(s);
        put(s, ",\"ttlMs\":60000,\"cacheScope\":\"private\",");
        break;
    case KIND_TOOLS:
        put_tools(s);
        put(s, ",\"ttlMs\":60000,\"cacheScope\":\"private\",");
        break;
    case KIND_CALL:
        put(s, "\"content\":[{\"type\":\"text\",\"text\":\"");
        put_escaped(s, out_buf, a->out_n);
        if (a->truncated) put(s, "\\n[output cut off here]");
        if (a->timed_out) put(s, "\\n[stopped: it did not finish in time]");
        put(s, "\"}],\"isError\":");
        put(s, a->failed ? "true," : "false,");
        break;
    default:
        break;
    }
    put_server_meta(s);
    put(s, "}}");
}

static void send_head(int32_t sock, const char *status, uint32_t length, bool json) {
    char head[160];
    uint32_t n = 0;
    const char *parts[] = { "HTTP/1.1 ", status,
                            json ? "\r\nContent-Type: application/json" : "",
                            "\r\nConnection: close\r\nContent-Length: " };
    for (uint32_t p = 0; p < 4; p++) {
        const uint32_t k = len(parts[p]);
        copy(head + n, parts[p], k);
        n += k;
    }
    char t[11];
    uint32_t i = sizeof t;
    do { t[--i] = (char)('0' + length % 10); length /= 10; } while (length);
    copy(head + n, t + i, (uint32_t)sizeof t - i);
    n += (uint32_t)sizeof t - i;
    copy(head + n, "\r\n\r\n", 4);
    send_all(sock, (const uint8_t *)head, n + 4);
}

// Every connection carries one request and closes. A second request on a kept
// connection would save little, and this server has one thread: a client that
// sat on an idle connection would shut every other client out until it timed
// out.
static void respond(int32_t sock, const answer_t *a) {
    static sink_t s;
    s.sock = sock;
    s.sending = false;
    s.gone = false;
    s.count = 0;
    s.fill = 0;
    render(&s, a);
    send_head(sock, a->status, s.count, true);
    s.sending = true;
    render(&s, a);
    flush(&s);
}

static void refuse(int32_t sock, answer_t *a, const char *status, int32_t code, const char *why) {
    a->kind = KIND_ERROR;
    a->status = status;
    a->code = code;
    a->message = why;
    respond(sock, a);
}

// --- RUNNING A TOOL ---------------------------------------------------------

static bool child_alive(int32_t pid) {
    for (uint32_t s = 0; s < UBIQOS_PS_SLOTS; s++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(s, &p) != 0) continue;
        if ((int32_t)p.pid == pid) return p.state != UBIQOS_PS_ZOMBIE;
    }
    return false;
}

// Starts it on the far end of a two-way pipe, as netcon does for the shell.
// Past OUT_MAX the output is still read, or the program would stop on a full
// pipe and be killed for it.
static void run(const char *module, const char *args, answer_t *a) {
    int32_t pair[2];
    if (ubiqos_pipepair(pair) < 0) {
        a->failed = true;
        return;
    }
    const int32_t s0 = ubiqos_dup(UBIQOS_STDIN, -1);
    const int32_t s1 = ubiqos_dup(UBIQOS_STDOUT, -1);
    const int32_t s2 = ubiqos_dup(UBIQOS_STDERR, -1);
    ubiqos_dup(pair[1], UBIQOS_STDIN);
    ubiqos_dup(pair[1], UBIQOS_STDOUT);
    ubiqos_dup(pair[1], UBIQOS_STDERR);
    const int32_t pid = ubiqos_exec(module, args);
    ubiqos_dup(s0, UBIQOS_STDIN);
    ubiqos_dup(s1, UBIQOS_STDOUT);
    ubiqos_dup(s2, UBIQOS_STDERR);
    ubiqos_close(s0);
    ubiqos_close(s1);
    ubiqos_close(s2);
    ubiqos_close(pair[1]);

    if (pid < 0) {
        ubiqos_close(pair[0]);
        a->failed = true;
        const char *no = pid == UBIQOS_EXEC_NOT_FOUND ? "this board has no such program"
                                                      : "the program could not be started";
        a->out_n = len(no);
        copy(out_buf, no, a->out_n);
        return;
    }

    static char discard[128];
    uint32_t waited = 0;
    for (;;) {
        const bool alive = child_alive(pid);
        bool moved = false;
        while (ubiqos_readable(pair[0]) > 0) {
            const uint32_t room = OUT_MAX - a->out_n;
            const int32_t n = room ? ubiqos_read(pair[0], out_buf + a->out_n, room)
                                   : ubiqos_read(pair[0], discard, sizeof discard);
            if (n <= 0) break;
            if (room) a->out_n += (uint32_t)n;
            else a->truncated = true;
            moved = true;
        }
        if (!alive) break;          // and only after a last look at the pipe
        if (moved) continue;
        ubiqos_sleep(2);
        waited += 2;
        if (waited >= TOOL_MS) {
            a->failed = true;
            a->timed_out = true;
            break;
        }
    }
    ubiqos_hangup(pair[0]);
    ubiqos_close(pair[0]);
    ubiqos_wait(pid);
}

// Fills in the command line: the module, and its arguments with the caller's
// word where the star is. The word cannot leave the place it was put: a space
// would make it two arguments and a .. would climb out of /sd.
static bool build_command(const char *template, const char *word,
                          char *module, uint32_t module_cap, char *args, uint32_t args_cap) {
    for (const char *w = word; *w; w++) {
        if (*w == ' ' || *w == '\t' || *w == '\n' || *w == '\r') return false;
        if (w[0] == '.' && w[1] == '.') return false;
    }
    const char *p = first_word(template, module, module_cap);
    if (!p) return false;
    while (*p == ' ') p++;
    uint32_t n = 0;
    for (; *p; p++) {
        if (*p != '*') {
            if (n + 1 >= args_cap) return false;
            args[n++] = *p;
            continue;
        }
        const uint32_t k = len(word);
        if (n + k + 1 >= args_cap) return false;
        copy(args + n, word, k);
        n += k;
    }
    args[n] = 0;
    return true;
}

static void call_tool(int32_t sock, span_t params, answer_t *a) {
    char name[32];
    span_t v;
    if (!get(params, "name", &v) || !get_text(v, name, sizeof name)) {
        refuse(sock, a, "200 OK", -32602, "params.name is missing");
        return;
    }
    const tool_t *tool = find_tool(name);
    if (!tool) {
        refuse(sock, a, "200 OK", -32602, "no such tool");
        return;
    }
    char word[ARG_MAX];
    word[0] = 0;
    span_t arguments;
    if (get(params, "arguments", &arguments) && get(arguments, "args", &v) &&
        !get_text(v, word, sizeof word)) {
        refuse(sock, a, "200 OK", -32602, "args must be a short string");
        return;
    }
    char module[16];
    char args[ARG_MAX + 32];
    if (!build_command(tool->command, word, module, sizeof module, args, sizeof args)) {
        refuse(sock, a, "200 OK", -32602, "args must be one word, with no .. in it");
        return;
    }
    a->kind = KIND_CALL;
    a->status = "200 OK";
    run(module, args, a);
    respond(sock, a);
}

// --- THE PROTOCOL -----------------------------------------------------------

typedef struct {
    span_t version;            // MCP-Protocol-Version
    span_t method;             // Mcp-Method
    span_t name;               // Mcp-Name
    bool origin;
    bool chunked;
    uint32_t body;             // Content-Length
    bool has_length;
} head_t;

static bool name_is(span_t name, const char *lower) {
    uint32_t i = 0;
    for (; i < name.n; i++) {
        char c = name.p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (!lower[i] || c != lower[i]) return false;
    }
    return !lower[i];
}

static void parse_headers(const char *p, const char *end, head_t *h) {
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\r' && *eol != '\n') eol++;
        const char *colon = p;
        while (colon < eol && *colon != ':') colon++;
        if (colon < eol) {
            const span_t name = { p, (uint32_t)(colon - p) };
            const char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *ve = eol;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            const span_t value = { v, (uint32_t)(ve - v) };
            if (name_is(name, "content-length")) {
                h->body = 0;
                for (uint32_t i = 0; i < value.n; i++) {
                    const char c = value.p[i];
                    if (c < '0' || c > '9' || h->body > REQ_MAX) { h->body = REQ_MAX + 1; break; }
                    h->body = h->body * 10 + (uint32_t)(c - '0');
                }
                h->has_length = true;
            } else if (name_is(name, "origin")) {
                h->origin = true;
            } else if (name_is(name, "transfer-encoding")) {
                h->chunked = true;
            } else if (name_is(name, "mcp-protocol-version")) {
                h->version = value;
            } else if (name_is(name, "mcp-method")) {
                h->method = value;
            } else if (name_is(name, "mcp-name")) {
                h->name = value;
            }
        }
        p = eol;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
}

static bool supported(span_t v) {
    for (uint32_t i = 0; i < NVERSIONS; i++)
        if (span_is(v, VERSIONS[i])) return true;
    return false;
}

// One JSON-RPC message. Anything wrong with the transport has been answered
// before this.
static void handle(int32_t sock, const head_t *h, span_t body) {
    answer_t a;
    clear(&a, sizeof a);
    a.status = "200 OK";
    span_t v = { 0, 0 };

    const char *start = skip_space(body.p, body.p + body.n);
    const char *stop = skip_value(start, body.p + body.n);
    if (!stop || *start != '{' || skip_space(stop, body.p + body.n) != body.p + body.n) {
        refuse(sock, &a, "400 Bad Request", -32700, "not a JSON object");
        return;
    }

    span_t id = { 0, 0 };
    const bool has_id = get(body, "id", &id);
    if (has_id && (id.p[0] == '"' || id.p[0] == '-' || (id.p[0] >= '0' && id.p[0] <= '9')))
        a.id = id;
    else if (has_id && !span_is(id, "null"))
        id.n = 0;

    span_t method;
    if (!get(body, "method", &method) || method.n < 2 || method.p[0] != '"') {
        refuse(sock, &a, "400 Bad Request", -32600, "no method");
        return;
    }
    method.p++;
    method.n -= 2;

    // Which era is asking. A request with no version header can only be an old
    // client, and the specification allows taking it for 2025-03-26.
    const bool has_version = h->version.n != 0;
    if (has_version && !supported(h->version)) {
        a.kind = KIND_UNSUPPORTED;
        a.status = "400 Bad Request";
        a.code = -32022;
        a.message = "Unsupported protocol version";
        a.requested = h->version;
        respond(sock, &a);
        return;
    }
    const bool modern = has_version && span_is(h->version, MODERN);

    span_t params = { "{}", 2 };
    get(body, "params", &params);

    if (modern) {
        // What the headers say has to be what the body says, so nothing in
        // front of this can route on one and have the other carried out. A
        // base64 Mcp-Name is not decoded: tool names are plain words, so it
        // could only be a name that fails either way.
        span_t name;
        span_t meta;
        bool same = span_eq(h->method, method);
        if (same && get(params, "_meta", &meta) &&
            get(meta, "io.modelcontextprotocol/protocolVersion", &v))
            same = v.n >= 2 && v.p[0] == '"' &&
                   span_eq((span_t){ v.p + 1, v.n - 2 }, h->version);
        else
            same = false;
        if (same && span_is(method, "tools/call"))
            same = get(params, "name", &name) && name.n >= 2 && name.p[0] == '"' &&
                   span_eq((span_t){ name.p + 1, name.n - 2 }, h->name);
        if (!same) {
            refuse(sock, &a, "400 Bad Request", -32020, "Header mismatch");
            return;
        }
    }

    if (!has_id) {                              // a notification
        send_head(sock, "202 Accepted", 0, false);
        return;
    }
    if (!id.n) {
        refuse(sock, &a, "400 Bad Request", -32600, "the id must be a string or a number");
        return;
    }

    if (span_is(method, "initialize")) {
        a.kind = KIND_INITIALIZE;
        a.agreed = LEGACY_LATEST;
        char wanted[16];
        if (get(params, "protocolVersion", &v) && get_text(v, wanted, sizeof wanted))
            for (uint32_t i = 1; i < NVERSIONS; i++)
                if (is(wanted, VERSIONS[i])) a.agreed = VERSIONS[i];
        respond(sock, &a);
    } else if (span_is(method, "server/discover")) {
        a.kind = KIND_DISCOVER;
        respond(sock, &a);
    } else if (span_is(method, "tools/list")) {
        a.kind = KIND_TOOLS;
        respond(sock, &a);
    } else if (span_is(method, "tools/call")) {
        call_tool(sock, params, &a);
    } else if (span_is(method, "ping") && !modern) {
        a.kind = KIND_RESULT_EMPTY;
        respond(sock, &a);
    } else {
        refuse(sock, &a, modern ? "404 Not Found" : "200 OK", -32601, "Method not found");
    }
}

// --- HTTP -------------------------------------------------------------------

static void plain(int32_t sock, const char *status) { send_head(sock, status, 0, false); }

// Reads until the whole request is here. A zero from receive means nothing has
// arrived yet, not that the client has gone.
static void serve(int32_t sock) {
    char *b = request_buf;
    uint32_t have = 0;
    uint32_t idle = 0;
    int32_t head_end = -1;
    head_t h;
    clear(&h, sizeof h);

    for (;;) {
        if (head_end < 0) {
            for (uint32_t i = 0; i + 3 < have; i++)
                if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
                    head_end = (int32_t)i + 4;
                    break;
                }
            if (head_end >= 0) {
                const char *eol = b;
                while (*eol != '\r') eol++;
                parse_headers(eol, b + head_end, &h);
            }
        }
        if (head_end >= 0 && (h.chunked || have >= (uint32_t)head_end + h.body)) break;
        if (head_end >= 0 && h.body > REQ_MAX - (uint32_t)head_end) break;
        if (have == REQ_MAX) break;

        const int32_t got = ubiqos_sock_recv(sock, (uint8_t *)b + have, REQ_MAX - have);
        if (got < 0) return;
        if (got == 0) {
            if (++idle >= READ_MS / 5) return;
            ubiqos_sleep(5);
            continue;
        }
        idle = 0;
        have += (uint32_t)got;
    }

    if (head_end < 0) {
        plain(sock, "431 Request Header Fields Too Large");
        return;
    }
    const bool post = have > 5 && b[0] == 'P' && b[1] == 'O' && b[2] == 'S' && b[3] == 'T' && b[4] == ' ';
    const char *path = b;
    while (*path != ' ' && path < b + head_end) path++;
    while (*path == ' ' && path < b + head_end) path++;
    const bool at_mcp = path[0] == '/' && path[1] == 'm' && path[2] == 'c' && path[3] == 'p' &&
                        (path[4] == ' ' || path[4] == '?');

    answer_t a;
    clear(&a, sizeof a);
    if (!at_mcp) {
        plain(sock, "404 Not Found");
    } else if (h.origin) {
        // A browser is the only thing that sends this, and a page it is showing
        // must not reach the board by way of a name that resolves here. Agents
        // do not run in pages, so no origin is a good one.
        refuse(sock, &a, "403 Forbidden", -32000, "requests from web pages are refused");
    } else if (!post) {
        plain(sock, "405 Method Not Allowed");
    } else if (h.chunked || !h.has_length) {
        plain(sock, "411 Length Required");
    } else if (h.body > REQ_MAX - (uint32_t)head_end) {
        plain(sock, "413 Content Too Large");
    } else {
        const span_t body = { b + head_end, h.body };
        handle(sock, &h, body);
    }
}

// Whoever closes first holds the port in TIME_WAIT for two minutes, and lwIP has
// no SO_REUSE here, so with this server closing first `kill mcpd` could not be
// followed by `mcpd`: the listen failed until the last connection had aged out.
// Every answer says Connection: close, so the client is about to go anyway.
static void let_client_close_first(int32_t sock) {
    for (uint32_t i = 0; i < CLOSE_WAIT_MS / 5; i++) {
        if (ubiqos_sock_state(sock) != UBIQOS_TCP_ESTABLISHED) return;
        ubiqos_sleep(5);
    }
}

// --- STARTING -------------------------------------------------------------

// The cable's subnet. See netcon, whose rule this is.
static uint32_t cable_subnet(void) {
    char t[20];
    if (ubiqos_config_get(UBIQOS_CFG_USB_ADDRESS, t, sizeof t) <= 0) return 0;
    uint32_t a = 0, part = 0, dots = 0;
    for (const char *p = t; ; p++) {
        if (*p >= '0' && *p <= '9') { part = part * 10u + (uint32_t)(*p - '0'); continue; }
        if (part > 255u) return 0;
        a = (a << 8) | part;
        part = 0;
        if (*p != '.') break;
        dots++;
    }
    return dots == 3 ? (a & USB_MASK) : 0;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: mcpd [port] [-a]\n\n"
            "The board's programs as tools for an AI agent, over the Model\n"
            "Context Protocol: POST to http://<board>:8080/mcp\n\n"
            "  port   which port to answer on; 8080 by default\n"
            "  -a     answer on every interface. Without it only the USB\n"
            "         cable's own subnet is let in\n\n"
            "The tools are a read-only list built in. /sd/mcp.txt replaces it,\n"
            "one tool to a line:\n\n"
            "  name: command: what to tell the agent\n\n"
            "A * in the command takes one word from the caller. Nothing is\n"
            "encrypted and nothing asks for a password, so put nothing there\n"
            "that changes the machine if the server is open with -a.\n"
            "Run it with 'mcpd &' and stop it with 'kill mcpd'.\n"))
        return;

    uint32_t port = DEFAULT_PORT;
    bool anywhere = false;
    for (int i = 1; i < argc; i++) {
        if (is(argv[i], "-a")) anywhere = true;
        else if (to_u32(argv[i])) port = to_u32(argv[i]);
    }

    char *space = ubiqos_alloc_bulk(REQ_MAX + OUT_MAX + TOOLS_MAX);
    if (!space) {
        ubiqos_write_str(UBIQOS_STDOUT, "mcpd: no memory\r\n");
        return;
    }
    request_buf = space;
    out_buf = space + REQ_MAX;
    load_tools(space + REQ_MAX + OUT_MAX);
    drop_missing();

    if (ubiqos_config_get(UBIQOS_CFG_HOSTNAME, hostname, sizeof hostname) <= 0)
        copy(hostname, "ubiqos", 7);
    if (ubiqos_config_get(UBIQOS_CFG_VERSION, version, sizeof version) <= 0)
        copy(version, "0", 2);

    // The stack comes up later than a program started from /sd/startup.
    int32_t server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    for (uint32_t waited = 0; server < 0 && waited < NET_WAIT_S; waited++) {
        ubiqos_sleep(1000);
        server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    }
    if (server < 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "mcpd: the network stack would not take the port\r\n");
        return;
    }

    const uint32_t subnet = cable_subnet();
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "mcpd: ");
    ubiqos_line_u32(&l, ntools);
    ubiqos_line_str(&l, " tools on port ");
    ubiqos_line_u32(&l, port);
    ubiqos_line_str(&l, anywhere ? ", to anybody who asks\r\n" : ", to the USB cable only\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    for (;;) {
        const int32_t c = ubiqos_sock_accept(server);
        if (c < 0) {
            ubiqos_sleep(20);
            continue;
        }
        uint32_t ip = 0;
        ubiqos_sock_peer(c, &ip);
        if (!anywhere && (!subnet || (ip & USB_MASK) != subnet)) {
            plain(c, "403 Forbidden");
        } else {
            serve(c);
        }
        let_client_close_first(c);
        ubiqos_sock_close(c);
    }
}

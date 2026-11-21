#include <stdlib.h>
#include <string.h>
#include "../../common/ubiqos_abi.h"
#include "ubiqos_tls.h"

// agent -- talk to Claude, which can look around this board and change it.
//
//   agent                 a conversation; /help inside it says the rest
//
// The board asks api.anthropic.com itself, over TLS with the certificate
// checked: there is no tunnel and no other machine in it. The API key is the
// key store's "api.anthropic", entered once with `key set api.anthropic` and
// handed to this program alone -- see UBIQOS_KEY_OP_TAKE for what that does
// and does not protect.
//
// Claude has six tools: list a directory, read a file, write one, change one
// in place, run one of the board's programs, and look at the screen of
// what it ran. Before a write, an edit or a run it is said here what is
// about to happen and asked; "a" at that question stops the asking for the
// rest of the conversation. There is no language on the board yet, so it
// cannot write a program -- what it can do is what a person at this keyboard
// can do with the programs that are here.
//
// The conversation lives in memory, as the JSON the API is sent each turn,
// and goes when the program does. Each answer streams in as it is written.

UBIQOS_MEM_SIZE(32768);                 // Mbed TLS's big numbers are on the stack
// TLS (about 80 kB) and the conversation, which grows a turn at a time: a
// megabyte is a long session of reading files. PSRAM on the Fruit Jam.
uint32_t ubiqos_heap_bytes = 1024u * 1024u;

#define API_HOST     "api.anthropic.com"
#define MODEL        "claude-opus-5-5"
#define MAX_TOKENS   "16000"
#define KEY_NAME     "api.anthropic"
#define IDLE_MS      120000u    // nothing from the server for this long ends a turn
#define TOOL_MS      60000u     // a program that runs longer is stopped
#define OUT_MAX      16384u     // of a program's output, kept for Claude
#define READ_MAX     32768u     // of a file, per read
#define LINE_MAX     2048u
#define CONVO_MAX    (768u * 1024u)

// --- output -----------------------------------------------------------------

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static void say_n(const char *s, uint32_t n) { ubiqos_write(UBIQOS_STDOUT, s, n); }

// Claude's text has bare newlines; the terminal wants a carriage return too.
static void say_text(const char *s, uint32_t n) {
    uint32_t from = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] != '\n') continue;
        if (i > from) say_n(s + from, i - from);
        say("\r\n");
        from = i + 1;
    }
    if (n > from) say_n(s + from, n - from);
}

static void say_u32(uint32_t v) {
    char b[12]; int k = 11; b[k] = 0;
    do { b[--k] = (char)('0' + v % 10u); v /= 10u; } while (v);
    say(&b[k]);
}

// --- a growing buffer -------------------------------------------------------

typedef struct { char *p; uint32_t n, cap; bool failed; } buf_t;

static void put_n(buf_t *b, const char *s, uint32_t n) {
    if (b->failed) return;
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = b->cap ? b->cap : 1024u;
        while (b->n + n + 1 > cap) cap *= 2u;
        char *q = realloc(b->p, cap);
        if (!q) { b->failed = true; return; }
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void put(buf_t *b, const char *s) { put_n(b, s, (uint32_t)strlen(s)); }

static void put_u32(buf_t *b, uint32_t v) {
    char t[12]; int k = 11; t[k] = 0;
    do { t[--k] = (char)('0' + v % 10u); v /= 10u; } while (v);
    put(b, &t[k]);
}

static void buf_free(buf_t *b) { free(b->p); memset(b, 0, sizeof *b); }

// A JSON string, quotes and all. Bytes that are not UTF-8 -- a binary file
// read as text -- become '?', because the API refuses the whole request over
// one of them.
static void put_json_str(buf_t *b, const char *s, uint32_t n) {
    put(b, "\"");
    uint32_t i = 0;
    while (i < n) {
        const uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; put_n(b, e, 2); i++; continue; }
        if (c == '\n') { put(b, "\\n"); i++; continue; }
        if (c == '\r') { put(b, "\\r"); i++; continue; }
        if (c == '\t') { put(b, "\\t"); i++; continue; }
        if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            put_n(b, e, 6); i++; continue;
        }
        if (c < 0x80) { put_n(b, (const char *)&s[i], 1); i++; continue; }
        uint32_t len = (c & 0xE0) == 0xC0 ? 2u : (c & 0xF0) == 0xE0 ? 3u : (c & 0xF8) == 0xF0 ? 4u : 0u;
        bool ok = len && i + len <= n;
        for (uint32_t k = 1; ok && k < len; k++) ok = ((uint8_t)s[i + k] & 0xC0) == 0x80;
        if (ok) { put_n(b, &s[i], len); i += len; }
        else    { put(b, "?"); i++; }
    }
    put(b, "\"");
}

// --- reading JSON -----------------------------------------------------------
//
// Enough to find a key in an object and take a string or a number out: the
// answers are read in place, never built into a tree.

static const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

// Past one value, or NULL if it does not end before e.
static const char *skip(const char *p, const char *e) {
    p = ws(p, e);
    if (p >= e) return 0;
    if (*p == '"') {
        for (p++; p < e; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return 0;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') { p = skip(p, e); if (!p) return 0; p--; continue; }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return 0;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

// The value of key in the object at obj, or NULL.
static const char *field(const char *obj, const char *e, const char *key) {
    const char *p = ws(obj, e);
    if (p >= e || *p != '{') return 0;
    p++;
    const uint32_t kl = (uint32_t)strlen(key);
    for (;;) {
        p = ws(p, e);
        if (p >= e || *p == '}') return 0;
        if (*p != '"') return 0;
        const char *k = p + 1;
        const char *kend = skip(p, e);
        if (!kend) return 0;
        const bool hit = (uint32_t)(kend - 1 - k) == kl && !memcmp(k, key, kl);
        p = ws(kend, e);
        if (p >= e || *p != ':') return 0;
        p = ws(p + 1, e);
        if (hit) return p;
        p = skip(p, e);
        if (!p) return 0;
        p = ws(p, e);
        if (p < e && *p == ',') p++;
    }
}

static void put_utf8(buf_t *b, uint32_t cp) {
    char u[4]; uint32_t n;
    if (cp < 0x80)       { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63));
        u[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63));
        u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    put_n(b, u, n);
}

static uint32_t hex4(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v = v * 16u + (uint32_t)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

// A JSON string's contents, unescaped, appended to out. false if v is not one.
static bool str_into(const char *v, const char *e, buf_t *out) {
    if (!v || v >= e || *v != '"') return false;
    const char *end = skip(v, e);
    if (!end) return false;
    for (const char *p = v + 1; p < end - 1; p++) {
        if (*p != '\\') { put_n(out, p, 1); continue; }
        p++;
        switch (*p) {
        case 'n': put(out, "\n"); break;
        case 'r': put(out, "\r"); break;
        case 't': put(out, "\t"); break;
        case 'b': put(out, "\b"); break;
        case 'f': put(out, "\f"); break;
        case 'u': {
            if (p + 4 >= end) return true;
            uint32_t cp = hex4(p + 1);
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < end && p[1] == '\\' && p[2] == 'u') {
                const uint32_t lo = hex4(p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            put_utf8(out, cp);
            break;
        }
        default: put_n(out, p, 1); break;   // \" \\ \/
        }
    }
    return true;
}

static bool str_is(const char *v, const char *e, const char *want) {
    if (!v || v >= e || *v != '"') return false;
    const uint32_t n = (uint32_t)strlen(want);
    return v + n + 2 <= e && !memcmp(v + 1, want, n) && v[n + 1] == '"';
}

static uint32_t num(const char *v, const char *e) {
    uint32_t n = 0;
    while (v && v < e && *v >= '0' && *v <= '9') n = n * 10u + (uint32_t)(*v++ - '0');
    return n;
}

// --- the keyboard -----------------------------------------------------------

static bool read_char(uint8_t *ch) {
    for (;;) {
        const int32_t n = ubiqos_read(UBIQOS_STDIN, ch, 1);
        if (n < 0) return false;
        if (n == 1) return true;
    }
}

// A line, echoed, with backspace. false on Ctrl-C or Ctrl-D at the start.
static bool read_line(char *out, uint32_t cap) {
    uint32_t n = 0;
    ubiqos_esc_t esc = { 0 };
    for (;;) {
        uint8_t ch;
        if (!read_char(&ch)) return false;
        if (ubiqos_esc_skip(&esc, ch)) continue;
        if (ch == '\r' || ch == '\n') { say("\r\n"); out[n] = 0; return true; }
        if (ch == 3) { say("^C\r\n"); return false; }
        if (ch == 4 && !n) { say("\r\n"); return false; }
        if (ch == 8 || ch == 127) {
            if (!n) continue;
            // Back over one character, which may be several bytes of UTF-8.
            do n--; while (n && ((uint8_t)out[n] & 0xC0) == 0x80);
            say("\b \b");
            continue;
        }
        if ((ch >= ' ' || ch >= 0x80) && n + 1 < cap) { out[n++] = (char)ch; say_n((const char *)&ch, 1); }
    }
}

static bool ask_all;                    // "a" was answered: stop asking

// May Claude do this? Said first, then y, n or a.
static bool allowed(const char *what, const char *detail, uint32_t dn) {
    say("\r\n\x1b[1m"); say(what); say("\x1b[0m ");
    say_text(detail, dn > 400 ? 400 : dn);
    if (dn > 400) { say(" ... ("); say_u32(dn); say(" bytes)"); }
    if (ask_all) { say("\r\n"); return true; }
    say("\r\n  allow? [y]es, [n]o, [a]ll from now on: ");
    for (;;) {
        uint8_t ch;
        if (!read_char(&ch)) return false;
        if (ch == 'y' || ch == 'Y') { say("yes\r\n"); return true; }
        if (ch == 'a' || ch == 'A') { say("all\r\n"); ask_all = true; return true; }
        if (ch == 'n' || ch == 'N' || ch == 3 || ch == '\r') { say("no\r\n"); return false; }
    }
}

// --- the tools --------------------------------------------------------------

static const char *const tools_json =
    "["
    "{\"name\":\"list_dir\",\"description\":\"List a directory on the board: the output of its "
    "own ls. Paths start with a volume, /sd for the microSD card.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}},"
    "{\"name\":\"read_file\",\"description\":\"Read a file as text, up to 32 kB from offset "
    "(default 0). Says if there is more.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"write_file\",\"description\":\"Create or replace a file with the given text. "
    "The person at the board is asked first.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}},"
    "{\"name\":\"edit_file\",\"description\":\"Replace old_text, which must occur exactly once, "
    "with new_text in a file. The person at the board is asked first.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"old_text\",\"new_text\"]}},"
    "{\"name\":\"run\",\"description\":\"Run one of the board's programs with arguments and "
    "return what it printed (16 kB at most; stopped after 60 s). There is no shell between: "
    "no pipes, redirection, & or ; -- one program and its words. A program that waits for "
    "keyboard input gets end of file. The person at the board is asked first.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}"
    "]";

typedef struct { const char *p, *e; } span_t;

static bool child_alive(int32_t pid) {
    for (uint32_t s = 0; s < UBIQOS_PS_SLOTS; s++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(s, &p) != 0) continue;
        if ((int32_t)p.pid == pid) return p.state != UBIQOS_PS_ZOMBIE;
    }
    return false;
}

// One program on the far end of a two-way pipe, as mcpd runs its tools.
static void run_program(const char *line, buf_t *out) {
    char module[UBIQOS_NAME_LEN + 1];
    while (*line == ' ') line++;
    uint32_t k = 0;
    while (line[k] && line[k] != ' ' && k < UBIQOS_NAME_LEN) { module[k] = line[k]; k++; }
    module[k] = 0;
    const char *args = line + k;
    while (*args == ' ') args++;
    if (!k) { put(out, "no program named"); return; }

    int32_t pair[2];
    if (ubiqos_pipepair(pair) < 0) { put(out, "could not make a pipe"); return; }
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
    ubiqos_close(s0); ubiqos_close(s1); ubiqos_close(s2);
    ubiqos_close(pair[1]);
    if (pid < 0) {
        ubiqos_close(pair[0]);
        put(out, pid == UBIQOS_EXEC_NOT_FOUND ? "this board has no such program"
                                              : "the program could not be started");
        return;
    }

    static char chunk[256];
    uint32_t waited = 0, kept = 0;
    bool cut = false, slow = false;
    for (;;) {
        const bool alive = child_alive(pid);
        bool moved = false;
        while (ubiqos_readable(pair[0]) > 0) {
            const int32_t n = ubiqos_read(pair[0], chunk, sizeof chunk);
            if (n <= 0) break;
            const uint32_t room = OUT_MAX - kept;
            const uint32_t take = (uint32_t)n < room ? (uint32_t)n : room;
            if (take) { put_n(out, chunk, take); kept += take; }
            if (take < (uint32_t)n) cut = true;
            moved = true;
        }
        if (!alive) break;
        if (moved) continue;
        ubiqos_sleep(5);
        if ((waited += 5) >= TOOL_MS) { slow = true; break; }
    }
    ubiqos_hangup(pair[0]);
    ubiqos_close(pair[0]);
    if (slow) ubiqos_kill(pid);
    ubiqos_wait(pid);
    if (cut)  put(out, "\n[output cut at 16 kB]");
    if (slow) put(out, "\n[stopped after 60 s]");
    if (!out->n) put(out, "[no output]");
}

static bool read_whole(const char *path, buf_t *out, uint32_t from, uint32_t max, bool *more) {
    const int32_t fd = ubiqos_open_flags(path, UBIQOS_O_RDONLY);
    if (fd < 0) return false;
    static char chunk[1024];
    uint32_t at = 0;
    if (more) *more = false;
    for (;;) {
        const int32_t n = ubiqos_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (int32_t i = 0; i < n; i++, at++) {
            if (at < from) continue;
            if (out->n >= max) { if (more) *more = true; break; }
            put_n(out, &chunk[i], 1);
        }
        if (more && *more) break;
    }
    ubiqos_close(fd);
    return !out->failed;
}

static bool write_whole(const char *path, const char *s, uint32_t n) {
    const int32_t fd = ubiqos_open_flags(path, UBIQOS_O_WRONLY | UBIQOS_O_CREAT | UBIQOS_O_TRUNC);
    if (fd < 0) return false;
    uint32_t done = 0;
    while (done < n) {
        const int32_t w = ubiqos_write(fd, s + done, n - done > 512 ? 512 : n - done);
        if (w <= 0) break;
        done += (uint32_t)w;
    }
    ubiqos_close(fd);
    return done == n;
}

// One tool call: input is its JSON object. The answer goes to out, and
// is_error says whether it is a failure.
static void do_tool(const char *name, const char *in, const char *ie, buf_t *out, bool *is_error) {
    buf_t path = { 0 };
    *is_error = false;
    str_into(field(in, ie, "path"), ie, &path);

    if (!strcmp(name, "list_dir")) {
        char line[160] = "ls ";
        strncat(line, path.p ? path.p : "/", sizeof line - 5);
        say("\r\n\x1b[2m[ls "); say(path.p ? path.p : "/"); say("]\x1b[0m\r\n");
        run_program(line, out);
    } else if (!strcmp(name, "read_file")) {
        const uint32_t from = num(field(in, ie, "offset"), ie);
        say("\r\n\x1b[2m[read "); say(path.p ? path.p : "?"); say("]\x1b[0m\r\n");
        bool more = false;
        if (!path.p || !read_whole(path.p, out, from, READ_MAX, &more)) {
            *is_error = true; out->n = 0; put(out, "could not read that file");
        } else if (more) {
            put(out, "\n[more follows; read again from offset ");
            put_u32(out, from + READ_MAX); put(out, "]");
        } else if (!out->n) put(out, "[empty file]");
    } else if (!strcmp(name, "write_file")) {
        buf_t text = { 0 };
        str_into(field(in, ie, "content"), ie, &text);
        if (!path.p) { *is_error = true; put(out, "no path"); }
        else if (!allowed("write", path.p, (uint32_t)strlen(path.p))) {
            *is_error = true; put(out, "the person at the board said no");
        } else if (!write_whole(path.p, text.p ? text.p : "", text.n)) {
            *is_error = true; put(out, "the write failed");
        } else { put(out, "written, "); put_u32(out, text.n); put(out, " bytes"); }
        buf_free(&text);
    } else if (!strcmp(name, "edit_file")) {
        buf_t old = { 0 }, neu = { 0 }, file = { 0 };
        str_into(field(in, ie, "old_text"), ie, &old);
        str_into(field(in, ie, "new_text"), ie, &neu);
        char *hit = 0;
        if (!path.p || !old.n || !read_whole(path.p, &file, 0, 1024u * 1024u, 0)) {
            *is_error = true; put(out, "could not read that file");
        } else if (!(hit = strstr(file.p ? file.p : "", old.p)) || strstr(hit + 1, old.p)) {
            *is_error = true; put(out, hit ? "old_text occurs more than once" : "old_text is not in the file");
        } else if (!allowed("edit", path.p, (uint32_t)strlen(path.p))) {
            *is_error = true; put(out, "the person at the board said no");
        } else {
            buf_t res = { 0 };
            put_n(&res, file.p, (uint32_t)(hit - file.p));
            put_n(&res, neu.p ? neu.p : "", neu.n);
            put(&res, hit + old.n);
            if (!write_whole(path.p, res.p, res.n)) { *is_error = true; put(out, "the write failed"); }
            else put(out, "edited");
            buf_free(&res);
        }
        buf_free(&old); buf_free(&neu); buf_free(&file);
    } else if (!strcmp(name, "run")) {
        buf_t cmd = { 0 };
        str_into(field(in, ie, "command"), ie, &cmd);
        if (!cmd.p) { *is_error = true; put(out, "no command"); }
        else if (!allowed("run", cmd.p, cmd.n)) { *is_error = true; put(out, "the person at the board said no"); }
        else run_program(cmd.p, out);
        buf_free(&cmd);
    } else {
        *is_error = true;
        put(out, "no such tool");
    }
    buf_free(&path);
}

// --- the conversation -------------------------------------------------------

static buf_t convo;                     // the messages, comma-separated, no brackets
static char api_key[UBIQOS_KEY_VALUE_MAX + 1];
static buf_t system_prompt;

static void add_message(const char *role, const char *content_json, uint32_t n) {
    if (convo.n) put(&convo, ",");
    put(&convo, "{\"role\":\""); put(&convo, role); put(&convo, "\",\"content\":");
    put_n(&convo, content_json, n);
    put(&convo, "}");
}

// What Claude is told about where it is. The program names are this board's
// own, read from the module directory, so it does not have to guess them.
static void make_system_prompt(void) {
    buf_t s = { 0 };
    put(&s,
        "You are running on a microcontroller board, an Adafruit Fruit Jam (RP2350, "
        "Cortex-M33 at 150 MHz, 520 kB SRAM, 8 MB PSRAM), under UbiqOS: a small "
        "multitasking OS in the spirit of OS-9, written by the person at the keyboard, Ulf. "
        "You talk to him on an 80-column text console, so keep answers short and plain: no "
        "tables, little markdown. He writes in Swedish or English; answer in his language.\n\n"
        "You act on the board with your tools. Programs are modules, run by name, and most "
        "answer -h or --help with their usage. Paths name a volume first: /sd is the "
        "FAT32 microSD card. Files named config.txt, wificfg.txt and unlock.key hold "
        "secrets and are refused to every program; do not try to read them. The startup "
        "file /sd/startup runs line by line at boot. There is no programming language on "
        "the board yet, so you cannot write and run code; you can read and write files, "
        "and run the programs that are here.\n\n"
        "The person is asked before each write, edit or run, and may say no; take that "
        "as an answer, not an obstacle. Do not reboot the board, erase or format "
        "anything, change WiFi settings, or touch the key store unless he asks.\n\n"
        "The programs on this board: ");
    ubiqos_modinfo_t m;
    bool first = true;
    for (uint32_t i = 0; ubiqos_moddir_get(i, &m) == 0; i++) {
        if (m.type != UBIQOS_TYPE_PROGRAM) continue;
        if (!first) put(&s, ", ");
        put(&s, m.name);
        first = false;
    }
    put(&s, ".");
    put_json_str(&system_prompt, s.p, s.n);
    buf_free(&s);
}

// --- HTTP and the stream ----------------------------------------------------

typedef struct {
    // The answer as it is rebuilt, block by block, for the next request.
    buf_t content;                      // a JSON array's insides
    bool  have_block;
    char  type[24];
    buf_t text, json, sig, start;       // the current block's parts
    char  stop[24];
    uint32_t in_tok, out_tok, cache_read, cache_write;
    bool  said_text;
    buf_t error;
} turn_t;

static void block_start(turn_t *t, const char *d, const char *e) {
    const char *cb = field(d, e, "content_block");
    if (!cb) return;
    const char *end = skip(cb, e);
    t->have_block = true;
    t->type[0] = 0;
    buf_t ty = { 0 };
    str_into(field(cb, end, "type"), end, &ty);
    if (ty.p) strncpy(t->type, ty.p, sizeof t->type - 1);
    buf_free(&ty);
    t->text.n = t->json.n = t->sig.n = t->start.n = 0;
    put_n(&t->start, cb, (uint32_t)(end - cb));
    if (!strcmp(t->type, "text")) {
        if (!t->said_text) { say("\r\n"); t->said_text = true; }
    } else if (!strcmp(t->type, "tool_use")) {
        // The name and id are in the start; the input comes in pieces.
    }
}

static void block_delta(turn_t *t, const char *d, const char *e) {
    const char *dl = field(d, e, "delta");
    if (!dl) return;
    const char *end = skip(dl, e);
    const char *ty = field(dl, end, "type");
    if (str_is(ty, end, "text_delta")) {
        const uint32_t before = t->text.n;
        str_into(field(dl, end, "text"), end, &t->text);
        if (t->text.p) say_text(t->text.p + before, t->text.n - before);
    } else if (str_is(ty, end, "input_json_delta")) {
        str_into(field(dl, end, "partial_json"), end, &t->json);
    } else if (str_is(ty, end, "thinking_delta")) {
        str_into(field(dl, end, "thinking"), end, &t->text);
    } else if (str_is(ty, end, "signature_delta")) {
        str_into(field(dl, end, "signature"), end, &t->sig);
    }
}

// The finished block, as the API wants it back.
static void block_stop(turn_t *t) {
    if (!t->have_block) return;
    t->have_block = false;
    buf_t *c = &t->content;
    if (c->n) put(c, ",");
    if (!strcmp(t->type, "text")) {
        put(c, "{\"type\":\"text\",\"text\":");
        put_json_str(c, t->text.p ? t->text.p : "", t->text.n);
        put(c, "}");
    } else if (!strcmp(t->type, "tool_use")) {
        const char *s = t->start.p, *se = s + t->start.n;
        put(c, "{\"type\":\"tool_use\",\"id\":");
        const char *id = field(s, se, "id"), *nm = field(s, se, "name");
        put_n(c, id, (uint32_t)(skip(id, se) - id));
        put(c, ",\"name\":");
        put_n(c, nm, (uint32_t)(skip(nm, se) - nm));
        put(c, ",\"input\":");
        if (t->json.n) put_n(c, t->json.p, t->json.n); else put(c, "{}");
        put(c, "}");
    } else if (!strcmp(t->type, "thinking")) {
        // Passed back exactly as it came, signature and all.
        put(c, "{\"type\":\"thinking\",\"thinking\":");
        put_json_str(c, t->text.p ? t->text.p : "", t->text.n);
        put(c, ",\"signature\":");
        put_json_str(c, t->sig.p ? t->sig.p : "", t->sig.n);
        put(c, "}");
    } else {
        // redacted_thinking and anything newer: whole in its start event.
        put_n(c, t->start.p, t->start.n);
    }
}

static void on_event(turn_t *t, const char *d, uint32_t n) {
    const char *e = d + n;
    const char *ty = field(d, e, "type");
    if (str_is(ty, e, "content_block_start")) block_start(t, d, e);
    else if (str_is(ty, e, "content_block_delta")) block_delta(t, d, e);
    else if (str_is(ty, e, "content_block_stop")) block_stop(t);
    else if (str_is(ty, e, "message_start")) {
        const char *m = field(d, e, "message");
        const char *me = m ? skip(m, e) : 0;
        const char *u = m ? field(m, me, "usage") : 0;
        const char *ue = u ? skip(u, me) : 0;
        if (u) {
            t->in_tok = num(field(u, ue, "input_tokens"), ue);
            t->cache_read = num(field(u, ue, "cache_read_input_tokens"), ue);
            t->cache_write = num(field(u, ue, "cache_creation_input_tokens"), ue);
        }
    } else if (str_is(ty, e, "message_delta")) {
        const char *dl = field(d, e, "delta");
        const char *de = dl ? skip(dl, e) : 0;
        buf_t s = { 0 };
        if (dl) str_into(field(dl, de, "stop_reason"), de, &s);
        if (s.p) strncpy(t->stop, s.p, sizeof t->stop - 1);
        buf_free(&s);
        const char *u = field(d, e, "usage");
        if (u) t->out_tok = num(field(u, skip(u, e), "output_tokens"), skip(u, e));
    } else if (str_is(ty, e, "error")) {
        const char *er = field(d, e, "error");
        if (er) str_into(field(er, skip(er, e), "message"), skip(er, e), &t->error);
        if (!t->error.n) put(&t->error, "the server reported an error");
    }
}

// Feeds the body: undoes chunked encoding if the server used it, and hands
// each "data:" line of the event stream to on_event.
typedef struct {
    bool chunked;
    uint32_t chunk_left;                // bytes of the current chunk still to come
    int  chunk_state;                   // 0 size line, 1 data, 2 CRLF after data
    char size_line[16]; uint32_t size_n;
    buf_t line;
} body_t;

static void sse_byte(turn_t *t, body_t *b, char c) {
    if (c == '\r') return;
    if (c != '\n') { put_n(&b->line, &c, 1); return; }
    if (b->line.n > 5 && !memcmp(b->line.p, "data:", 5)) {
        const char *d = b->line.p + 5;
        while (*d == ' ') d++;
        on_event(t, d, (uint32_t)(b->line.p + b->line.n - d));
    }
    b->line.n = 0;
}

static void body_bytes(turn_t *t, body_t *b, const char *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const char c = p[i];
        if (!b->chunked) { sse_byte(t, b, c); continue; }
        if (b->chunk_state == 0) {
            if (c == '\n') {
                b->size_line[b->size_n] = 0;
                b->chunk_left = (uint32_t)strtoul(b->size_line, 0, 16);
                b->size_n = 0;
                b->chunk_state = b->chunk_left ? 1 : 3;
            } else if (c != '\r' && b->size_n < sizeof b->size_line - 1) b->size_line[b->size_n++] = c;
        } else if (b->chunk_state == 1) {
            sse_byte(t, b, c);
            if (--b->chunk_left == 0) b->chunk_state = 2;
        } else if (b->chunk_state == 2) {
            if (c == '\n') b->chunk_state = 0;
        }
    }
}

// One request: the whole conversation so far, and the answer streamed in.
// Returns the HTTP status, or 0 when the connection itself failed.
static uint32_t ask_claude(turn_t *t) {
    buf_t body = { 0 };
    put(&body, "{\"model\":\"" MODEL "\",\"max_tokens\":" MAX_TOKENS ",\"stream\":true,"
               "\"cache_control\":{\"type\":\"ephemeral\"},\"system\":");
    put_n(&body, system_prompt.p, system_prompt.n);
    put(&body, ",\"tools\":"); put(&body, tools_json);
    put(&body, ",\"messages\":["); put_n(&body, convo.p, convo.n); put(&body, "]}");
    if (body.failed) { buf_free(&body); put(&t->error, "out of memory for the request"); return 0; }

    buf_t head = { 0 };
    put(&head, "POST /v1/messages HTTP/1.1\r\nHost: " API_HOST "\r\n"
               "User-Agent: UbiqOS-agent\r\nanthropic-version: 2023-06-01\r\n"
               "content-type: application/json\r\nAccept: text/event-stream\r\n"
               "Connection: close\r\nx-api-key: ");
    put(&head, api_key);
    put(&head, "\r\ncontent-length: "); put_u32(&head, body.n); put(&head, "\r\n\r\n");

    char why[160];
    ubiqos_tls_t *tls = ubiqos_tls_open(API_HOST, 443, why, sizeof why);
    if (!tls) {
        memset(head.p, 0, head.n); buf_free(&head); buf_free(&body);
        put(&t->error, why);
        return 0;
    }
    bool sent = ubiqos_tls_write(tls, head.p, head.n) >= 0;
    memset(head.p, 0, head.n);
    buf_free(&head);
    for (uint32_t at = 0; sent && at < body.n; at += 4096u)
        sent = ubiqos_tls_write(tls, body.p + at, body.n - at > 4096u ? 4096u : body.n - at) >= 0;
    buf_free(&body);
    if (!sent) { ubiqos_tls_close(tls); put(&t->error, "the request could not be sent"); return 0; }

    // The headers, then the stream.
    static char rx[2048];
    buf_t hdr = { 0 };
    body_t bd = { 0 };
    bool in_body = false;
    uint32_t status = 0;
    buf_t errbody = { 0 };
    for (;;) {
        const int32_t n = ubiqos_tls_read(tls, rx, sizeof rx, IDLE_MS);
        if (n == -2) { put(&t->error, "nothing from the server for two minutes"); break; }
        if (n <= 0) break;
        uint32_t i = 0;
        if (!in_body) {
            for (; i < (uint32_t)n && !in_body; i++) {
                put_n(&hdr, &rx[i], 1);
                if (hdr.n >= 4 && !memcmp(hdr.p + hdr.n - 4, "\r\n\r\n", 4)) {
                    in_body = true;
                    const char *sp = strchr(hdr.p, ' ');
                    status = sp ? num(sp + 1, hdr.p + hdr.n) : 0;
                    for (char *q = hdr.p; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
                    bd.chunked = strstr(hdr.p, "transfer-encoding: chunked") != 0;
                }
            }
        }
        if (in_body && i < (uint32_t)n) {
            if (status == 200) body_bytes(t, &bd, rx + i, (uint32_t)n - i);
            else put_n(&errbody, rx + i, (uint32_t)n - i);
        }
    }
    ubiqos_tls_close(tls);
    if (status && status != 200 && !t->error.n) {
        const char *e = errbody.p ? errbody.p + errbody.n : 0;
        // A chunked error body has its size line first; the JSON starts at '{'.
        const char *j = errbody.p ? strchr(errbody.p, '{') : 0;
        const char *er = j ? field(j, e, "error") : 0;
        if (er) str_into(field(er, skip(er, e), "message"), skip(er, e), &t->error);
        if (!t->error.n) { put(&t->error, "HTTP "); put_u32(&t->error, status); }
    }
    buf_free(&hdr); buf_free(&bd.line); buf_free(&errbody);
    return status;
}

static void turn_free(turn_t *t) {
    buf_free(&t->content); buf_free(&t->text); buf_free(&t->json);
    buf_free(&t->sig); buf_free(&t->start); buf_free(&t->error);
}

// A whole turn: ask, and as long as Claude wants tools, run them and ask again.
static void converse(void) {
    for (;;) {
        turn_t t = { 0 };
        uint32_t status = 0;
        for (uint32_t attempt = 0; attempt < 4; attempt++) {
            if (attempt) {
                say("\r\n\x1b[2m[busy; trying again]\x1b[0m");
                ubiqos_sleep(2000u << attempt);
                turn_free(&t);
                memset(&t, 0, sizeof t);
            }
            say("\x1b[2m...\x1b[0m");
            status = ask_claude(&t);
            // Busy, or an error in the stream before anything was said: again.
            const bool stream_failed = status == 200 && t.error.n && !t.content.n && !t.said_text;
            if (status != 429 && status != 529 && status < 500 && !stream_failed) break;
        }
        if (status != 200 || t.error.n) {
            say("\r\n\x1b[1magent:\x1b[0m ");
            say(t.error.n ? t.error.p : "no answer");
            say("\r\n");
            turn_free(&t);
            // The question that got no answer is taken back, so the next one
            // does not follow a user message with another.
            return;
        }
        say("\r\n\x1b[2m[");
        say_u32(t.in_tok + t.cache_read + t.cache_write); say(" in (");
        say_u32(t.cache_read); say(" cached), "); say_u32(t.out_tok); say(" out]\x1b[0m\r\n");

        buf_t arr = { 0 };
        put(&arr, "["); put_n(&arr, t.content.p, t.content.n); put(&arr, "]");
        add_message("assistant", arr.p, arr.n);

        if (!strcmp(t.stop, "refusal")) say("\x1b[1magent:\x1b[0m Claude declined to go on with that.\r\n");
        if (!strcmp(t.stop, "max_tokens")) say("\x1b[1magent:\x1b[0m the answer was cut off at its limit.\r\n");
        if (strcmp(t.stop, "tool_use")) { buf_free(&arr); turn_free(&t); return; }

        // Every tool_use block gets its result, all in one message.
        buf_t results = { 0 };
        put(&results, "[");
        const char *e = arr.p + arr.n;
        const char *p = arr.p + 1;
        bool first = true;
        while ((p = ws(p, e)) < e && *p == '{') {
            const char *end = skip(p, e);
            if (!end) break;
            if (str_is(field(p, end, "type"), end, "tool_use")) {
                buf_t nm = { 0 };
                str_into(field(p, end, "name"), end, &nm);
                const char *id = field(p, end, "id");
                const char *in = field(p, end, "input");
                buf_t out = { 0 };
                bool is_error = false;
                do_tool(nm.p ? nm.p : "", in, in ? skip(in, end) : end, &out, &is_error);
                if (!first) put(&results, ",");
                first = false;
                put(&results, "{\"type\":\"tool_result\",\"tool_use_id\":");
                put_n(&results, id, (uint32_t)(skip(id, end) - id));
                if (is_error) put(&results, ",\"is_error\":true");
                put(&results, ",\"content\":");
                put_json_str(&results, out.p ? out.p : "", out.n);
                put(&results, "}");
                buf_free(&out); buf_free(&nm);
            }
            p = ws(end, e);
            if (p < e && *p == ',') p++;
        }
        put(&results, "]");
        add_message("user", results.p, results.n);
        buf_free(&results); buf_free(&arr); turn_free(&t);
        if (convo.n > CONVO_MAX || convo.failed) {
            say("\x1b[1magent:\x1b[0m the conversation is too long to go on; /new starts again.\r\n");
            return;
        }
    }
}

// --- main -------------------------------------------------------------------

static bool take_key(void) {
    ubiqos_keyreq_t r;
    memset(&r, 0, sizeof r);
    strncpy(r.name, KEY_NAME, sizeof r.name - 1);
    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_TAKE, &r);
    if (rc != 0 || !r.len) { memset(&r, 0, sizeof r); return false; }
    memcpy(api_key, r.value, r.len);
    api_key[r.len] = 0;
    memset(&r, 0, sizeof r);
    return true;
}

int main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: agent\n\n"
            "A conversation with Claude, which can list and read files, write and edit\n"
            "them, and run the board's programs -- asking you before each write, edit\n"
            "or run. The API key is the key store's api.anthropic: 'key set\n"
            "api.anthropic' once, with the store unlocked.\n\n"
            "Inside: /new forgets the conversation, /ask turns the asking back on,\n"
            "/quit or Ctrl-D leaves.\n"))
        return 0;

    if (!take_key()) {
        say("agent: no key -- unlock the store ('key unlock') and 'key set " KEY_NAME "'\r\n");
        return 1;
    }
    make_system_prompt();
    say("Claude (" MODEL ") on this board. /help for the commands, /quit to leave.\r\n");

    static char line[LINE_MAX];
    for (;;) {
        say("\r\n\x1b[1m>\x1b[0m ");
        if (!read_line(line, sizeof line)) break;
        if (!line[0]) continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (!strcmp(line, "/new")) { buf_free(&convo); say("a new conversation\r\n"); continue; }
        if (!strcmp(line, "/ask")) { ask_all = false; say("asking again before writes and runs\r\n"); continue; }
        if (!strcmp(line, "/help")) {
            say("/new forgets the conversation, /ask asks again before writes and runs,\r\n"
                "/quit leaves. Anything else goes to Claude.\r\n");
            continue;
        }
        buf_t msg = { 0 };
        put(&msg, "[{\"type\":\"text\",\"text\":");
        put_json_str(&msg, line, (uint32_t)strlen(line));
        put(&msg, "}]");
        const uint32_t mark = convo.n;
        add_message("user", msg.p, msg.n);
        buf_free(&msg);
        const uint32_t before = convo.n;
        converse();
        // No answer at all: the question goes, so the conversation still
        // alternates and the same question can be asked again.
        if (convo.n == before && convo.p) { convo.n = mark; convo.p[mark] = 0; }
    }
    memset(api_key, 0, sizeof api_key);
    buf_free(&convo);
    buf_free(&system_prompt);
    return 0;
}

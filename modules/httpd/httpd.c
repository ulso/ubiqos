#include "../../common/myrtos_abi.h"

// httpd -- a web server, in the only way this machine could have one cheaply.
//
// The ESP32-C6 carries the TCP/IP stack, so there is no stack here and there is
// not going to be one: this asks the chip to listen on a port, asks it who is
// there, reads the request and writes the answer. What makes a web server
// possible on a machine with half a megabyte of SRAM is that the hard half is
// on the other chip. See the note at the top of modules/wifilib.
//
// It serves files from the SD card, and the machine's own state where a file
// would not do. Both matter: a file server is what makes it useful, and /status
// is what makes it worth reaching for -- the board can be asked how it is from
// a browser instead of over the console.
//
//   httpd            serve on port 80 until interrupted
//   httpd 8080       another port, for a machine that already has one
//
// Ctrl-C ends it, which is why the accept loop sleeps rather than spins: a
// process spinning at this priority is a process nothing can interrupt.

#define REQ_MAX  512
#define BUF_MAX  1024

static bool starts(const char *s, const char *pre) {
    while (*pre) { if (*s != *pre) return false; s++; pre++; }
    return true;
}

static uint32_t u32_to_dec(uint32_t v, char *out) {
    char tmp[11];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

// A whole answer, headers and body, in as few writes as the chip will take.
// Every send costs a command, a wait and a poll for "did it go", so a header
// written a field at a time would cost more than the page.
static void send_str(int32_t sock, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    myrtos_sock_send(sock, (const uint8_t *)s, n);
}

static void send_head(int32_t sock, const char *status, const char *type, uint32_t len) {
    char head[160];
    uint32_t n = 0;
    const char *parts[] = { "HTTP/1.1 ", status, "\r\nContent-Type: ", type,
                            "\r\nContent-Length: " };
    for (uint32_t p = 0; p < 5; p++)
        for (const char *q = parts[p]; *q && n < sizeof(head) - 40; q++) head[n++] = *q;
    n += u32_to_dec(len, head + n);
    // Closed after every answer. Keep-alive would need a second timeout and a
    // second state, and this serves one client at a time anyway.
    for (const char *q = "\r\nConnection: close\r\n\r\n"; *q; q++) head[n++] = *q;
    head[n] = 0;
    myrtos_sock_send(sock, (const uint8_t *)head, n);
}

// What the machine will say about itself. Small enough to build in one buffer,
// which is what lets the length be known before the header goes out.
static uint32_t status_page(char *out, uint32_t max) {
    (void)max;
    uint32_t n = 0;
    const char *head =
        "<!doctype html><meta charset=\"utf-8\"><title>myrtos</title>"
        "<h1>myrtos</h1><table>";
    for (const char *q = head; *q; q++) out[n++] = *q;

    struct { const char *label; uint32_t value; } rows[] = {
        { "SRAM largest free",  (uint32_t)myrtos_meminfo(MYRTOS_MEM_LARGEST_FREE) },
        { "PSRAM largest free", (uint32_t)myrtos_meminfo(MYRTOS_MEM_BULK_FREE) },
        { "PSRAM total",        (uint32_t)myrtos_meminfo(MYRTOS_MEM_BULK_SIZE) },
        { "Processes",          (uint32_t)myrtos_meminfo(MYRTOS_MEM_PROCESSES) },
        { "Assertions stepped", (uint32_t)myrtos_meminfo(MYRTOS_MEM_ASSERTS) },
    };
    for (uint32_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        for (const char *q = "<tr><td>"; *q; q++) out[n++] = *q;
        for (const char *q = rows[i].label; *q; q++) out[n++] = *q;
        for (const char *q = "<td>"; *q; q++) out[n++] = *q;
        n += u32_to_dec(rows[i].value, out + n);
    }
    for (const char *q = "</table><p><a href=\"/\">files</a>"; *q; q++) out[n++] = *q;
    out[n] = 0;
    return n;
}

// The card's root as a page of links. Written with the same directory call ls
// uses, so what the browser lists and what the console lists cannot disagree.
static uint32_t index_page(char *out, uint32_t max) {
    uint32_t n = 0;
    for (const char *q = "<!doctype html><meta charset=\"utf-8\"><title>/sd</title>"
                         "<h1>/sd</h1><ul>"; *q; q++) out[n++] = *q;
    for (uint32_t i = 0; n < max - 160; i++) {
        char raw[MYRTOS_DIRNAME_MAX], name[MYRTOS_DIRNAME_MAX];
        uint32_t size = 0;
        if (myrtos_fs_dir_at("/sd", i, raw, &size) < 0) break;
        // The same expansion ls does. A short FAT entry is eleven padded
        // characters with the dot implied, and the rule for putting it back
        // lives in one place -- see the note beside myrtos_pretty_name.
        myrtos_pretty_name(raw, name);
        for (const char *q = "<li><a href=\"/sd/"; *q; q++) out[n++] = *q;
        for (const char *q = name; *q; q++) out[n++] = *q;
        for (const char *q = "\">"; *q; q++) out[n++] = *q;
        for (const char *q = name; *q; q++) out[n++] = *q;
        for (const char *q = "</a> "; *q; q++) out[n++] = *q;
        n += u32_to_dec(size, out + n);
    }
    for (const char *q = "</ul><p><a href=\"/status\">status</a>"; *q; q++) out[n++] = *q;
    out[n] = 0;
    return n;
}

// A file, in chip-sized pieces. The length goes in the header first, which is
// what a stat is for -- without it the answer would have to be buffered whole,
// and the card holds files larger than this machine's memory.
static bool send_file(int32_t sock, const char *path) {
    uint32_t size = 0;
    if (myrtos_fs_stat(path, &size) < 0) return false;

    // By extension, which is a guess and is the guess every server makes. A
    // wrong one shows the file rather than losing it.
    const char *type = "application/octet-stream";
    uint32_t n = 0;
    while (path[n]) n++;
    if (n > 4 && starts(path + n - 4, ".txt")) type = "text/plain; charset=utf-8";
    else if (n > 5 && starts(path + n - 5, ".html")) type = "text/html; charset=utf-8";

    send_head(sock, "200 OK", type, size);

    int32_t fd = myrtos_open_flags(path, MYRTOS_O_RDONLY);
    if (fd < 0) return false;
    uint8_t buf[BUF_MAX];
    for (;;) {
        int32_t got = myrtos_read(fd, buf, sizeof buf);
        if (got <= 0) break;
        if (myrtos_sock_send(sock, buf, (uint32_t)got) < 0) break;
    }
    myrtos_close(fd);
    return true;
}

static void serve(int32_t sock, const char *req) {
    // GET and nothing else. A machine that cannot be written to over the
    // network is a machine one fewer thing can go wrong with, and nothing here
    // wants a PUT.
    if (!starts(req, "GET ")) {
        const char *msg = "method not allowed";
        send_head(sock, "405 Method Not Allowed", "text/plain", 18);
        send_str(sock, msg);
        return;
    }

    char path[128];
    uint32_t n = 0;
    const char *p = req + 4;
    while (*p && *p != ' ' && *p != '\r' && n < sizeof(path) - 1) path[n++] = *p++;
    path[n] = 0;

    char page[BUF_MAX * 3];
    if (starts(path, "/status")) {
        uint32_t len = status_page(page, sizeof page);
        send_head(sock, "200 OK", "text/html; charset=utf-8", len);
        send_str(sock, page);
        return;
    }
    if (path[1] == 0) {                                  // "/"
        uint32_t len = index_page(page, sizeof page);
        send_head(sock, "200 OK", "text/html; charset=utf-8", len);
        send_str(sock, page);
        return;
    }
    if (send_file(sock, path)) return;

    send_head(sock, "404 Not Found", "text/plain", 9);
    send_str(sock, "not found");
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: httpd [port]\n\n"
            "Serves /sd over HTTP, and /status for the machine's own numbers.\n"
            "Needs the board to be on a network -- 'wifi connect' first.\n"))
        return;

    uint32_t port = 80;
    if (argc > 1) {
        port = 0;
        for (const char *q = argv[1]; *q >= '0' && *q <= '9'; q++) port = port * 10 + (uint32_t)(*q - '0');
        if (!port || port > 65535) { say("httpd: that is not a port\r\n"); return; }
    }

    char addr[48];
    if (myrtos_wifi_address(addr, sizeof addr) != 0) {
        say("httpd: not on a network. 'wifi connect <ssid>' first.\r\n");
        return;
    }

    int32_t server = myrtos_sock_listen((uint16_t)port);
    if (server < 0) { say("httpd: the chip would not listen\r\n"); return; }

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "httpd: serving on ");
    myrtos_line_str(&l, addr);
    myrtos_line_str(&l, " port ");
    myrtos_line_u32(&l, port);
    myrtos_line_str(&l, ", ctrl-C to stop\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    char req[REQ_MAX];
    for (;;) {
        int32_t client = myrtos_sock_accept(server);
        if (client < 0) {
            // Sleeping and not spinning. A loop that never yields is a loop
            // nothing else runs beside -- including the console that has to
            // deliver the ctrl-C that ends this.
            myrtos_sleep(20);
            continue;
        }

        // The request line is all that is read. Headers after it are skipped by
        // not reading them, which is allowed and is what lets this answer
        // without a parser.
        uint32_t n = 0;
        for (int spin = 0; spin < 100 && n < sizeof(req) - 1; spin++) {
            int32_t got = myrtos_sock_recv(client, (uint8_t *)req + n, sizeof(req) - 1 - n);
            if (got < 0) break;
            if (got == 0) { myrtos_sleep(5); continue; }
            n += (uint32_t)got;
            req[n] = 0;
            bool done = false;
            for (uint32_t i = 3; i < n; i++)
                if (req[i - 3] == '\r' && req[i - 2] == '\n' &&
                    req[i - 1] == '\r' && req[i] == '\n') done = true;
            if (done) break;
        }
        req[n] = 0;
        if (n) serve(client, req);
        myrtos_sock_close(client);
    }
}

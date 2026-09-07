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

// Sixteen kilobytes, and the number is not cautious. A module gets 4096 bytes
// of data and stack together unless it says otherwise, and the first version of
// this file did not say otherwise while putting 4.6 kB of buffers on the stack:
// the page buffer, the file buffer and the request line are all live at once
// down the same call chain. It overflowed into PSRAM below its own allocation
// and wrote over the bulk pool's control block, so `free` reported "PSRAM
// largest free: 0" on a pool that was still handing out memory happily.
//
// Nothing caught it. That is worth knowing about this system: a module's stack
// has no guard page and no canary, so it corrupts a neighbour rather than
// faulting -- and the neighbour here was the allocator's own bookkeeping.
MYRTOS_MEM_SIZE(16384);

#define REQ_MAX  256
#define BUF_MAX  512
// The largest page built in memory rather than streamed. Content-Length has to
// go out before the body, so a generated page is written whole and then sent;
// a file is not, because its length comes from a stat and the card holds files
// larger than this machine's memory.
#define PAGE_MAX 2048

// Where the scanner publishes. It is hibouair that owns the dongle; this only
// serves what it has written. See the note at /api/sensors.
#define SENSORS_PATH "/tmp/sensors.json"

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

// The page. Compiled in rather than served off the card, and that is a
// compromise rather than the right answer: a page belongs in a file, and this
// one is here so that it deploys with a flash instead of needing the card to be
// handed to a host first. Move it to /sd/www when there is a comfortable way to
// put files there.
//
// It is the BleuIO tab of pico-io-bridge and nothing else -- the sensor cards,
// the state dot, the two-second poll. The rest of that UI is I2C, SCPI and
// audio, none of which this machine has.
static const char SENSOR_PAGE[] =
    "<!doctype html><meta charset=\"utf-8\"><title>myrtos - HibouAir</title>\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<style>\n"
    ":root{--bg:#0f1720;--card:#152029;--line:#233240;--ink:#e6edf3;--dim:#8b9bab;--ok:#4ade80;--off:#64748b}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;padding:24px;background:var(--bg);color:var(--ink);\n"
    "     font:15px/1.45 system-ui,-apple-system,\"Segoe UI\",sans-serif}\n"
    "h1{margin:0;font-size:26px}\n"
    ".sub{color:var(--dim);margin:2px 0 20px}\n"
    ".top{display:flex;justify-content:space-between;align-items:flex-start;flex-wrap:wrap;gap:12px}\n"
    ".state{display:flex;align-items:center;gap:8px;font-weight:600}\n"
    ".dot{width:10px;height:10px;border-radius:50%;background:var(--off)}\n"
    ".dot.on{background:var(--ok)}\n"
    ".grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(240px,1fr))}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}\n"
    ".card h2{margin:0 0 12px;font-size:17px;display:flex;justify-content:space-between;\n"
    "         align-items:baseline;gap:8px}\n"
    ".id{color:var(--dim);font-size:13px;font-weight:400}\n"
    ".pairs{display:grid;grid-template-columns:1fr 1fr;gap:10px 14px}\n"
    ".k{color:var(--dim);font-size:12px}\n"
    ".v{font-size:18px;font-weight:600}\n"
    ".foot{color:var(--dim);font-size:12px;margin-top:12px;border-top:1px solid var(--line);padding-top:10px}\n"
    ".note{color:var(--dim)}\n"
    "a{color:var(--dim)}\n"
    "</style>\n"
    "<div class=top>\n"
    "  <div><h1>myrtos &middot; HibouAir</h1><div class=sub>BleuIO scanner over the USB host</div></div>\n"
    "  <div class=state><span class=dot id=dot></span><span id=stateText>connecting</span></div>\n"
    "</div>\n"
    "<div class=grid id=cards></div>\n"
    "<p class=note id=note></p>\n"
    "<p><a href=\"/files\">files on the card</a> &middot; <span id=mem></span></p>\n"
    "<script>\n"
    "// Polling, not a WebSocket. The sensors advertise every couple of seconds and\n"
    "// the scanner republishes on the same beat, so asking on that beat sees every\n"
    "// change there is -- and it keeps the server able to close after each answer.\n"
    "var CARDS = document.getElementById(\"cards\"), NOTE = document.getElementById(\"note\"),\n"
    "    DOT = document.getElementById(\"dot\"), STATE = document.getElementById(\"stateText\"),\n"
    "    MEM = document.getElementById(\"mem\");\n"
    "\n"
    "function pair(k, v){ return \"<div><div class=k>\" + k + \"</div><div class=v>\" + v + \"</div></div>\"; }\n"
    "\n"
    "function draw(d){\n"
    "  var s = d.sensors || [];\n"
    "  DOT.className = \"dot\" + (s.length ? \" on\" : \"\");\n"
    "  STATE.textContent = s.length ? (s.length + \" sensor\" + (s.length > 1 ? \"s\" : \"\")) : \"no sensors\";\n"
    "  NOTE.textContent = s.length ? \"\" :\n"
    "    (d.scanning === false ? \"Nothing is scanning. Run 'hibouair -q &' on the board.\"\n"
    "                          : \"Scanning; nothing has advertised yet.\");\n"
    "  CARDS.innerHTML = s.map(function(e){\n"
    "    var p = pair(\"Temperature\", e.temp + \" &deg;C\") + pair(\"Humidity\", e.humidity + \" %\");\n"
    "    if (e.co2)      p += pair(\"CO2\", e.co2 + \" ppm\");\n"
    "    p += pair(\"Pressure\", e.pressure + \" hPa\");\n"
    "    if (e.voc)      p += pair(\"VOC \" + (e.vocUnit || \"\"), e.voc);\n"
    "    if (e.pm1 > 0 || e.pm25 > 0)\n"
    "      p += pair(\"PM1\", e.pm1 + \" &micro;g/m&sup3;\") + pair(\"PM2.5\", e.pm25 + \" &micro;g/m&sup3;\");\n"
    "    return \"<div class=card><h2>\" + (e.type || \"sensor\") +\n"
    "           \"<span class=id>#\" + e.board + \"</span></h2><div class=pairs>\" + p +\n"
    "           \"</div><div class=foot>\" + e.addr + \"</div></div>\";\n"
    "  }).join(\"\");\n"
    "}\n"
    "\n"
    "function tick(){\n"
    "  fetch(\"/api/sensors\", {cache:\"no-store\"}).then(function(r){ return r.json(); }).then(draw)\n"
    "    // A parse failure is \"not this time\" and not an error: the scanner writes\n"
    "    // the file in place, so a reader can catch it half written. The next poll\n"
    "    // is two seconds away.\n"
    "    .catch(function(){});\n"
    "  fetch(\"/api/status\", {cache:\"no-store\"}).then(function(r){ return r.json(); })\n"
    "    .then(function(m){\n"
    "      MEM.textContent = \"SRAM \" + m.sramFree + \" B free, PSRAM \" +\n"
    "                        Math.round(m.psramFree/1024) + \" kB free, \" + m.processes + \" processes\";\n"
    "    }).catch(function(){});\n"
    "}\n"
    "tick(); setInterval(tick, 2000);\n"
    "</script>\n"
    "\n";

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

// A whole answer, headers and body, in as few writes as the chip will take.
// Every send costs a command, a wait and a poll for "did it go", so a header
// written a field at a time would cost more than the page.
// Until it has all gone. myrtos_sock_send hands the chip one buffer and answers
// with how much it took -- at most 2000 bytes, which is the chip's own limit --
// so a single call is not a write, it is the first of however many it takes.
//
// This called it once. Every answer under two kilobytes was therefore correct
// and the sensor page, which is four, arrived cut in half: curl gave up with
// "transfer closed with outstanding read data remaining" and a browser showed a
// page missing its script. A short write that is not looped is a bug that hides
// until the day something gets big.
static void send_str(int32_t sock, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    for (uint32_t done = 0; done < n; ) {
        int32_t sent = myrtos_sock_send(sock, (const uint8_t *)s + done, n - done);
        if (sent <= 0) return;                 // the client has gone
        done += (uint32_t)sent;
    }
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

// What the machine will say about itself, as JSON.
//
// It used to be built as HTML here, which put the server in charge of what a
// page looks like. Data is the better boundary and it is the one the
// pico-io-bridge UI already assumes: its tabs fetch JSON and draw themselves,
// which is why that page could be inherited at all when its server could not.
static uint32_t status_json(char *out, uint32_t max) {
    (void)max;
    uint32_t n = 0;
    struct { const char *key; uint32_t value; } rows[] = {
        { "sramFree",   (uint32_t)myrtos_meminfo(MYRTOS_MEM_LARGEST_FREE) },
        { "psramFree",  (uint32_t)myrtos_meminfo(MYRTOS_MEM_BULK_FREE) },
        { "psramTotal", (uint32_t)myrtos_meminfo(MYRTOS_MEM_BULK_SIZE) },
        { "processes",  (uint32_t)myrtos_meminfo(MYRTOS_MEM_PROCESSES) },
        { "assertions", (uint32_t)myrtos_meminfo(MYRTOS_MEM_ASSERTS) },
    };
    out[n++] = '{';
    for (uint32_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (i) out[n++] = ',';
        out[n++] = '"';
        for (const char *q = rows[i].key; *q; q++) out[n++] = *q;
        out[n++] = '"'; out[n++] = ':';
        n += u32_to_dec(rows[i].value, out + n);
    }
    out[n++] = '}';
    out[n] = 0;
    return n;
}

// The card's root as a page of links. Written with the same directory call ls
// uses, so what the browser lists and what the console lists cannot disagree.
static uint32_t index_page(char *out, uint32_t max) {
    uint32_t n = 0;
    for (const char *q = "<!doctype html><meta charset=\"utf-8\"><title>/sd</title>"
                         "<h1>/sd</h1><ul>"; *q; q++) out[n++] = *q;
    for (uint32_t i = 0; n + 200 < max; i++) {
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
    for (const char *q = "</ul><p><a href=\"/\">sensors</a>"; *q; q++) out[n++] = *q;
    out[n] = 0;
    return n;
}

// A file, in chip-sized pieces. The length goes in the header first, which is
// what a stat is for -- without it the answer would have to be buffered whole,
// and the card holds files larger than this machine's memory.
static bool send_file(int32_t sock, const char *path, const char *as_type) {
    uint32_t size = 0;
    if (myrtos_fs_stat(path, &size) < 0) return false;

    // By extension, which is a guess and is the guess every server makes. A
    // wrong one shows the file rather than losing it. A caller that knows
    // better says so -- /api/sensors serves a file whose name ends in .json
    // but whose type is the endpoint's business, not the filename's.
    const char *type = as_type ? as_type : "application/octet-stream";
    uint32_t n = 0;
    while (path[n]) n++;
    if (!as_type) {
        if (n > 4 && starts(path + n - 4, ".txt")) type = "text/plain; charset=utf-8";
        else if (n > 5 && starts(path + n - 5, ".html")) type = "text/html; charset=utf-8";
        else if (n > 5 && starts(path + n - 5, ".json")) type = "application/json";
    }

    send_head(sock, "200 OK", type, size);

    int32_t fd = myrtos_open_flags(path, MYRTOS_O_RDONLY);
    if (fd < 0) return false;
    uint8_t buf[BUF_MAX];
    for (;;) {
        int32_t got = myrtos_read(fd, buf, sizeof buf);
        if (got <= 0) break;
        // The same loop, for the same reason. This one happened to be safe --
        // BUF_MAX is 512 and the chip takes 2000 -- which is exactly how a
        // missing loop survives review.
        bool gone = false;
        for (uint32_t done = 0; done < (uint32_t)got && !gone; ) {
            int32_t sent = myrtos_sock_send(sock, buf + done, (uint32_t)got - done);
            if (sent <= 0) gone = true; else done += (uint32_t)sent;
        }
        if (gone) break;
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

    char page[PAGE_MAX];

    // --- /api ---------------------------------------------------------------
    // Two endpoints and no more. The sensors are not read here: the scanner
    // owns the dongle and publishes what it has seen to /tmp/sensors.json, and
    // this serves that file like any other. Two readers of one dongle is what
    // that arrangement exists to prevent, and it means /api/sensors needed no
    // code at all beyond the name.
    if (starts(path, "/api/status")) {
        uint32_t len = status_json(page, sizeof page);
        send_head(sock, "200 OK", "application/json", len);
        send_str(sock, page);
        return;
    }
    if (starts(path, "/api/sensors")) {
        if (send_file(sock, SENSORS_PATH, "application/json")) return;
        // Nothing is scanning, which is not an error and should not read as
        // one: the page says so rather than showing an empty table as though
        // the room had no air in it.
        static const char none[] = "{\"sensors\":[],\"count\":0,\"scanning\":false}";
        send_head(sock, "200 OK", "application/json", sizeof none - 1);
        send_str(sock, none);
        return;
    }

    if (path[1] == 0 || starts(path, "/sensors")) {      // "/" is the sensor page
        send_head(sock, "200 OK", "text/html; charset=utf-8", sizeof SENSOR_PAGE - 1);
        send_str(sock, SENSOR_PAGE);
        return;
    }
    if (starts(path, "/files")) {
        uint32_t len = index_page(page, sizeof page);
        send_head(sock, "200 OK", "text/html; charset=utf-8", len);
        send_str(sock, page);
        return;
    }
    if (send_file(sock, path, 0)) return;

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
            // Two hundred milliseconds, not twenty. Every ask is an
            // AVAIL_DATA_TCP over SPI, so twenty meant fifty transactions a
            // second for ever, whether or not anybody was connecting -- and
            // each one is a chance for the protocol to go wrong. Nobody
            // notices a fifth of a second before a page starts loading, and
            // nine tenths of the traffic to the chip was this loop asking
            // whether anything had happened yet.
            //
            // Sleeping and not spinning, which was always the point: a loop
            // that never yields is a loop nothing else runs beside, including
            // the console that has to deliver the ctrl-C that ends this.
            myrtos_sleep(200);
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

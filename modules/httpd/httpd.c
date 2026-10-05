#include "../../common/ubiqos_abi.h"

// httpd -- a web server.
//
// It was written when the ESP32-C6 ran NINA and carried the TCP/IP stack, so the
// machine had none: this asked the chip to listen on a port, asked it who was
// there, read the request and wrote the answer. Since 10 Sep 2026 the chip runs
// ESP-Hosted and is only a radio, and the stack is lwIP in the kernel -- one
// stack for the USB cable and the WiFi alike. The socket calls stayed the same;
// only the stack they name changed, which is the second argument below.
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
UBIQOS_MEM_SIZE(16384);

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

// How long to wait for a network stack that has not started yet.
#define NET_WAIT_S 30

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
    "<!doctype html><meta charset=\"utf-8\"><title>UbiqOS - HibouAir</title>\n"
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
    "  <div><h1>UbiqOS &middot; HibouAir</h1><div class=sub id=sub></div></div>\n"
    "  <div class=state><span class=dot id=dot></span><span id=stateText>connecting</span></div>\n"
    "</div>\n"
    "<div class=grid id=cards></div>\n"
    "<p class=note id=note></p>\n"
    "<p><span id=files hidden><a href=\"/files\">files on the card</a> &middot; </span><span id=mem></span></p>\n"
    "<script>\n"
    "// Polling, not a WebSocket. The sensors advertise every couple of seconds and\n"
    "// the scanner republishes on the same beat, so asking on that beat sees every\n"
    "// change there is -- and it keeps the server able to close after each answer.\n"
    "var CARDS = document.getElementById(\"cards\"), NOTE = document.getElementById(\"note\"),\n"
    "    DOT = document.getElementById(\"dot\"), STATE = document.getElementById(\"stateText\"),\n"
    "    MEM = document.getElementById(\"mem\"), SUB = document.getElementById(\"sub\"),\n"
    "    FILES = document.getElementById(\"files\");\n"
    "\n"
    "function pair(k, v){ return \"<div><div class=k>\" + k + \"</div><div class=v>\" + v + \"</div></div>\"; }\n"
    "\n"
    "function draw(d){\n"
    "  var s = d.sensors || [];\n"
    "  if (d.source) SUB.textContent = d.source;\n"
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
    "      FILES.hidden = !m.card;\n"
    "    }).catch(function(){});\n"
    "}\n"
    "tick(); setInterval(tick, 2000);\n"
    "</script>\n"
    "\n";

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

// How many processes are running this module, this one included.
static uint32_t running_as(const char *name)
{
    uint32_t n = 0;
    for (uint32_t slot = 0; slot < UBIQOS_PS_SLOTS; slot++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(slot, &p) < 0) continue;
        uint32_t i = 0;
        while (name[i] && p.name[i] == name[i]) i++;
        if (!name[i] && !p.name[i]) n++;
    }
    return n;
}

// A whole answer, headers and body, in as few writes as the chip will take.
// Every send costs a command, a wait and a poll for "did it go", so a header
// written a field at a time would cost more than the page.
// Until it has all gone. ubiqos_sock_send hands the chip one buffer and answers
// with how much it took -- at most 2000 bytes, which is the chip's own limit --
// so a single call is not a write, it is the first of however many it takes.
//
// This called it once. Every answer under two kilobytes was therefore correct
// and the sensor page, which is four, arrived cut in half: curl gave up with
// "transfer closed with outstanding read data remaining" and a browser showed a
// page missing its script. A short write that is not looped is a bug that hides
// until the day something gets big.
// ZERO IS NOT AN ERROR, and treating it as one cost a page. The NINA chip
// always takes something, so this could say "sent <= 0 means gone" and be
// right by accident for a year. lwIP has a send buffer of its own and answers
// 0 when it is full, which happens on any page bigger than TCP_SND_BUF -- the
// four kilobyte one arrived as exactly 2920 bytes, which is that buffer to the
// byte, and curl reported it as a closed transfer.
//
// So 0 means ask again, as it already does for receive, and only a negative
// answer is the client having gone. The wait is bounded because a client that
// has stopped reading must not hold this here for ever.
//
// One loop for everything that sends, so the rule cannot be forgotten again: it
// was, in send_file, which still took 0 for a client that had gone -- so a file
// larger than the send buffer stopped at 2812 bytes, every time. False when the
// client has gone or taken nothing for a second.
static bool send_all(int32_t sock, const uint8_t *p, uint32_t n) {
    uint32_t stalled = 0;
    for (uint32_t done = 0; done < n; ) {
        int32_t sent = ubiqos_sock_send(sock, p + done, n - done);
        if (sent < 0) return false;            // the client has gone
        if (sent == 0) {
            if (++stalled > 1000) return false;   // a second of nothing taken
            ubiqos_sleep(1);                   // let the acknowledgements in
            continue;
        }
        stalled = 0;
        done += (uint32_t)sent;
    }
    return true;
}

static void send_str(int32_t sock, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    send_all(sock, (const uint8_t *)s, n);
}

static void send_head(int32_t sock, const char *status, const char *type, uint32_t len) {
    char head[160];
    uint32_t n = 0;
    const char *parts[] = { "HTTP/1.1 ", status, "\r\nContent-Type: ", type,
                            "\r\nContent-Length: " };
    for (uint32_t p = 0; p < 5; p++)
        for (const char *q = parts[p]; *q && n < sizeof(head) - 40; q++) head[n++] = *q;
    n += u32_to_dec(len, head + n);
    // Kept open. Every reply here goes through this function with a real
    // Content-Length, so a client always knows where a body ends and the next
    // reply begins -- which is the whole precondition for keeping a connection,
    // and the reason it is safe to say so.
    for (const char *q = "\r\nConnection: keep-alive\r\n\r\n"; *q; q++) head[n++] = *q;
    head[n] = 0;
    send_all(sock, (const uint8_t *)head, n);
}

// What the machine will say about itself, as JSON.
//
// It used to be built as HTML here, which put the server in charge of what a
// page looks like. Data is the better boundary and it is the one the
// pico-io-bridge UI already assumes: its tabs fetch JSON and draw themselves,
// which is why that page could be inherited at all when its server could not.
// Whether there is a card to list. The root holds nothing but volumes, so a
// card is there exactly when "sd" is among them -- and a Challenger, which has
// no socket for one, offers no link to a page of nothing.
static bool card_present(void) {
    char raw[UBIQOS_DIRNAME_MAX], name[UBIQOS_DIRNAME_MAX];
    uint32_t size;
    for (uint32_t i = 0; ubiqos_fs_dir_at("/", i, raw, &size) >= 0; i++) {
        ubiqos_pretty_name(raw, name);
        if (name[0] == 's' && name[1] == 'd' && !name[2]) return true;
    }
    return false;
}

static uint32_t status_json(char *out, uint32_t max) {
    (void)max;
    uint32_t n = 0;
    struct { const char *key; uint32_t value; } rows[] = {
        { "sramFree",   (uint32_t)ubiqos_meminfo(UBIQOS_MEM_LARGEST_FREE) },
        { "psramFree",  (uint32_t)ubiqos_meminfo(UBIQOS_MEM_BULK_FREE) },
        { "psramTotal", (uint32_t)ubiqos_meminfo(UBIQOS_MEM_BULK_SIZE) },
        { "processes",  (uint32_t)ubiqos_meminfo(UBIQOS_MEM_PROCESSES) },
        { "assertions", (uint32_t)ubiqos_meminfo(UBIQOS_MEM_ASSERTS) },
        { "card",       card_present() ? 1u : 0u },
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

// --- /api/i2c ------------------------------------------------------------------
//
// The I2C bus over HTTP, for an app on a phone: one transaction per request,
// the same transaction /dev/i2c takes, in the URL.
//
//   /api/i2c                       scan: {"ok":true,"devices":[119]}
//   /api/i2c?addr=77&w=d0&n=1      write d0, read one byte back:
//                                  {"ok":true,"addr":119,"data":[97]}
//   /api/i2c?addr=77&w=7202        write 72 02 and read nothing
//
// addr and w are hex, as a datasheet writes them -- "0x77" or "77", and w the
// bytes back to back -- and n is a decimal count, 0 to 64. The data comes back
// as numbers, which is what JSON has. A device that does not answer is
// {"ok":false,"error":"no answer"} with 200, since the request was a good one;
// a request that is not one is 400, and a board with no bus is 503.
#define I2C_WRITE_MAX 32

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The value of name= in the query, and its length; 0 when it is not there.
static const char *param(const char *query, const char *name, uint32_t *len) {
    for (const char *q = query; q && *q; ) {
        const char *s = q;
        uint32_t k = 0;
        while (name[k] && s[k] == name[k]) k++;
        const char *end = q;
        while (*end && *end != '&') end++;
        if (!name[k] && s[k] == '=') { *len = (uint32_t)(end - (s + k + 1)); return s + k + 1; }
        q = *end ? end + 1 : 0;
    }
    return 0;
}

static bool parse_hex(const char *s, uint32_t n, uint32_t *out) {
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
    if (!n || n > 8) return false;
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int d = hexval(s[i]);
        if (d < 0) return false;
        v = v * 16u + (uint32_t)d;
    }
    *out = v;
    return true;
}

static void send_json(int32_t sock, const char *status, const char *body, uint32_t len) {
    send_head(sock, status, "application/json", len);
    send_all(sock, (const uint8_t *)body, len);
}

static uint32_t put_str(char *out, uint32_t n, const char *s) {
    while (*s) out[n++] = *s++;
    return n;
}

static void api_i2c(int32_t sock, const char *query, char *out) {
    uint32_t n = 0;
    const int32_t fd = ubiqos_open("/dev/i2c");
    if (fd < 0) {
        n = put_str(out, 0, "{\"ok\":false,\"error\":\"no I2C bus on this board\"}");
        send_json(sock, "503 Service Unavailable", out, n);
        return;
    }

    uint32_t alen = 0, wlen = 0, nlen = 0;
    const char *a = param(query, "addr", &alen);
    const char *w = param(query, "w", &wlen);
    const char *c = param(query, "n", &nlen);

    uint8_t x[4 + I2C_WRITE_MAX];
    if (!a) {
        // A scan: address each in turn and ask for a byte, as `i2c` does.
        n = put_str(out, 0, "{\"ok\":true,\"devices\":[");
        bool first = true;
        for (uint32_t addr = 0x08; addr <= 0x77; addr++) {
            x[0] = (uint8_t)addr; x[1] = 0; x[2] = 1; x[3] = 0;
            if (ubiqos_write(fd, x, 4) < 0) continue;
            if (!first) out[n++] = ',';
            n += u32_to_dec(addr, out + n);
            first = false;
        }
        n = put_str(out, n, "]}");
        ubiqos_close(fd);
        send_json(sock, "200 OK", out, n);
        return;
    }

    uint32_t addr = 0, count = 0;
    bool good = parse_hex(a, alen, &addr) && addr <= 0x7Fu && wlen % 2 == 0 && wlen / 2 <= I2C_WRITE_MAX;
    for (uint32_t i = 0; good && i < wlen / 2; i++) {
        const int hi = hexval(w[2 * i]), lo = hexval(w[2 * i + 1]);
        if (hi < 0 || lo < 0) good = false;
        else x[4 + i] = (uint8_t)(hi << 4 | lo);
    }
    for (uint32_t i = 0; good && c && i < nlen; i++) {
        if (c[i] < '0' || c[i] > '9') good = false;
        else count = count * 10u + (uint32_t)(c[i] - '0');
    }
    if (!good || count > UBIQOS_I2C_MAX_READ || (!wlen && !count)) {
        ubiqos_close(fd);
        n = put_str(out, 0, "{\"ok\":false,\"error\":\"addr is hex, w is hex bytes (up to 32), n is 0 to 64, and one of w and n is needed\"}");
        send_json(sock, "400 Bad Request", out, n);
        return;
    }

    x[0] = (uint8_t)addr; x[1] = (uint8_t)(wlen / 2); x[2] = (uint8_t)count; x[3] = 0;
    if (ubiqos_write(fd, x, 4 + wlen / 2) < 0) {
        ubiqos_close(fd);
        n = put_str(out, 0, "{\"ok\":false,\"addr\":");
        n += u32_to_dec(addr, out + n);
        n = put_str(out, n, ",\"error\":\"no answer\"}");
        send_json(sock, "200 OK", out, n);
        return;
    }
    uint8_t got[UBIQOS_I2C_MAX_READ];
    const int32_t r = count ? ubiqos_read(fd, got, count) : 0;
    ubiqos_close(fd);

    n = put_str(out, 0, "{\"ok\":true,\"addr\":");
    n += u32_to_dec(addr, out + n);
    n = put_str(out, n, ",\"data\":[");
    for (int32_t i = 0; i < r; i++) {
        if (i) out[n++] = ',';
        n += u32_to_dec(got[i], out + n);
    }
    n = put_str(out, n, "]}");
    send_json(sock, "200 OK", out, n);
}

// The card's root as a page of links. Written with the same directory call ls
// uses, so what the browser lists and what the console lists cannot disagree.
static uint32_t index_page(char *out, uint32_t max) {
    uint32_t n = 0;
    for (const char *q = "<!doctype html><meta charset=\"utf-8\"><title>/sd</title>"
                         "<h1>/sd</h1><ul>"; *q; q++) out[n++] = *q;
    for (uint32_t i = 0; n + 200 < max; i++) {
        char raw[UBIQOS_DIRNAME_MAX], name[UBIQOS_DIRNAME_MAX];
        uint32_t size = 0;
        if (ubiqos_fs_dir_at("/sd", i, raw, &size) < 0) break;
        // The same expansion ls does. A short FAT entry is eleven padded
        // characters with the dot implied, and the rule for putting it back
        // lives in one place -- see the note beside ubiqos_pretty_name.
        ubiqos_pretty_name(raw, name);
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
    if (ubiqos_fs_stat(path, &size) < 0) return false;

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

    int32_t fd = ubiqos_open_flags(path, UBIQOS_O_RDONLY);
    if (fd < 0) return false;
    uint8_t buf[BUF_MAX];
    for (;;) {
        int32_t got = ubiqos_read(fd, buf, sizeof buf);
        if (got <= 0) break;
        if (!send_all(sock, buf, (uint32_t)got)) break;
    }
    ubiqos_close(fd);
    return true;
}

static void serve(int32_t sock, const char *req) {
    // GET and nothing else: nothing here wants a PUT. Not quite read-only any
    // more, though -- /api/i2c can write to a device on the bus, because that
    // is what it is for: a phone's app driving a sensor over the network.
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

    if (starts(path, "/api/i2c") && (path[8] == 0 || path[8] == '?')) {
        api_i2c(sock, path[8] == '?' ? path + 9 : "", page);
        return;
    }

    // --- /api ---------------------------------------------------------------
    // Two endpoints besides /api/i2c above. The sensors are not read here: the scanner
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
        // One read, and the length is what that read returned. Served like any
        // file, the size came from a stat and the body from later reads, and
        // the scanner replacing the file in between made the two disagree --
        // a body cut short, or one longer than it was said to be. The file is
        // swapped whole now (hibouair renames a new one over it), so a single
        // read is a single version. One larger than the buffer is served the
        // old way.
        int32_t fd = ubiqos_open_flags(SENSORS_PATH, UBIQOS_O_RDONLY);
        if (fd >= 0) {
            int32_t got = ubiqos_read(fd, (uint8_t *)page, sizeof page);
            ubiqos_close(fd);
            if (got > 0 && got < (int32_t)sizeof page) {
                send_head(sock, "200 OK", "application/json", (uint32_t)got);
                send_all(sock, (const uint8_t *)page, (uint32_t)got);
                return;
            }
        }
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
    if (ubiqos_help(argc, argv,
            "usage: httpd [port] [stack]\n\n"
            "Serves the sensors at /, the card at /files and the files under it,\n"
            "and the machine's own numbers at /api/status -- on port 80 unless\n"
            "told otherwise.\n\n"
            "The stack is 1, lwIP, by default: the USB cable and the WiFi both.\n"
            "0 is the WiFi coprocessor's own stack, which only NINA firmware has.\n"
            "Asking for one that is not there is refused rather than served\n"
            "on a different network.\n\n"
            "A stack still starting is waited for, up to 30 seconds, so this can\n"
            "be started from /sd/startup. 'kill httpd' stops it.\n"))
        return;

    uint32_t port = 80;
    if (argc > 1) {
        port = 0;
        for (const char *q = argv[1]; *q >= '0' && *q <= '9'; q++) port = port * 10 + (uint32_t)(*q - '0');
        if (!port || port > 65535) { say("httpd: that is not a port\r\n"); return; }
    }

    // Which stack. lwIP unless told otherwise: NINA was the default while the
    // chip ran it, and went on being the default after the chip stopped -- so
    // plain `httpd` asked a stack that was no longer there for an address, got
    // none, and said the board was not on a network while it was on two.
    uint32_t stack = UBIQOS_NET_LWIP;
    if (argc > 2) {
        stack = 0;
        for (const char *q = argv[2]; *q >= '0' && *q <= '9'; q++)
            stack = stack * 10 + (uint32_t)(*q - '0');
    }

    // The address check belongs to the coprocessor and only to it. Asking it
    // whether lwIP has an address would be the wrong question, and answering
    // it would refuse a stack that is perfectly well connected.
    char addr[48];
    if (stack == UBIQOS_NET_NINA && ubiqos_wifi_address(addr, sizeof addr) != 0) {
        say("httpd: not on a network. 'wifi connect <ssid>' first.\r\n");
        return;
    }
    // A listen fails for one of two reasons, and they want opposite answers.
    // Another httpd on the port is final, and it is said at once -- it used to
    // lead with "no such network stack", which on a board whose stack was
    // plainly up and serving the very browser asking read as a fault. A stack
    // that has not started yet is not final at all: from /sd/startup this runs
    // while lwIP is still being brought up, so it is waited for.
    int32_t server = ubiqos_sock_listen_on(stack, (uint16_t)port);
    if (server < 0 && running_as("httpd") > 1) {
        ubiqos_line_t e;
        ubiqos_line_reset(&e);
        ubiqos_line_str(&e, "httpd: could not listen on port ");
        ubiqos_line_u32(&e, port);
        ubiqos_line_str(&e, " -- another httpd is running (ps; kill httpd)\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &e);
        return;
    }
    for (uint32_t waited = 0; server < 0 && waited < NET_WAIT_S; waited++) {
        if (!waited) say("httpd: waiting for the network stack\r\n");
        ubiqos_sleep(1000);
        server = ubiqos_sock_listen_on(stack, (uint16_t)port);
    }
    if (server < 0) {
        ubiqos_line_t e;
        ubiqos_line_reset(&e);
        ubiqos_line_str(&e, "httpd: could not listen on port ");
        ubiqos_line_u32(&e, port);
        ubiqos_line_str(&e, " -- no stack came up, or something else holds the port\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &e);
        return;
    }

    // lwIP answers on every interface it has, so there is no one address to
    // name: the board is its hostname.local on the cable and on the WiFi.
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "httpd: serving on ");
    ubiqos_line_str(&l, stack == UBIQOS_NET_NINA ? addr : "lwIP (the cable and the WiFi)");
    ubiqos_line_str(&l, " port ");
    ubiqos_line_u32(&l, port);
    ubiqos_line_str(&l, ", ctrl-C to stop\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    // Several connections at once, which is what makes keeping them free.
    //
    // Before this, one client was accepted, served to the end and closed
    // before the next was looked at -- so every millisecond spent waiting for
    // one client's next request was charged to whoever was queued behind it.
    // That is why the keep-alive wait had to be cut to fifteen milliseconds:
    // it was a direct tax on everybody else. With a table there is no waiting
    // at all. A connection that has nothing to say is simply skipped.
    //
    // Not parallel, and worth being clear about: the wifi service handles one
    // command at a time, so this interleaves rather than overlaps. That is
    // still the whole difference, because what a client mostly does is think.
    //
    // Four, because four request buffers is a kilobyte and the chip has ten
    // sockets with one of them the listener. It is not a limit anybody will
    // reach on a board with one page to serve.
    #define MAX_CONNS 4
    struct {
        int32_t  sock;                 // -1 when free
        uint32_t n;                    // bytes of the request so far
        uint32_t quiet_since;
        bool     served;               // has this connection answered anything
        bool     asked;                // whether the chip has been asked if it is still there
        uint8_t  match;                // how much of the blank line ending a request is seen
        char     req[REQ_MAX];
    } conn[MAX_CONNS];
    for (uint32_t i = 0; i < MAX_CONNS; i++) {
        conn[i].sock = -1; conn[i].n = 0; conn[i].served = false; conn[i].asked = false;
        conn[i].match = 0;
    }

    uint32_t last_request_ms = 0;

    for (;;) {
        bool worked = false;
        uint32_t now = ubiqos_ticks_now();

        // One new client a pass. Asking is an SPI transaction and there is no
        // hurry: if two arrive together the second is taken on the next pass,
        // five milliseconds later.
        for (uint32_t i = 0; i < MAX_CONNS; i++) {
            if (conn[i].sock >= 0) continue;
            int32_t c = ubiqos_sock_accept(server);
            if (c >= 0) {
                conn[i].sock = c; conn[i].n = 0; conn[i].served = false;
                conn[i].asked = false; conn[i].quiet_since = now; conn[i].match = 0;
                worked = true;
            }
            break;
        }

        for (uint32_t i = 0; i < MAX_CONNS; i++) {
            if (conn[i].sock < 0) continue;
            uint8_t chunk[REQ_MAX];
            int32_t got = ubiqos_sock_recv(conn[i].sock, chunk, sizeof chunk);
            if (got < 0) {
                // A receive that fails is the client gone, or the fault nobody
                // has explained yet -- six requests in a thousand come back
                // empty with the SPI channel reporting no trouble at all. It
                // is worth a word only when nothing was ever served on this
                // connection, because after that it is just a client leaving.
                if (!conn[i].served) {
                    ubiqos_line_t e;
                    ubiqos_line_reset(&e);
                    ubiqos_line_str(&e, "httpd: receive failed before any request, sock ");
                    ubiqos_line_u32(&e, (uint32_t)conn[i].sock);
                    ubiqos_line_str(&e, "\r\n");
                    ubiqos_line_flush(UBIQOS_STDERR, &e);
                }
                ubiqos_sock_close(conn[i].sock);
                conn[i].sock = -1;
                continue;
            }
            if (got == 0) {
                uint32_t quiet = now - conn[i].quiet_since;
                // A RECEIVE OF NOTHING IS NOT A CLOSED CONNECTION. It means
                // nothing has arrived yet, and a client that has finished and
                // gone looks exactly the same from here -- so the chip has to
                // be asked.
                //
                // Leaving that out was worth measuring: every finished
                // connection sat in this table until it timed out, all four
                // slots filled with the departed within a fifth of a second,
                // and a client opening a fresh connection each time went from
                // 54 milliseconds a page to 484.
                //
                // Asked once, after a tenth of a second of silence, because it
                // is an SPI transaction and a client that is coming back
                // usually already has. Still established after that and it may
                // stay until the two-second timeout.
                if (quiet > 100u && !conn[i].asked) {
                    conn[i].asked = true;
                    if (ubiqos_sock_state(conn[i].sock) != (int32_t)UBIQOS_TCP_ESTABLISHED) {
                        ubiqos_sock_close(conn[i].sock);
                        conn[i].sock = -1;
                        continue;
                    }
                }
                if (quiet > 2000u) {
                    ubiqos_sock_close(conn[i].sock);
                    conn[i].sock = -1;
                }
                continue;
            }

            worked = true;
            conn[i].quiet_since = now;

            // A request ends at its blank line, however long it is, and only
            // the first REQ_MAX bytes are kept -- the request line is in them,
            // and nothing past it is looked at. The rest is read and dropped.
            //
            // It used to end at the blank line OR a full buffer, and a browser's
            // request is 300 to 700 bytes of headers. The first 255 were served,
            // the rest arrived as a request of their own, and got a 405: two
            // answers to one question. On a kept-alive connection every answer
            // after that belonged to the request before it -- the sensor poll
            // was handed the status, the status the sensors, and a click on
            // "files on the card" either the 405 or somebody's JSON.
            for (int32_t k = 0; k < got; k++) {
                const char ch = (char)chunk[k];
                if (conn[i].n < REQ_MAX - 1) conn[i].req[conn[i].n++] = ch;
                if (ch == "\r\n\r\n"[conn[i].match]) conn[i].match++;
                else conn[i].match = (ch == '\r') ? 1 : 0;
                if (conn[i].match < 4) continue;

                conn[i].req[conn[i].n] = 0;
                last_request_ms = now;
                serve(conn[i].sock, conn[i].req);
                conn[i].n = 0;
                conn[i].match = 0;
                conn[i].served = true;
                conn[i].asked = false;
                conn[i].quiet_since = ubiqos_ticks_now();
            }
        }

        // Only when there was nothing to do anywhere. Fast while the board is
        // busy, slow when it is not: an idle server asks five times a second,
        // not two hundred.
        if (!worked) {
            uint32_t idle = ubiqos_ticks_now() - last_request_ms;
            ubiqos_sleep(idle < 1000u ? 5 : 200);
        }
    }
}

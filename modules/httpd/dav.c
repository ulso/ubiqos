#include "../../common/ubiqos_abi.h"

// WebDAV under /dav/, which is the card: /dav/notes/a.txt is /sd/notes/a.txt.
//
// What it is for is a Mac editing files on the board as files. Finder mounts
// it with Cmd-K and http://fruit-jam.local/dav/, nothing to install, and from
// then on any editor -- Zed was the one asked about -- opens and saves there
// as it would in a local folder, while the board goes on using the card. The
// other ways of getting files across both took the card away from the board
// (usbdisk) or needed a server on the Mac and a fetch on the board.
//
// The methods are the ones macOS's client uses, and no more: OPTIONS,
// PROPFIND (depth 0 and 1), GET and HEAD, PUT, DELETE, MKCOL, MOVE, COPY of a
// file, PROPPATCH (accepted and ignored), and LOCK and UNLOCK. LOCK is there
// because Finder mounts a server read-only unless it says it does class 2;
// the locks here are tokens and nothing else, which is honest for a server
// with one user.
//
// A password, always. Basic authentication sends it in the clear, which on a
// home network is the ordinary bargain of a printer's web page -- and the
// reason it is its own key and not ssh's: key set dav.password, and only the
// kernel ever compares it. The name given with it is ignored.
//
// Not kept: modification times. The card's file system call does not give
// them out, so every resource says it changed now; a client then never
// trusts a cached copy, which costs a fetch and cannot show a stale file.

bool http_send_all(int32_t sock, const uint8_t *p, uint32_t n);
void dav_log(const char *s, uint32_t n);

#define KEY_DAV  "dav.password"
#define IO_MAX   4096
#define PATH_MAX_DAV 192

// --- SMALL THINGS ------------------------------------------------------------

static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static bool same_ci(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

static void send_s(int32_t sock, const char *s) { http_send_all(sock, (const uint8_t *)s, slen(s)); }

static uint32_t dec(uint32_t v, char *out) {
    char t[11]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

// A header's value, or null. The name must start a line and be followed by a
// colon; case does not matter, as HTTP says.
static const char *header(const char *req, const char *name, uint32_t *len) {
    const uint32_t n = slen(name);
    for (const char *p = req; *p; p++) {
        if (p != req && p[-1] != '\n') continue;
        if (!same_ci(p, name, n) || p[n] != ':') continue;
        const char *v = p + n + 1;
        while (*v == ' ' || *v == '\t') v++;
        uint32_t k = 0;
        while (v[k] && v[k] != '\r' && v[k] != '\n') k++;
        *len = k;
        return v;
    }
    return 0;
}

static uint32_t header_u32(const char *req, const char *name, bool *have) {
    uint32_t len, v = 0;
    const char *p = header(req, name, &len);
    *have = p != 0;
    for (uint32_t i = 0; p && i < len && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10u + (uint32_t)(p[i] - '0');
    return v;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "/dav/a%20b/c.txt" -> "/sd/a b/c.txt". False for anything that tries to
// climb out (a ".." segment) or will not fit.
static bool to_fs(const char *url, uint32_t n, char *out) {
    // Past a scheme and host, for a Destination header.
    if (n > 7 && (same_ci(url, "http://", 7) || same_ci(url, "https://", 8))) {
        const char *p = url + (url[4] == 's' ? 8 : 7);
        while (p < url + n && *p != '/') p++;
        n -= (uint32_t)(p - url);
        url = p;
    }
    if (n < 4 || !same_ci(url, "/dav", 4) || (n > 4 && url[4] != '/')) return false;
    url += 4; n -= 4;
    uint32_t o = 0;
    const char *root = "/sd";
    while (*root) out[o++] = *root++;
    for (uint32_t i = 0; i < n && url[i] != '?'; i++) {
        char c = url[i];
        if (c == '%' && i + 2 < n && hexv(url[i + 1]) >= 0 && hexv(url[i + 2]) >= 0) {
            c = (char)(hexv(url[i + 1]) * 16 + hexv(url[i + 2]));
            i += 2;
        }
        if (o >= PATH_MAX_DAV - 2) return false;
        out[o++] = c;
    }
    while (o > 3 && out[o - 1] == '/') o--;      // "/sd/notes/" is "/sd/notes"
    out[o] = 0;
    // No "..", however it was spelt.
    for (uint32_t i = 0; i + 2 < o + 1; i++)
        if (out[i] == '/' && out[i + 1] == '.' && out[i + 2] == '.' && (out[i + 3] == '/' || out[i + 3] == 0))
            return false;
    return true;
}

// The other way, for an href: "/sd/a b" -> "/dav/a%20b".
static uint32_t to_href(const char *fs, char *out, uint32_t max) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t o = 0;
    for (const char *q = "/dav"; *q; q++) out[o++] = *q;
    for (const char *p = fs + 3; *p && o + 4 < max; p++) {
        const unsigned char c = (unsigned char)*p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    if (o == 4) out[o++] = '/';
    out[o] = 0;
    return o;
}

// Base64, for the Authorization header only.
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool authorised(const char *req) {
    uint32_t len;
    const char *v = header(req, "Authorization", &len);
    if (!v || len < 7 || !same_ci(v, "Basic ", 6)) return false;
    v += 6; len -= 6;
    uint8_t plain[96];
    uint32_t n = 0, acc = 0, bits = 0;
    for (uint32_t i = 0; i < len; i++) {
        const int d = b64v(v[i]);
        if (d < 0) break;
        acc = (acc << 6) | (uint32_t)d; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < sizeof plain) plain[n++] = (uint8_t)(acc >> bits); }
    }
    uint32_t colon = 0;
    while (colon < n && plain[colon] != ':') colon++;
    if (colon >= n) return false;

    ubiqos_keyreq_t r;
    uint8_t *z = (uint8_t *)&r;
    for (uint32_t i = 0; i < sizeof r; i++) z[i] = 0;
    for (uint32_t i = 0; KEY_DAV[i]; i++) r.name[i] = KEY_DAV[i];
    const uint32_t pn = n - colon - 1;
    if (pn == 0 || pn > sizeof r.value) return false;
    for (uint32_t i = 0; i < pn; i++) r.value[i] = plain[colon + 1 + i];
    r.len = pn;
    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_MATCH, &r);
    for (uint32_t i = 0; i < sizeof r.value; i++) r.value[i] = 0;
    for (uint32_t i = 0; i < sizeof plain; i++) plain[i] = 0;
    return rc == 0;
}

// --- REPLIES -----------------------------------------------------------------

static void reply(int32_t sock, const char *status, const char *extra, const char *body) {
    char head[400];
    uint32_t n = 0;
    const char *parts[] = { "HTTP/1.1 ", status, "\r\n", extra ? extra : "",
                            "Content-Type: text/plain; charset=utf-8\r\nContent-Length: " };
    for (uint32_t p = 0; p < 5; p++)
        for (const char *q = parts[p]; *q && n < sizeof head - 60; q++) head[n++] = *q;
    n += dec(body ? slen(body) : 0, head + n);
    for (const char *q = "\r\nConnection: keep-alive\r\n\r\n"; *q; q++) head[n++] = *q;
    http_send_all(sock, (const uint8_t *)head, n);
    if (body) send_s(sock, body);
    if (body && status[0] >= '4') { dav_log("  ", 2); dav_log(body, slen(body)); if (body[slen(body) - 1] != '\n') dav_log("\n", 1); }
}

// A body whose length is not known first goes out in chunks.
static void chunked_head(int32_t sock, const char *status, const char *type, const char *extra) {
    char first[48];
    uint32_t n = 0;
    for (const char *q = "HTTP/1.1 "; *q; q++) first[n++] = *q;
    for (const char *q = status; *q && n < sizeof first - 1; q++) first[n++] = *q;
    http_send_all(sock, (const uint8_t *)first, n);   // in one piece, so the log sees it
    send_s(sock, "\r\nContent-Type: ");
    send_s(sock, type);
    send_s(sock, "\r\n");
    if (extra) send_s(sock, extra);
    send_s(sock, "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");
}

static void chunk(int32_t sock, const char *s, uint32_t n) {
    if (!n) return;
    static const char hex[] = "0123456789abcdef";
    char h[12]; uint32_t k = 0;
    char t[8]; uint32_t m = 0;
    for (uint32_t v = n; v; v >>= 4) t[m++] = hex[v & 15];
    while (m) h[k++] = t[--m];
    h[k++] = '\r'; h[k++] = '\n';
    http_send_all(sock, (const uint8_t *)h, k);
    http_send_all(sock, (const uint8_t *)s, n);
    http_send_all(sock, (const uint8_t *)"\r\n", 2);
}

static void chunked_end(int32_t sock) { send_s(sock, "0\r\n\r\n"); }

// --- THE REQUEST BODY ------------------------------------------------------
//
// Some of it may have arrived with the headers, in the same receive; that part
// is handed in, and the rest is read from the socket. By length, or in chunks
// when the client says so -- macOS sends a large PUT chunked.

typedef struct {
    int32_t        sock;
    const uint8_t *pre;
    uint32_t       pre_n, pre_used;
    bool           chunked;
    uint32_t       left;          // of the body, or of the current chunk
    bool           done;
    bool           failed;
} body_t;

static int32_t raw_byte(body_t *b) {
    if (b->pre_used < b->pre_n) return b->pre[b->pre_used++];
    uint8_t c;
    for (uint32_t waited = 0; waited < 5000; ) {
        const int32_t got = ubiqos_sock_recv(b->sock, &c, 1);
        if (got == 1) return c;
        if (got < 0) break;
        ubiqos_sleep(1); waited++;
    }
    b->failed = true;
    return -1;
}

// Up to max bytes of the raw stream, from what came with the headers first.
static int32_t raw_read(body_t *b, uint8_t *buf, uint32_t max) {
    if (b->pre_used < b->pre_n) {
        uint32_t n = b->pre_n - b->pre_used;
        if (n > max) n = max;
        for (uint32_t i = 0; i < n; i++) buf[i] = b->pre[b->pre_used + i];
        b->pre_used += n;
        return (int32_t)n;
    }
    for (uint32_t waited = 0; waited < 5000; ) {
        const int32_t got = ubiqos_sock_recv(b->sock, buf, max);
        if (got > 0) return got;
        if (got < 0) break;
        ubiqos_sleep(1); waited++;
    }
    b->failed = true;
    return -1;
}

static bool next_chunk(body_t *b) {
    uint32_t size = 0;
    int32_t c;
    bool any = false;
    while ((c = raw_byte(b)) >= 0 && hexv((char)c) >= 0) { size = size * 16u + (uint32_t)hexv((char)c); any = true; }
    while (c >= 0 && c != '\n') c = raw_byte(b);            // extensions, then CRLF
    if (c < 0 || !any) { b->failed = true; return false; }
    if (size == 0) {                                        // the trailer: up to a blank line
        uint32_t run = 0;
        while ((c = raw_byte(b)) >= 0) {
            if (c == '\n') { if (run == 0) break; run = 0; }
            else if (c != '\r') run++;
        }
        b->done = true;
        return false;
    }
    b->left = size;
    return true;
}

// The next piece of the body; 0 at its end, -1 if it broke off.
static int32_t body_read(body_t *b, uint8_t *buf, uint32_t max) {
    if (b->done) return 0;
    if (b->chunked && b->left == 0) {
        if (!next_chunk(b)) return b->failed ? -1 : 0;
    }
    if (b->left == 0) { b->done = true; return 0; }
    if (max > b->left) max = b->left;
    const int32_t got = raw_read(b, buf, max);
    if (got <= 0) return -1;
    b->left -= (uint32_t)got;
    if (b->chunked && b->left == 0) {                       // the CRLF after a chunk
        int32_t c = raw_byte(b);
        if (c == '\r') c = raw_byte(b);
        if (c != '\n') b->failed = true;
    } else if (!b->chunked && b->left == 0) {
        b->done = true;
    }
    return got;
}

static void body_skip(body_t *b) {
    uint8_t buf[128];
    while (body_read(b, buf, sizeof buf) > 0) { }
}

// --- THE FILE SYSTEM -------------------------------------------------------

static int32_t attr_of(const char *path, uint32_t *size) {
    uint32_t s = 0;
    const int32_t a = ubiqos_fs_stat(path, &s);
    if (size) *size = s;
    return a;
}

static bool is_dir(int32_t attr) { return attr >= 0 && (attr & UBIQOS_ATTR_DIRECTORY); }

static void join(const char *dir, const char *name, char *out) {
    uint32_t o = 0;
    for (const char *p = dir; *p && o < PATH_MAX_DAV - 2; p++) out[o++] = *p;
    if (o && out[o - 1] != '/') out[o++] = '/';
    for (const char *p = name; *p && o < PATH_MAX_DAV - 1; p++) out[o++] = *p;
    out[o] = 0;
}

// The entry at index, as a name to show; false at the end. Skips . and ..
static bool entry(const char *dir, uint32_t *index, char *name, uint32_t *size) {
    char raw[UBIQOS_DIRNAME_MAX];
    for (;;) {
        if (ubiqos_fs_dir_at(dir, (*index)++, raw, size) < 0) return false;
        ubiqos_pretty_name(raw, name);
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        return true;
    }
}

// A directory and everything in it, which is what DELETE of a collection
// means. Depth-limited, because a module's stack is not a thing to recurse on
// without one.
static bool remove_tree(const char *path, uint32_t depth) {
    const int32_t a = attr_of(path, 0);
    if (a < 0) return false;
    if (!is_dir(a)) return ubiqos_fs_remove(path) == 0;
    if (depth > 6) return false;
    char name[UBIQOS_DIRNAME_MAX], child[PATH_MAX_DAV];
    for (;;) {
        uint32_t i = 0, size;
        if (!entry(path, &i, name, &size)) break;     // always the first: the list shrinks
        join(path, name, child);
        if (!remove_tree(child, depth + 1)) return false;
    }
    return ubiqos_rmdir(path) == 0;
}

// --- DATES -------------------------------------------------------------------
//
// "Sun, 22 Nov 2026 16:44:09 GMT". The card's file system call gives no times
// out (and the FAT library stamps none in), so every resource says the same
// one: when the board started, from /var/time less /var/uptime. It said "now"
// at first, which was honest and slow -- a client that is told a file changed
// a moment ago trusts no copy of it, and Finder fetched the card's 1.9 MB
// sound files again for every preview while everything else waited. A fixed
// time lets it keep what it has fetched. The price is that a file changed on
// the board itself, keeping its size, is not noticed until the next boot.
// It is local time and says GMT; for a time that only has to stay put, an
// hour does not matter.

static uint32_t num2(const char *p, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = v * 10u + (uint32_t)(p[i] - '0');
    return v;
}

// Howard Hinnant's, as uptime has them.
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m > 2 ? m - 3 : m + 9) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, uint32_t *y, uint32_t *m, uint32_t *d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (uint32_t)((int32_t)yoe + era * 400) + (*m <= 2);
}

static uint32_t read_small(const char *path, char *out, uint32_t cap) {
    const int32_t fd = ubiqos_open_flags(path, UBIQOS_O_RDONLY);
    if (fd < 0) return 0;
    const int32_t r = ubiqos_read(fd, (uint8_t *)out, cap - 1);
    ubiqos_close(fd);
    const uint32_t got = r > 0 ? (uint32_t)r : 0;
    out[got] = 0;
    return got;
}

static void http_date(char *out) {
    char t[32], up[16];
    uint32_t y = 2026, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    if (read_small("/var/time", t, sizeof t) >= 19 && t[4] == '-' && t[0] >= '0' && t[0] <= '9') {
        uint32_t secs = 0;
        read_small("/var/uptime", up, sizeof up);
        for (const char *p = up; *p >= '0' && *p <= '9'; p++) secs = secs * 10u + (uint32_t)(*p - '0');
        const int32_t days = days_from_civil((int32_t)num2(t, 4), num2(t + 5, 2), num2(t + 8, 2));
        uint32_t when = (uint32_t)days * 86400u + num2(t + 11, 2) * 3600u + num2(t + 14, 2) * 60u
                      + num2(t + 17, 2) - secs;
        civil_from_days((int32_t)(when / 86400u), &y, &mo, &d);
        when %= 86400u;
        h = when / 3600u; mi = when / 60u % 60u; s = when % 60u;
    }
    // Sakamoto's day of the week.
    static const uint8_t k[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint32_t yy = mo < 3 ? y - 1 : y;
    const uint32_t wd = (yy + yy / 4 - yy / 100 + yy / 400 + k[(mo - 1) % 12] + d) % 7;
    static const char days[] = "SunMonTueWedThuFriSat";
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    uint32_t o = 0;
    for (uint32_t i = 0; i < 3; i++) out[o++] = days[wd * 3 + i];
    out[o++] = ','; out[o++] = ' ';
    out[o++] = (char)('0' + d / 10); out[o++] = (char)('0' + d % 10); out[o++] = ' ';
    for (uint32_t i = 0; i < 3; i++) out[o++] = mons[((mo - 1) % 12) * 3 + i];
    out[o++] = ' ';
    o += dec(y, out + o);
    out[o++] = ' ';
    out[o++] = (char)('0' + h / 10); out[o++] = (char)('0' + h % 10); out[o++] = ':';
    out[o++] = (char)('0' + mi / 10); out[o++] = (char)('0' + mi % 10); out[o++] = ':';
    out[o++] = (char)('0' + s / 10); out[o++] = (char)('0' + s % 10);
    for (const char *q = " GMT"; *q; q++) out[o++] = *q;
    out[o] = 0;
}

// --- PROPFIND ----------------------------------------------------------------

static uint32_t put(char *out, uint32_t o, uint32_t max, const char *s) {
    while (*s && o < max - 1) out[o++] = *s++;
    out[o] = 0;
    return o;
}

// XML text: the five characters that matter.
static uint32_t put_text(char *out, uint32_t o, uint32_t max, const char *s) {
    for (; *s && o < max - 7; s++) {
        switch (*s) {
        case '&': o = put(out, o, max, "&amp;"); break;
        case '<': o = put(out, o, max, "&lt;"); break;
        case '>': o = put(out, o, max, "&gt;"); break;
        case '"': o = put(out, o, max, "&quot;"); break;
        default:  out[o++] = *s; out[o] = 0;
        }
    }
    return o;
}

static void prop_response(int32_t sock, const char *fs, const char *name, bool dir, uint32_t size,
                          const char *date) {
    char x[700], href[PATH_MAX_DAV * 3 + 8], num[12];
    to_href(fs, href, sizeof href);
    uint32_t o = 0;
    o = put(x, o, sizeof x, "<D:response><D:href>");
    o = put_text(x, o, sizeof x, href);
    if (dir && href[slen(href) - 1] != '/') o = put(x, o, sizeof x, "/");
    o = put(x, o, sizeof x, "</D:href><D:propstat><D:prop><D:displayname>");
    o = put_text(x, o, sizeof x, name);
    o = put(x, o, sizeof x, "</D:displayname><D:resourcetype>");
    if (dir) o = put(x, o, sizeof x, "<D:collection/>");
    o = put(x, o, sizeof x, "</D:resourcetype>");
    if (!dir) {
        dec(size, num);
        o = put(x, o, sizeof x, "<D:getcontentlength>");
        o = put(x, o, sizeof x, num);
        o = put(x, o, sizeof x, "</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype>");
    }
    o = put(x, o, sizeof x, "<D:getlastmodified>");
    o = put(x, o, sizeof x, date);
    o = put(x, o, sizeof x, "</D:getlastmodified><D:supportedlock><D:lockentry><D:lockscope><D:exclusive/>"
                            "</D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>"
                            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
    chunk(sock, x, o);
}

static void do_propfind(int32_t sock, const char *req, const char *fs) {
    uint32_t size;
    const int32_t a = attr_of(fs, &size);
    if (a < 0 && !(fs[3] == 0)) { reply(sock, "404 Not Found", 0, "not found"); return; }
    const bool dir = fs[3] == 0 || is_dir(a);

    uint32_t dlen;
    const char *depth = header(req, "Depth", &dlen);
    const bool one = !depth || depth[0] != '0';     // 1 and infinity both get one level

    char date[40];
    http_date(date);
    chunked_head(sock, "207 Multi-Status", "application/xml; charset=utf-8", 0);
    static const char top[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
    chunk(sock, top, sizeof top - 1);

    const char *base = fs;
    for (const char *p = fs; *p; p++) if (*p == '/' && p[1]) base = p + 1;
    prop_response(sock, fs, fs[3] == 0 ? "sd" : base, dir, size, date);

    if (dir && one) {
        char name[UBIQOS_DIRNAME_MAX], child[PATH_MAX_DAV];
        uint32_t i = 0, csize;
        while (entry(fs, &i, name, &csize)) {
            join(fs, name, child);
            const int32_t ca = attr_of(child, 0);
            prop_response(sock, child, name, is_dir(ca), csize, date);
        }
    }
    static const char end[] = "</D:multistatus>";
    chunk(sock, end, sizeof end - 1);
    chunked_end(sock);
}

// --- THE REST ----------------------------------------------------------------

static void do_get(int32_t sock, const char *fs, bool head) {
    uint32_t size;
    const int32_t a = attr_of(fs, &size);
    if (a < 0) { reply(sock, "404 Not Found", 0, "not found"); return; }
    if (is_dir(a)) { reply(sock, "200 OK", 0, head ? 0 : "a directory: mount it, do not browse it\n"); return; }
    char date[40], hdr[200];
    http_date(date);
    uint32_t o = 0;
    o = put(hdr, o, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nLast-Modified: ");
    o = put(hdr, o, sizeof hdr, date);
    o = put(hdr, o, sizeof hdr, "\r\nContent-Length: ");
    o += dec(size, hdr + o);
    o = put(hdr, o, sizeof hdr, "\r\nConnection: keep-alive\r\n\r\n");
    http_send_all(sock, (const uint8_t *)hdr, o);
    if (head) return;
    const int32_t fd = ubiqos_open_flags(fs, UBIQOS_O_RDONLY);
    if (fd < 0) return;
    uint8_t buf[IO_MAX];
    for (;;) {
        const int32_t got = ubiqos_read(fd, buf, sizeof buf);
        if (got <= 0) break;
        if (!http_send_all(sock, buf, (uint32_t)got)) break;
    }
    ubiqos_close(fd);
}

// Written to a name beside the target and moved over it at the end, so that
// an upload that breaks off leaves the file as it was rather than half of it.
// The FAT rename will not replace a file, so the old one is removed first.
static void do_put(int32_t sock, body_t *b, const char *fs) {
    uint32_t size;
    const int32_t a = attr_of(fs, &size);
    if (is_dir(a)) { body_skip(b); reply(sock, "405 Method Not Allowed", 0, "a directory\n"); return; }
    char tmp[PATH_MAX_DAV + 8];
    uint32_t n = 0;
    for (const char *p = fs; *p && n < PATH_MAX_DAV; p++) tmp[n++] = *p;
    for (const char *q = ".dav~"; *q; q++) tmp[n++] = *q;
    tmp[n] = 0;
    ubiqos_fs_remove(tmp);
    const int32_t fd = ubiqos_open_flags(tmp, UBIQOS_O_WRONLY | UBIQOS_O_CREAT | UBIQOS_O_TRUNC);
    if (fd < 0) { body_skip(b); reply(sock, "409 Conflict", 0, "cannot create it -- is the directory there?\n"); return; }
    uint8_t buf[IO_MAX];
    bool ok = true;
    for (;;) {
        const int32_t got = body_read(b, buf, sizeof buf);
        if (got == 0) break;
        if (got < 0) { ok = false; break; }
        if (ubiqos_write(fd, buf, (uint32_t)got) != got) { ok = false; body_skip(b); break; }
    }
    ubiqos_close(fd);
    if (!ok || b->failed) { ubiqos_fs_remove(tmp); reply(sock, "500 Internal Server Error", 0, "the upload broke off\n"); return; }
    if (a >= 0) ubiqos_fs_remove(fs);
    if (ubiqos_fs_rename(tmp, fs) != 0) { reply(sock, "500 Internal Server Error", 0, "could not put it in place\n"); return; }
    reply(sock, a >= 0 ? "204 No Content" : "201 Created", 0, 0);
}

static bool copy_file(const char *from, const char *to) {
    const int32_t in = ubiqos_open_flags(from, UBIQOS_O_RDONLY);
    if (in < 0) return false;
    const int32_t out = ubiqos_open_flags(to, UBIQOS_O_WRONLY | UBIQOS_O_CREAT | UBIQOS_O_TRUNC);
    if (out < 0) { ubiqos_close(in); return false; }
    uint8_t buf[IO_MAX];
    bool ok = true;
    for (;;) {
        const int32_t got = ubiqos_read(in, buf, sizeof buf);
        if (got <= 0) break;
        if (ubiqos_write(out, buf, (uint32_t)got) != got) { ok = false; break; }
    }
    ubiqos_close(in);
    ubiqos_close(out);
    return ok;
}

static void do_move_copy(int32_t sock, const char *req, const char *fs, bool move) {
    uint32_t len;
    const char *d = header(req, "Destination", &len);
    char to[PATH_MAX_DAV];
    if (!d || !to_fs(d, len, to) || to[3] == 0) { reply(sock, "400 Bad Request", 0, "no usable Destination\n"); return; }
    const int32_t a = attr_of(fs, 0);
    if (a < 0) { reply(sock, "404 Not Found", 0, "not found"); return; }
    const char *ow = header(req, "Overwrite", &len);
    const bool overwrite = !ow || (ow[0] != 'F' && ow[0] != 'f');
    const int32_t ta = attr_of(to, 0);
    if (ta >= 0 && !overwrite) { reply(sock, "412 Precondition Failed", 0, "it is there already\n"); return; }
    if (ta >= 0 && !remove_tree(to, 0)) { reply(sock, "409 Conflict", 0, "could not replace it\n"); return; }
    bool ok;
    if (move) ok = ubiqos_fs_rename(fs, to) == 0;
    else if (is_dir(a)) { reply(sock, "403 Forbidden", 0, "copying a directory is not done here\n"); return; }
    else ok = copy_file(fs, to);
    if (!ok) { reply(sock, "409 Conflict", 0, "could not do it\n"); return; }
    reply(sock, ta >= 0 ? "204 No Content" : "201 Created", 0, 0);
}

static void do_lock(int32_t sock, const char *fs) {
    // A lock on a name that is not there makes the file, empty: that is what
    // the client is about to write, and what RFC 4918 says a lock does.
    bool made = false;
    if (attr_of(fs, 0) < 0) {
        const int32_t fd = ubiqos_open_flags(fs, UBIQOS_O_WRONLY | UBIQOS_O_CREAT);
        if (fd >= 0) { ubiqos_close(fd); made = true; }
    }
    char token[48], num[12];
    uint32_t o = 0;
    o = put(token, o, sizeof token, "opaquelocktoken:ubiqos-");
    dec(ubiqos_ticks_now(), num);
    o = put(token, o, sizeof token, num);
    char extra[96];
    o = 0;
    o = put(extra, o, sizeof extra, "Lock-Token: <");
    o = put(extra, o, sizeof extra, token);
    o = put(extra, o, sizeof extra, ">\r\n");
    char x[520];
    o = 0;
    o = put(x, o, sizeof x, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>"
                            "<D:activelock><D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/>"
                            "</D:lockscope><D:depth>0</D:depth><D:timeout>Second-3600</D:timeout>"
                            "<D:locktoken><D:href>");
    o = put(x, o, sizeof x, token);
    o = put(x, o, sizeof x, "</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>");
    chunked_head(sock, made ? "201 Created" : "200 OK", "application/xml; charset=utf-8", extra);
    chunk(sock, x, o);
    chunked_end(sock);
}

static void do_proppatch(int32_t sock, const char *fs) {
    if (attr_of(fs, 0) < 0 && fs[3] != 0) { reply(sock, "404 Not Found", 0, "not found"); return; }
    char href[PATH_MAX_DAV * 3 + 8], x[600];
    to_href(fs, href, sizeof href);
    uint32_t o = 0;
    o = put(x, o, sizeof x, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    o = put_text(x, o, sizeof x, href);
    o = put(x, o, sizeof x, "</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                            "</D:response></D:multistatus>");
    chunked_head(sock, "207 Multi-Status", "application/xml; charset=utf-8", 0);
    chunk(sock, x, o);
    chunked_end(sock);
}

// --- THE ENTRY ---------------------------------------------------------------
//
// Called for any request whose path is /dav or under it. pre is what arrived
// after the headers in the same receive; *used says how much of it was this
// request's body, so the caller can go on from there.

void dav_serve(int32_t sock, const char *req, const uint8_t *pre, uint32_t pre_n, uint32_t *used) {
    *used = 0;

    char method[12];
    uint32_t m = 0;
    while (req[m] && req[m] != ' ' && m < sizeof method - 1) { method[m] = req[m]; m++; }
    method[m] = 0;
    const char *url = req + m + (req[m] == ' ');
    uint32_t ulen = 0;
    while (url[ulen] && url[ulen] != ' ' && url[ulen] != '\r') ulen++;

    // The request line and the headers that decide anything, to the log.
    {
        dav_log("> ", 2);
        dav_log(req, m + 1 + ulen);
        static const char *const keep[] = { "Depth", "Content-Length", "Transfer-Encoding", "Expect",
                                            "Destination", "Overwrite", "If", "Lock-Token", "Timeout" };
        for (uint32_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
            uint32_t hl;
            const char *hv = header(req, keep[i], &hl);
            if (!hv) continue;
            dav_log(" ", 1); dav_log(keep[i], slen(keep[i])); dav_log("=", 1);
            dav_log(hv, hl > 80 ? 80 : hl);
        }
        dav_log("\n", 1);
    }

    bool have_len;
    const uint32_t clen = header_u32(req, "Content-Length", &have_len);
    uint32_t tlen;
    const char *te = header(req, "Transfer-Encoding", &tlen);
    body_t b = { sock, pre, pre_n, 0, false, 0, false, false };
    if (te && tlen >= 7 && same_ci(te, "chunked", 7)) b.chunked = true;
    else { b.left = have_len ? clen : 0; if (!b.left) b.done = true; }

    // OPTIONS needs no password: it is how a client learns there is DAV here at
    // all, and Finder asks it before it asks for one.
    if (same_ci(method, "OPTIONS", 8)) {
        body_skip(&b);
        reply(sock, "200 OK",
              "DAV: 1, 2\r\nMS-Author-Via: DAV\r\n"
              "Allow: OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK\r\n",
              0);
        *used = b.pre_used;
        return;
    }

    if (!authorised(req)) {
        body_skip(&b);
        ubiqos_sleep(500);                 // a guesser pays for each guess
        reply(sock, "401 Unauthorized", "WWW-Authenticate: Basic realm=\"UbiqOS card\"\r\n",
              "the password is the key dav.password (key set dav.password)\n");
        *used = b.pre_used;
        return;
    }

    char fs[PATH_MAX_DAV];
    // A client that asked to be told before it sends a body is told now, and
    // only now that it has proved who it is.
    uint32_t elen;
    const char *ex = header(req, "Expect", &elen);
    if (ex && elen >= 12 && same_ci(ex, "100-continue", 12) && !b.done)
        send_s(sock, "HTTP/1.1 100 Continue\r\n\r\n");

    if (!to_fs(url, ulen, fs)) {
        body_skip(&b);
        reply(sock, "400 Bad Request", 0, "not a path on the card\n");
        *used = b.pre_used;
        return;
    }

    if      (same_ci(method, "PROPFIND", 9))  { body_skip(&b); do_propfind(sock, req, fs); }
    else if (same_ci(method, "GET", 4))       { body_skip(&b); do_get(sock, fs, false); }
    else if (same_ci(method, "HEAD", 5))      { body_skip(&b); do_get(sock, fs, true); }
    else if (same_ci(method, "PUT", 4))       { do_put(sock, &b, fs); }
    else if (same_ci(method, "DELETE", 7)) {
        body_skip(&b);
        if (fs[3] == 0) reply(sock, "403 Forbidden", 0, "not the whole card\n");
        else if (attr_of(fs, 0) < 0) reply(sock, "404 Not Found", 0, "not found");
        else if (remove_tree(fs, 0)) reply(sock, "204 No Content", 0, 0);
        else reply(sock, "409 Conflict", 0, "could not remove all of it\n");
    }
    else if (same_ci(method, "MKCOL", 6)) {
        const bool had_body = !b.done;
        body_skip(&b);
        if (had_body) reply(sock, "415 Unsupported Media Type", 0, 0);
        else if (attr_of(fs, 0) >= 0) reply(sock, "405 Method Not Allowed", 0, "it is there already\n");
        else if (ubiqos_mkdir(fs) == 0) reply(sock, "201 Created", 0, 0);
        else reply(sock, "409 Conflict", 0, "could not make it -- is its parent there?\n");
    }
    else if (same_ci(method, "MOVE", 5))      { body_skip(&b); do_move_copy(sock, req, fs, true); }
    else if (same_ci(method, "COPY", 5))      { body_skip(&b); do_move_copy(sock, req, fs, false); }
    else if (same_ci(method, "LOCK", 5))      { body_skip(&b); do_lock(sock, fs); }
    else if (same_ci(method, "UNLOCK", 7))    { body_skip(&b); reply(sock, "204 No Content", 0, 0); }
    else if (same_ci(method, "PROPPATCH", 10)) { body_skip(&b); do_proppatch(sock, fs); }
    else { body_skip(&b); reply(sock, "405 Method Not Allowed", 0, "not here\n"); }

    *used = b.pre_used;
}

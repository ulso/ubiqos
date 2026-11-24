#include "tui.h"
#include "plat.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t ch;
    uint8_t  attr;
} cell_t;

int scr_w, scr_h;
static cell_t *back, *front;         // what is drawn, what the terminal shows
static int cur_x, cur_y;
static bool cur_show;
// The terminal's alternate screen, as a full-screen program uses it: what the
// shell had on the screen comes back untouched when Blixt leaves, and nothing
// the terminal drew in the margin for those lines (macOS Terminal's marks)
// shows through. The board's console reads past the sequence and has one
// screen, which is why leaving also clears it.
static bool alt_screen;

static char *out;
static int   out_len, out_cap;

static void emit(const char *s, int n)
{
    if (out_len + n > out_cap) {
        int c = out_cap ? out_cap * 2 : 16384;
        while (c < out_len + n) c *= 2;
        char *p = realloc(out, (size_t)c);
        if (!p) return;
        out = p;
        out_cap = c;
    }
    memcpy(out + out_len, s, (size_t)n);
    out_len += n;
}

static void emits(const char *s) { emit(s, (int)strlen(s)); }

static void emit_num(int v)
{
    char b[12];
    int n = 0;
    if (v < 0) v = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) emit(&b[--n], 1);
}

static void emit_utf8(uint32_t c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
    else if (c < 0x10000) {
        b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (char)(0x80 | (c & 0x3f)); n = 3;
    } else {
        b[0] = (char)(0xf0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); n = 4;
    }
    emit(b, n);
}

void tui_resize(int rows, int cols)
{
    if (rows < 10) rows = 10;
    if (cols < 40) cols = 40;
    if (rows > 120) rows = 120;
    if (cols > 300) cols = 300;
    free(back);
    free(front);
    scr_w = cols;
    scr_h = rows;
    back = calloc((size_t)(rows * cols), sizeof *back);
    front = calloc((size_t)(rows * cols), sizeof *front);
}

void tui_init(void)
{
    if (!back) tui_resize(24, 80);
}

void tui_done(void)
{
    out_len = 0;
    emits("\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l");
    alt_screen = false;
    plat_write(out, out_len);
    out_len = 0;
}

void tui_put(int x, int y, uint32_t ch, uint8_t attr)
{
    if (x < 0 || y < 0 || x >= scr_w || y >= scr_h) return;
    cell_t *c = &back[y * scr_w + x];
    c->ch = ch;
    c->attr = attr;
}

void tui_fill(int x, int y, int w, int h, uint32_t ch, uint8_t attr)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) tui_put(x + i, y + j, ch, attr);
}

static uint32_t next_cp(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    int more = 0;
    if (c >= 0xf0) { c &= 7; more = 3; }
    else if (c >= 0xe0) { c &= 15; more = 2; }
    else if (c >= 0xc0) { c &= 31; more = 1; }
    while (more-- && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
    *ps = (const char *)s;
    return c;
}

int tui_text(int x, int y, const char *s, uint8_t attr, int max)
{
    int n = 0;
    while (*s && n < max) {
        tui_put(x + n, y, next_cp(&s), attr);
        n++;
    }
    return n;
}

int tui_hot(int x, int y, const char *s, uint8_t attr, uint8_t hot_attr)
{
    int n = 0;
    bool hot = false;
    while (*s) {
        if (*s == '~') { hot = !hot; s++; continue; }
        tui_put(x + n, y, next_cp(&s), hot ? hot_attr : attr);
        n++;
    }
    return n;
}

void tui_box(int x, int y, int w, int h, bool dbl, uint8_t attr)
{
    uint32_t hz = dbl ? G_DH : G_H, vt = dbl ? G_DV : G_V;
    for (int i = 1; i < w - 1; i++) { tui_put(x + i, y, hz, attr); tui_put(x + i, y + h - 1, hz, attr); }
    for (int j = 1; j < h - 1; j++) { tui_put(x, y + j, vt, attr); tui_put(x + w - 1, y + j, vt, attr); }
    tui_put(x, y, dbl ? G_DTL : G_TL, attr);
    tui_put(x + w - 1, y, dbl ? G_DTR : G_TR, attr);
    tui_put(x, y + h - 1, dbl ? G_DBL : G_BL, attr);
    tui_put(x + w - 1, y + h - 1, dbl ? G_DBR : G_BR, attr);
}

// What lies under a window's shadow stays readable, darkened: the Borland look.
void tui_shadow(int x, int y, int w, int h)
{
    for (int j = 1; j <= h; j++)
        for (int i = 0; i < 2; i++) {
            int px = x + w + i, py = y + j;
            if (px < scr_w && py < scr_h) back[py * scr_w + px].attr = ATTR(C_DGREY, C_BLACK);
        }
    for (int i = 2; i < w + 2; i++) {
        int px = x + i, py = y + h;
        if (px < scr_w && py < scr_h) back[py * scr_w + px].attr = ATTR(C_DGREY, C_BLACK);
    }
}

void tui_cursor(int x, int y, bool show)
{
    cur_x = x;
    cur_y = y;
    cur_show = show;
}

static void sgr(uint8_t a)
{
    int fg = a & 15, bg = a >> 4;
    emits("\x1b[");
    emit_num(fg < 8 ? 30 + fg : 90 + fg - 8);
    emit(";", 1);
    emit_num(bg < 8 ? 40 + bg : 100 + bg - 8);
    emit("m", 1);
}

static void move(int x, int y)
{
    emits("\x1b[");
    emit_num(y + 1);
    emit(";", 1);
    emit_num(x + 1);
    emit("H", 1);
}

void tui_flush(bool full)
{
    out_len = 0;
    if (!alt_screen) {
        emits("\x1b[?1049h");
        alt_screen = true;
        full = true;
    }
    emits("\x1b[?25l");
    int last_attr = -1;
    if (full) emits("\x1b[0m");
    for (int y = 0; y < scr_h; y++) {
        int px = -1;                       // where the terminal's cursor is, -1 unknown
        for (int x = 0; x < scr_w; x++) {
            cell_t *b = &back[y * scr_w + x], *f = &front[y * scr_w + x];
            if (!full && b->ch == f->ch && b->attr == f->attr) continue;
            // A short gap of unchanged cells is cheaper to write again than to
            // jump over: a jump is seven or eight bytes.
            if (px != x) {
                bool bridged = false;
                if (px >= 0 && x - px <= 4) {
                    bridged = true;
                    for (int k = px; k < x; k++) {
                        cell_t *g = &back[y * scr_w + k];
                        if (g->attr != last_attr) { sgr(g->attr); last_attr = g->attr; }
                        emit_utf8(g->ch ? g->ch : ' ');
                    }
                }
                if (!bridged) move(x, y);
            }
            if (b->attr != last_attr) { sgr(b->attr); last_attr = b->attr; }
            emit_utf8(b->ch ? b->ch : ' ');
            *f = *b;
            // After the last column the cursor waits to wrap; never write past it.
            px = x + 1 < scr_w ? x + 1 : -1;
        }
    }
    move(cur_x, cur_y);
    if (cur_show) emits("\x1b[?25h");
    plat_write(out, out_len);
    out_len = 0;
}

// --- keys ------------------------------------------------------------------

static uint8_t inq[512];
static int inq_head, inq_len;

static int getb(int timeout_ms)
{
    if (!inq_len) {
        uint8_t b[128];
        int n = plat_read(b, (int)sizeof b, timeout_ms);
        if (n <= 0) return -1;
        for (int i = 0; i < n && inq_len < (int)sizeof inq; i++) {
            inq[(inq_head + inq_len) % (int)sizeof inq] = b[i];
            inq_len++;
        }
    }
    int c = inq[inq_head];
    inq_head = (inq_head + 1) % (int)sizeof inq;
    inq_len--;
    return c;
}

#define ESC_WAIT 60

static int csi_tilde(int p0, int p1)
{
    int k;
    switch (p0) {
    case 1: case 7: return K_HOME;
    case 4: case 8: return K_END;
    case 2: return K_INS;
    case 3: return K_DEL;
    case 5: return K_PGUP;
    case 6: return K_PGDN;
    case 11: k = K_F1; break;  case 12: k = K_F2; break;
    case 13: k = K_F3; break;  case 14: k = K_F4; break;
    case 15: k = K_F5; break;  case 17: k = K_F6; break;
    case 18: k = K_F7; break;  case 19: k = K_F8; break;
    case 20: k = K_F9; break;  case 21: k = K_F10; break;
    case 23: k = K_F11; break; case 24: k = K_F12; break;
    default: return K_NONE;
    }
    // A modifier of 3 is Alt (xterm's 1 + 2).
    return (p1 == 3 || p1 == 4) ? (K_ALTF | k) : k;
}

static int read_escape(void)
{
    int c = getb(ESC_WAIT);
    if (c < 0 || c == 0x1b) return K_ESC;
    if (c == 'O') {
        c = getb(ESC_WAIT);
        switch (c) {
        case 'P': return K_F1; case 'Q': return K_F2; case 'R': return K_F3; case 'S': return K_F4;
        case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
        case 'H': return K_HOME; case 'F': return K_END;
        }
        return K_NONE;
    }
    if (c != '[') {
        if (c >= 'A' && c <= 'Z') c += 32;
        return K_ALT | c;
    }
    int p[4] = { 0, 0, 0, 0 }, np = 0;
    for (;;) {
        c = getb(ESC_WAIT);
        if (c < 0) return K_NONE;
        if (c >= '0' && c <= '9') { p[np] = p[np] * 10 + (c - '0'); continue; }
        if (c == ';') { if (np < 3) np++; continue; }
        if (c == '?' || c == '>') continue;
        break;
    }
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'P': return np ? (K_ALTF | K_F1) : K_F1;
    case 'Q': return np ? (K_ALTF | K_F2) : K_F2;
    case 'R': return K_NONE;              // a cursor report nobody asked for
    case '~': return csi_tilde(p[0], p[1]);
    case 't':
        // ESC [ 8 ; rows ; cols t: the terminal's size, asked for or sent by
        // sshd when the window at the far end changes.
        if (p[0] == 8 && np >= 2 && p[1] >= 4 && p[2] >= 10) {
            tui_resize(p[1], p[2]);
            return K_RESIZE;
        }
        return K_NONE;
    }
    return K_NONE;
}

int tui_key(void)
{
    for (;;) {
        int c = getb(-1);
        if (c < 0) continue;
        if (c == 0x1b) {
            int k = read_escape();
            if (k != K_NONE) return k;
            continue;
        }
        if (c < 0x80) return c;
        // UTF-8: gather the rest of the character.
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        uint32_t cp = (uint32_t)c & (more == 3 ? 7 : more == 2 ? 15 : 31);
        if (!more) return c;
        while (more--) {
            int d = getb(ESC_WAIT);
            if (d < 0 || (d & 0xc0) != 0x80) return '?';
            cp = (cp << 6) | ((uint32_t)d & 0x3f);
        }
        return (int)cp;
    }
}

bool tui_ask_size(int *rows, int *cols)
{
    plat_write("\x1b[18t", 5);
    uint32_t start = plat_ms();
    // Bytes that are not the answer are typed keys; they are kept in order.
    uint8_t keep[64];
    int nkeep = 0;
    bool ok = false;
    int state = 0, p[3] = { 0, 0, 0 }, np = 0;
    while (plat_ms() - start < 250) {
        int c = getb(20);
        if (c < 0) continue;
        if (state == 0) {
            if (c == 0x1b) { state = 1; continue; }
            if (nkeep < (int)sizeof keep) keep[nkeep++] = (uint8_t)c;
            continue;
        }
        if (state == 1) {
            if (c == '[') { state = 2; np = 0; p[0] = p[1] = p[2] = 0; continue; }
            state = 0;
            continue;
        }
        if (c >= '0' && c <= '9') { p[np] = p[np] * 10 + (c - '0'); continue; }
        if (c == ';') { if (np < 2) np++; continue; }
        if (c == 't' && p[0] == 8 && np >= 2) { *rows = p[1]; *cols = p[2]; ok = true; break; }
        state = 0;
    }
    // Put the typed bytes back at the front of the queue.
    for (int i = nkeep - 1; i >= 0; i--) {
        inq_head = (inq_head + (int)sizeof inq - 1) % (int)sizeof inq;
        inq[inq_head] = keep[i];
        inq_len++;
    }
    return ok;
}

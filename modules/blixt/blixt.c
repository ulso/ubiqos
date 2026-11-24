// Blixt -- a Thumb-2 assembler with an editor, in the manner of the Borland
// IDEs: a menu bar on top, the source in a blue window, the keys on the bottom
// line, and F9 to assemble and run what is on the screen.
//
//     blixt [FILE]
//
// F9 assembles, writes the program as a module to /tmp and runs it with the
// terminal its own; when it ends, a key brings the editor back. F4 shows the
// listing beside the source -- address and code for every line -- and keeps
// it current as you type, errors included.
//
// It is written against plat.h, so the same editor runs in a terminal on a
// Mac, which is where most of it was tried.
#include "asm.h"
#include "modwrite.h"
#include "plat.h"
#include "tui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLIXT_VERSION "0.1"

// --- colours, the Borland ones ----------------------------------------------

#define A_MENU     ATTR(C_BLACK, C_LGREY)
#define A_MENU_HOT ATTR(C_RED, C_LGREY)
#define A_MENU_SEL ATTR(C_BLACK, C_GREEN)
#define A_MENU_SEL_HOT ATTR(C_RED, C_GREEN)
#define A_MENU_DIS ATTR(C_DGREY, C_LGREY)
#define A_EDIT     ATTR(C_YELLOW, C_BLUE)
#define A_COMMENT  ATTR(C_LGREY, C_BLUE)
#define A_LABEL    ATTR(C_WHITE, C_BLUE)
#define A_DIRECT   ATTR(C_LCYAN, C_BLUE)
#define A_FRAME    ATTR(C_WHITE, C_BLUE)
#define A_TITLE    ATTR(C_WHITE, C_BLUE)
#define A_SCROLL   ATTR(C_CYAN, C_BLUE)
#define A_GUTTER   ATTR(C_LCYAN, C_BLUE)
#define A_GUTTER_ERR ATTR(C_WHITE, C_RED)
#define A_ERRBAR   ATTR(C_WHITE, C_RED)
#define A_STATUS   ATTR(C_BLACK, C_LGREY)
#define A_STATUS_HOT ATTR(C_RED, C_LGREY)
#define A_DIALOG   ATTR(C_BLACK, C_LGREY)
#define A_DFRAME   ATTR(C_WHITE, C_LGREY)
#define A_BUTTON   ATTR(C_BLACK, C_GREEN)
#define A_INPUT    ATTR(C_WHITE, C_BLUE)
#define A_HELP     ATTR(C_BLACK, C_CYAN)
#define A_HFRAME   ATTR(C_WHITE, C_CYAN)
#define A_HELP_HOT ATTR(C_YELLOW, C_CYAN)

// --- the text ---------------------------------------------------------------

typedef struct {
    char *s;
    int   len, cap;
} line_t;

static line_t *L;
static int nl, lcap;
static int cy, cx;               // cursor: line, byte in the line
static int want_col = -1;        // the column up and down try to keep
static int top, left;            // the first line and column on the screen
static bool modified;
static char filename[128];
static char run_args[96];
static bool listing;
static bool quit;

static asm_result_t res;
static bool have_res;
static bool res_stale = true;

static char errbar[160];
static char notice[120];

static void line_reserve(line_t *l, int n)
{
    if (n + 1 <= l->cap) return;
    int c = l->cap ? l->cap : 16;
    while (c < n + 1) c *= 2;
    char *p = realloc(l->s, (size_t)c);
    if (!p) return;
    l->s = p;
    l->cap = c;
}

static void lines_reserve(int n)
{
    if (n <= lcap) return;
    int c = lcap ? lcap : 64;
    while (c < n) c *= 2;
    line_t *p = realloc(L, (size_t)c * sizeof *p);
    if (!p) return;
    memset(p + lcap, 0, (size_t)(c - lcap) * sizeof *p);
    L = p;
    lcap = c;
}

static void insert_line(int at, const char *s, int len)
{
    lines_reserve(nl + 1);
    memmove(&L[at + 1], &L[at], (size_t)(nl - at) * sizeof *L);
    nl++;
    memset(&L[at], 0, sizeof L[at]);
    line_reserve(&L[at], len);
    if (len) memcpy(L[at].s, s, (size_t)len);
    L[at].len = len;
}

static void delete_line(int at)
{
    free(L[at].s);
    memmove(&L[at], &L[at + 1], (size_t)(nl - at - 1) * sizeof *L);
    nl--;
    memset(&L[nl], 0, sizeof L[nl]);
}

static void clear_text(void)
{
    for (int i = 0; i < nl; i++) free(L[i].s);
    nl = 0;
    cy = cx = top = left = 0;
}

static void set_text(const char *t, long n)
{
    clear_text();
    long i = 0;
    while (i <= n) {
        long j = i;
        while (j < n && t[j] != '\n') j++;
        int len = (int)(j - i);
        if (len && t[j - 1] == '\r') len--;
        if (j == n && len == 0 && nl > 0) break;      // no empty line for the final newline
        insert_line(nl, t + i, len);
        i = j + 1;
    }
    if (!nl) insert_line(0, "", 0);
    modified = false;
    res_stale = true;
}

static char *get_text(long *size)
{
    long n = 0;
    for (int i = 0; i < nl; i++) n += L[i].len + 1;
    char *t = malloc((size_t)n + 1);
    if (!t) return 0;
    long k = 0;
    for (int i = 0; i < nl; i++) {
        memcpy(t + k, L[i].s, (size_t)L[i].len);
        k += L[i].len;
        t[k++] = '\n';
    }
    t[k] = 0;
    *size = k;
    return t;
}

static const char *text_line(void *ctx, int i, int *len)
{
    (void)ctx;
    *len = L[i].len;
    return L[i].len ? L[i].s : "";
}

static void changed(void)
{
    modified = true;
    res_stale = true;
}

// --- columns: tabs every eight, UTF-8 one column a character -----------------

static bool cont(unsigned char c) { return (c & 0xc0) == 0x80; }

static int col_of(const line_t *l, int byte)
{
    int col = 0;
    for (int i = 0; i < byte && i < l->len; i++) {
        unsigned char c = (unsigned char)l->s[i];
        if (cont(c)) continue;
        col = c == '\t' ? (col / 8 + 1) * 8 : col + 1;
    }
    return col;
}

static int byte_at_col(const line_t *l, int want)
{
    int col = 0, i = 0;
    while (i < l->len) {
        unsigned char c = (unsigned char)l->s[i];
        int next = c == '\t' ? (col / 8 + 1) * 8 : col + 1;
        if (next > want) break;
        col = next;
        i++;
        while (i < l->len && cont((unsigned char)l->s[i])) i++;
    }
    return i;
}

// --- the screen's parts ------------------------------------------------------

static int ed_x0(void) { return 1; }
static int ed_y0(void) { return 2; }
static int ed_w(void)  { return scr_w - 2; }
static int ed_h(void)  { return scr_h - 4; }
#define GUTTER 16
static int text_x0(void) { return ed_x0() + (listing ? GUTTER : 0); }
static int text_w(void)  { return ed_w() - (listing ? GUTTER : 0); }

typedef struct {
    const char *label;
    const char *key;
    int cmd;
} item_t;

enum {
    CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_EXIT, CMD_DELLINE,
    CMD_DUPLINE, CMD_GOTO, CMD_TOP, CMD_BOTTOM, CMD_FIND, CMD_FINDNEXT, CMD_RUN, CMD_ARGS,
    CMD_COMPILE, CMD_LISTING, CMD_INFO, CMD_HELP, CMD_REFERENCE, CMD_SYSCALLS, CMD_ABOUT,
};

static const item_t m_file[] = {
    { "~N~ew", "", CMD_NEW }, { "~O~pen...", "F3", CMD_OPEN }, { "~S~ave", "F2", CMD_SAVE },
    { "Save ~a~s...", "", CMD_SAVEAS }, { 0, 0, 0 }, { "E~x~it", "Alt-X", CMD_EXIT }, { 0, 0, -1 },
};
static const item_t m_edit[] = {
    { "~D~elete line", "Ctrl-Y", CMD_DELLINE }, { "D~u~plicate line", "Ctrl-D", CMD_DUPLINE },
    { 0, 0, 0 }, { "Go to ~t~op", "", CMD_TOP }, { "Go to ~b~ottom", "", CMD_BOTTOM },
    { 0, 0, -1 },
};
static const item_t m_search[] = {
    { "~F~ind...", "Ctrl-F", CMD_FIND }, { "~S~earch again", "Ctrl-L", CMD_FINDNEXT },
    { "~G~o to line...", "Ctrl-G", CMD_GOTO }, { 0, 0, -1 },
};
static const item_t m_run[] = {
    { "~R~un", "F9", CMD_RUN }, { "~A~rguments...", "", CMD_ARGS }, { 0, 0, -1 },
};
static const item_t m_compile[] = {
    { "~C~ompile", "Alt-F9", CMD_COMPILE }, { "~L~isting", "F4", CMD_LISTING },
    { "~I~nformation...", "", CMD_INFO }, { 0, 0, -1 },
};
static const item_t m_help[] = {
    { "~K~eys", "F1", CMD_HELP }, { "~I~nstructions", "", CMD_REFERENCE },
    { "~S~ystem calls", "", CMD_SYSCALLS }, { 0, 0, 0 }, { "~A~bout...", "", CMD_ABOUT },
    { 0, 0, -1 },
};

typedef struct {
    const char *title;
    const item_t *items;
} menu_t;

static const menu_t menus[] = {
    { "~F~ile", m_file }, { "~E~dit", m_edit }, { "~S~earch", m_search },
    { "~R~un", m_run }, { "~C~ompile", m_compile }, { "~H~elp", m_help },
};
#define NMENUS ((int)(sizeof menus / sizeof menus[0]))

static int menu_x(int i)
{
    int x = 2;
    for (int k = 0; k < i; k++) {
        const char *t = menus[k].title;
        int n = 0;
        for (; *t; t++) if (*t != '~') n++;
        x += n + 2;
    }
    return x;
}

static void draw_menubar(int open)
{
    tui_fill(0, 0, scr_w, 1, ' ', A_MENU);
    for (int i = 0; i < NMENUS; i++) {
        int x = menu_x(i);
        bool sel = i == open;
        if (sel) tui_put(x - 1, 0, ' ', A_MENU_SEL);
        int n = tui_hot(x, 0, menus[i].title, sel ? A_MENU_SEL : A_MENU, sel ? A_MENU_SEL_HOT : A_MENU_HOT);
        if (sel) tui_put(x + n, 0, ' ', A_MENU_SEL);
    }
    const char *name = "Blixt";
    tui_text(scr_w - 7, 0, name, A_MENU, 6);
}

static void draw_status(const char *s)
{
    tui_fill(0, scr_h - 1, scr_w, 1, ' ', A_STATUS);
    if (!s) s = " ~F1~ Help  ~F2~ Save  ~F3~ Open  ~F4~ Listing  ~F9~ Run  ~Alt-F9~ Compile  ~F10~ Menu  ~Alt-X~ Exit";
    // Drop whole entries from the right when the line is too narrow.
    char buf[200];
    snprintf(buf, sizeof buf, "%s", s);
    for (;;) {
        int n = 0;
        for (const char *p = buf; *p; p++) if (*p != '~') n++;
        if (n <= scr_w) break;
        char *cut = strrchr(buf, ' ');
        while (cut && cut > buf && cut[-1] == ' ') cut--;
        if (!cut || cut == buf) break;
        *cut = 0;
        char *last = strrchr(buf, ' ');
        if (!last) break;
        while (last > buf && last[-1] == ' ') last--;
        *last = 0;
    }
    tui_hot(0, scr_h - 1, buf, A_STATUS, A_STATUS_HOT);
}

// Colour a line as the assembler reads it: a label, the mnemonic or
// directive, operands, and a comment. Decoration only -- nothing is said by
// the colour that the text does not say.
static void line_attrs(const line_t *l, uint8_t *attr)
{
    int i = 0;
    for (int k = 0; k < l->len; k++) attr[k] = A_EDIT;
    // Comment first: from @, ; or // outside a string.
    char q = 0;
    int comment = l->len;
    for (int k = 0; k < l->len; k++) {
        char c = l->s[k];
        if (q) { if (c == '\\') k++; else if (c == q) q = 0; continue; }
        if (c == '"') { q = c; continue; }
        if (c == '\'') { if (k + 1 < l->len && l->s[k + 1] == '\\') k++; k++; if (k + 1 < l->len && l->s[k + 1] == '\'') k++; continue; }
        if (c == '@' || c == ';' || (c == '/' && k + 1 < l->len && l->s[k + 1] == '/')) { comment = k; break; }
    }
    for (int k = comment; k < l->len; k++) attr[k] = A_COMMENT;
    while (i < comment && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    int j = i;
    while (j < comment && (isalnum((unsigned char)l->s[j]) || l->s[j] == '_' || l->s[j] == '.' || l->s[j] == '$')) j++;
    if (j > i && j < comment && l->s[j] == ':') {
        for (int k = i; k <= j; k++) attr[k] = A_LABEL;
        i = j + 1;
        while (i < comment && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
        j = i;
        while (j < comment && (isalnum((unsigned char)l->s[j]) || l->s[j] == '_' || l->s[j] == '.')) j++;
    }
    if (j > i && l->s[i] == '.')
        for (int k = i; k < j; k++) attr[k] = A_DIRECT;
}

static bool line_has_error(int line, const char **msg)
{
    if (!have_res) return false;
    for (int i = 0; i < res.nerrors && i < ASM_MAX_ERRORS; i++)
        if (res.errors[i].line == line) { if (msg) *msg = res.errors[i].msg; return true; }
    return false;
}

static void draw_gutter(int y, int line)
{
    tui_fill(ed_x0(), y, GUTTER, 1, ' ', A_GUTTER);
    if (!have_res || line >= res.nlines) return;
    if (line_has_error(line, 0)) {
        tui_text(ed_x0(), y, " Error here    ", A_GUTTER_ERR, GUTTER - 1);
        return;
    }
    uint32_t a = res.line_addr[line], n = res.line_size[line];
    char b[40];
    if (!n) {
        // A label shows where it is; an empty line shows nothing.
        const line_t *l = &L[line];
        int i = 0;
        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
        int j = i;
        while (j < l->len && (isalnum((unsigned char)l->s[j]) || l->s[j] == '_' || l->s[j] == '.' || l->s[j] == '$')) j++;
        if (j == i || j >= l->len || l->s[j] != ':') return;
        snprintf(b, sizeof b, "%04X", a);
    } else if (n == 2 && a + 2 <= res.size) {
        snprintf(b, sizeof b, "%04X %02X%02X", a, res.code[a + 1], res.code[a]);
    } else if (n == 4 && a + 4 <= res.size && !(L[line].len && strstr(L[line].s, ".word"))) {
        snprintf(b, sizeof b, "%04X %02X%02X %02X%02X", a, res.code[a + 1], res.code[a],
                 res.code[a + 3], res.code[a + 2]);
    } else {
        // Data, one byte after another, as many as fit.
        int k = snprintf(b, sizeof b, "%04X ", a);
        for (uint32_t i = 0; i < n && i < 4 && a + i < res.size; i++)
            k += snprintf(b + k, sizeof b - (size_t)k, "%02X", res.code[a + i]);
        if (n > 4) snprintf(b + k, sizeof b - (size_t)k, "+");
    }
    tui_text(ed_x0(), y, b, A_GUTTER, GUTTER - 1);
}

static void draw_editor(void)
{
    int x0 = ed_x0(), y0 = ed_y0(), w = ed_w(), h = ed_h();
    tui_box(0, 1, scr_w, scr_h - 2, true, A_FRAME);
    tui_fill(x0, y0, w, h, ' ', A_EDIT);
    // The title, and the close box and window number the IDE always had.
    const char *base = filename[0] ? filename : "NONAME.S";
    const char *slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    char title[80];
    snprintf(title, sizeof title, " %s ", base);
    int tl = (int)strlen(title);
    tui_text((scr_w - tl) / 2, 1, title, A_TITLE, scr_w - 10);
    tui_text(2, 1, "[", A_FRAME, 1);
    tui_put(3, 1, G_SQ, ATTR(C_LGREEN, C_BLUE));
    tui_text(4, 1, "]", A_FRAME, 1);
    tui_text(scr_w - 5, 1, "1", A_FRAME, 1);

    int tx = text_x0(), tw = text_w();
    uint8_t attr[1024];
    for (int r = 0; r < h; r++) {
        int li = top + r;
        if (listing) draw_gutter(y0 + r, li);
        if (li >= nl) continue;
        const line_t *l = &L[li];
        if (l->len <= (int)sizeof attr) line_attrs(l, attr);
        else memset(attr, A_EDIT, sizeof attr);
        int col = 0, i = 0;
        while (i < l->len) {
            uint8_t a = i < (int)sizeof attr ? attr[i] : A_EDIT;
            unsigned char c = (unsigned char)l->s[i];
            if (c == '\t') {
                int next = (col / 8 + 1) * 8;
                for (; col < next; col++)
                    if (col >= left && col - left < tw) tui_put(tx + col - left, y0 + r, ' ', a);
                i++;
                continue;
            }
            uint32_t cp = c;
            int more = 0;
            if (c >= 0xf0) { cp &= 7; more = 3; } else if (c >= 0xe0) { cp &= 15; more = 2; }
            else if (c >= 0xc0) { cp &= 31; more = 1; }
            i++;
            while (more-- && i < l->len && cont((unsigned char)l->s[i])) cp = (cp << 6) | ((unsigned char)l->s[i++] & 0x3f);
            if (cp < 32) cp = '?';
            if (col >= left && col - left < tw) tui_put(tx + col - left, y0 + r, cp, a);
            col++;
            if (col - left >= tw) break;
        }
    }

    // The scroll bar on the right-hand frame.
    int sx = scr_w - 1, sy = y0, sh = h;
    if (sh >= 3) {
        tui_put(sx, sy, G_UP, A_SCROLL);
        tui_put(sx, sy + sh - 1, G_DOWN, A_SCROLL);
        for (int k = 1; k < sh - 1; k++) tui_put(sx, sy + k, G_MED, A_SCROLL);
        int span = sh - 2;
        int pos = nl > 1 ? (cy * (span - 1)) / (nl - 1) : 0;
        tui_put(sx, sy + 1 + pos, G_SQ, A_SCROLL);
    }

    // Line and column, and the star that says there are unsaved changes.
    char where[32];
    snprintf(where, sizeof where, " %d:%d ", cy + 1, col_of(&L[cy], cx) + 1);
    tui_text(6, scr_h - 2, where, A_FRAME, 20);
    if (modified) tui_text(3, scr_h - 2, " * ", A_FRAME, 3);

    if (errbar[0]) {
        tui_fill(x0, y0, w, 1, ' ', A_ERRBAR);
        tui_text(x0 + 1, y0, errbar, A_ERRBAR, w - 2);
    }
}

// Keep the cursor on the screen.
static void scroll_to_cursor(void)
{
    int h = ed_h();
    if (errbar[0]) h--;
    if (cy < top) top = cy;
    if (cy >= top + h) top = cy - h + 1;
    if (top < 0) top = 0;
    int col = col_of(&L[cy], cx), tw = text_w();
    if (col < left) left = col;
    if (col >= left + tw) left = col - tw + 1;
}

static void reassemble(void)
{
    if (!res_stale) return;
    if (have_res) asm_free(&res);
    asm_assemble(text_line, 0, nl, &res);
    have_res = true;
    res_stale = false;
}

static void redraw_all(int open_menu, const char *status)
{
    if (listing) reassemble();
    draw_menubar(open_menu);
    draw_editor();
    draw_status(status);
}

static void place_cursor(void)
{
    int col = col_of(&L[cy], cx) - left;
    int y = ed_y0() + cy - top + (errbar[0] && cy == top ? 0 : 0);
    tui_cursor(text_x0() + col, y, true);
}

// --- dialogs ------------------------------------------------------------------

static void dialog_frame(int x, int y, int w, int h, const char *title, uint8_t body, uint8_t frame)
{
    tui_fill(x, y, w, h, ' ', body);
    tui_box(x, y, w, h, true, frame);
    if (title) {
        char t[80];
        snprintf(t, sizeof t, " %s ", title);
        int n = (int)strlen(t);
        tui_text(x + (w - n) / 2, y, t, frame, w - 4);
    }
    tui_shadow(x, y, w, h);
}

static void draw_button(int x, int y, const char *label, bool focus)
{
    // "  OK  ", and with the focus the arrows "►OK◄" as well as the colour.
    int n = (int)strlen(label) + 4;
    tui_fill(x, y, n, 1, ' ', A_BUTTON);
    tui_text(x + 2, y, label, A_BUTTON, n);
    if (focus) {
        tui_put(x, y, G_RIGHT, A_BUTTON);
        tui_put(x + n - 1, y, G_LEFT, A_BUTTON);
    }
    // The button's shadow, in half blocks.
    tui_put(x + n, y, 0x2584, ATTR(C_BLACK, C_LGREY));
    for (int i = 1; i <= n; i++) tui_put(x + i, y + 1, 0x2580, ATTR(C_BLACK, C_LGREY));
}

// A message and up to three buttons; returns the one chosen, -1 for Esc.
// A button's first letter chooses it too.
static int message(const char *title, const char *text, const char *const *buttons, int nb)
{
    int lines = 1, longest = 0, cur = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { lines++; if (cur > longest) longest = cur; cur = 0; }
        else if (((unsigned char)*p & 0xc0) != 0x80) cur++;
    }
    if (cur > longest) longest = cur;
    int bw = 0;
    for (int i = 0; i < nb; i++) bw += (int)strlen(buttons[i]) + 6;
    int w = longest + 6;
    if (w < bw + 4) w = bw + 4;
    if (w < 30) w = 30;
    if (w > scr_w - 4) w = scr_w - 4;
    int h = lines + 6;
    int x = (scr_w - w) / 2, y = (scr_h - h) / 2;
    int focus = 0;
    for (;;) {
        redraw_all(-1, " ~Enter~ Choose  ~Tab~ Next  ~Esc~ Cancel");
        dialog_frame(x, y, w, h, title, A_DIALOG, A_DFRAME);
        int ly = y + 2;
        const char *p = text;
        while (*p) {
            const char *e = strchr(p, '\n');
            char row[200];
            int n = e ? (int)(e - p) : (int)strlen(p);
            if (n >= (int)sizeof row) n = (int)sizeof row - 1;
            memcpy(row, p, (size_t)n);
            row[n] = 0;
            tui_text(x + 3, ly++, row, A_DIALOG, w - 6);
            if (!e) break;
            p = e + 1;
        }
        int bx = x + (w - bw) / 2;
        for (int i = 0; i < nb; i++) {
            draw_button(bx, y + h - 3, buttons[i], i == focus);
            bx += (int)strlen(buttons[i]) + 6;
        }
        tui_cursor(0, 0, false);
        tui_flush(false);
        int k = tui_key();
        if (k == K_RESIZE) { tui_flush(true); x = (scr_w - w) / 2; y = (scr_h - h) / 2; continue; }
        if (k == K_ESC) return -1;
        if (k == '\r' || k == '\n' || k == ' ') return focus;
        if (k == '\t' || k == K_RIGHT) focus = (focus + 1) % nb;
        if (k == K_LEFT) focus = (focus + nb - 1) % nb;
        for (int i = 0; i < nb; i++)
            if (k < 128 && tolower(k) == tolower((unsigned char)buttons[i][0])) return i;
    }
}

static void say(const char *title, const char *text)
{
    static const char *const ok[] = { "OK" };
    message(title, text, ok, 1);
}

// One line of input in a box; false when it was cancelled.
static bool ask(const char *title, const char *prompt, char *buf, int size)
{
    int w = 56;
    if (w > scr_w - 4) w = scr_w - 4;
    int h = 8, x = (scr_w - w) / 2, y = (scr_h - h) / 2;
    int len = (int)strlen(buf), pos = len;
    bool first = true;               // typing replaces the old text, as in the IDE
    for (;;) {
        redraw_all(-1, " ~Enter~ OK  ~Esc~ Cancel");
        dialog_frame(x, y, w, h, title, A_DIALOG, A_DFRAME);
        tui_hot(x + 3, y + 2, prompt, A_DIALOG, ATTR(C_YELLOW, C_LGREY));
        int fw = w - 6;
        tui_fill(x + 3, y + 3, fw, 1, ' ', A_INPUT);
        int start = pos > fw - 1 ? pos - (fw - 1) : 0;
        tui_text(x + 3, y + 3, buf + start, A_INPUT, fw);
        draw_button(x + w / 2 - 5, y + 5, "OK", true);
        tui_cursor(x + 3 + pos - start, y + 3, true);
        tui_flush(false);
        int k = tui_key();
        if (k == K_RESIZE) { tui_flush(true); x = (scr_w - w) / 2; y = (scr_h - h) / 2; continue; }
        if (k == K_ESC) return false;
        if (k == '\r' || k == '\n') return true;
        if (k == K_LEFT) { if (pos) pos--; first = false; continue; }
        if (k == K_RIGHT) { if (pos < len) pos++; first = false; continue; }
        if (k == K_HOME) { pos = 0; first = false; continue; }
        if (k == K_END) { pos = len; first = false; continue; }
        if (k == 8 || k == 127) {
            if (first) { len = pos = 0; buf[0] = 0; first = false; continue; }
            if (pos) { memmove(buf + pos - 1, buf + pos, (size_t)(len - pos + 1)); pos--; len--; }
            continue;
        }
        if (k == K_DEL) { if (pos < len) { memmove(buf + pos, buf + pos + 1, (size_t)(len - pos)); len--; } first = false; continue; }
        if (k >= 32 && k < 127) {
            if (first) { len = pos = 0; buf[0] = 0; first = false; }
            if (len + 1 < size) {
                memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));
                buf[pos++] = (char)k;
                len++;
            }
        }
    }
}

// A page of text to read, with the keys to move through it.
static void show_text(const char *title, const char *const *text, int n)
{
    int w = scr_w - 8, h = scr_h - 4;
    if (w > 84) w = 84;
    int x = (scr_w - w) / 2, y = 2, first = 0;
    for (;;) {
        h = scr_h - 4;
        redraw_all(-1, " ~Up~/~Down~ Scroll  ~Esc~ Close");
        dialog_frame(x, y, w, h, title, A_HELP, A_HFRAME);
        int rows = h - 2;
        if (first > n - rows) first = n - rows;
        if (first < 0) first = 0;
        for (int r = 0; r < rows && first + r < n; r++)
            tui_hot(x + 2, y + 1 + r, text[first + r], A_HELP, A_HELP_HOT);
        tui_cursor(0, 0, false);
        tui_flush(false);
        int k = tui_key();
        if (k == K_RESIZE) { tui_flush(true); x = (scr_w - w) / 2; continue; }
        if (k == K_UP) first--;
        else if (k == K_DOWN) first++;
        else if (k == K_PGUP) first -= rows;
        else if (k == K_PGDN || k == ' ') first += rows;
        else if (k == K_HOME) first = 0;
        else if (k == K_END) first = n;
        else return;
    }
}

static const char *const help_keys[] = {
    "~Blixt~ -- a Thumb-2 assembler for UbiqOS, in the Borland manner.",
    "",
    "~F1~        This help            ~F2~       Save",
    "~F3~        Open a file          ~F4~       Listing beside the source",
    "~F9~        Assemble and run     ~Alt-F9~   Assemble only",
    "~F10~       The menu bar         ~Alt-X~    Leave Blixt",
    "~Alt-F~ ~E~ ~S~ ~R~ ~C~ ~H~   Open that menu",
    "",
    "~Arrows~, ~Home~, ~End~, ~PgUp~, ~PgDn~   Move",
    "~Ctrl-Y~    Delete the line      ~Ctrl-D~   Duplicate the line",
    "~Ctrl-F~    Find                 ~Ctrl-L~   Find again",
    "~Ctrl-G~    Go to a line number  ~Tab~      A tab, shown every 8 columns",
    "~Enter~     A new line, indented like the one before",
    "",
    "Without F-keys or Alt: ~Ctrl-B~ assembles and runs, ~Ctrl-O~ opens,",
    "~Ctrl-S~ saves, ~Ctrl-Q~ leaves. ~Esc~ opens the menu bar.",
    "",
    "F9 writes the program as a module to ~/tmp~ and runs it with the",
    "terminal its own. When it ends, a key brings you back here.",
    "A program starts at ~main:~ (or ~_start:~, or its first line) with",
    "r0 = argc and r1 = argv, and ends by returning: ~bx lr~ or ~pop {pc}~.",
    "",
    "The listing (F4) is assembled as you type. A line the assembler",
    "cannot read says ~Error here~ in the margin; Alt-F9 tells you why.",
};

static const char *const help_ref[] = {
    "UAL syntax, as GNU as reads it. 16-bit forms are chosen when they fit;",
    "add ~.w~ for 32 bits. Comments start with ~@~, ~;~ or ~//~.",
    "",
    "~Moves~      mov movs mvn movw movt          ~Shifts~  lsl lsr asr ror rrx",
    "~Arithmetic~ add adds adc sub sbc rsb neg addw subw",
    "~Logic~      and orr eor bic orn   ~Compare~ cmp cmn tst teq",
    "~Multiply~   mul muls mla mls umull smull umlal smlal sdiv udiv",
    "~Bits~       bfi bfc ubfx sbfx clz rbit rev rev16 revsh uxtb uxth sxtb sxth",
    "~Load/store~ ldr ldrb ldrh ldrsb ldrsh ldrd str strb strh strd ldrex strex",
    "             [rn]  [rn, #imm]  [rn, #imm]!  [rn], #imm  [rn, rm, lsl #n]",
    "             ldr rX, =value     ldr rX, label     adr rX, label",
    "~Many~       push pop ldm ldmdb stm stmdb   {r0-r3, lr}",
    "~Branch~     b bl bx blx cbz cbnz tbb tbh   b~eq~ b~ne~ b~lt~ ... (all conditions)",
    "~IT blocks~  it itt ite itete... eq   then movEQ, addNE, ...",
    "~System~     svc bkpt udf nop wfi wfe sev yield dsb dmb isb cpsie cpsid mrs msr",
    "",
    "~Directives~ .word .hword .byte .ascii .asciz .space .align .balign",
    "             .equ NAME, value   NAME = value   .ltorg   .thumb_func",
    "             .stack BYTES  (the program's stack; 4096 if not given)",
    "~Labels~     name:   and local 1: 2: ... used as 1b (back) and 1f (forward)",
    "~Numbers~    10  0x1f  0b101  'A'  and + - * / % << >> & | ^ ~ ( )",
};

static const char *const help_sys[] = {
    "A system call: number in ~r7~, arguments in ~r0~ to ~r2~, then ~svc 0~.",
    "The answer comes back in ~r0~. r7 is the caller's, so push it first.",
    "",
    "~2~   exit                          ends the program",
    "~4~   write   r0 path, r1 buf, r2 length      -> bytes written",
    "~8~   read    r0 path, r1 buf, r2 length      -> bytes read",
    "~16~  sleep   r0 milliseconds",
    "~18~  ticks                         -> milliseconds since start",
    "~21~  alloc   r0 bytes               -> pointer, 0 if none",
    "~22~  free    r0 pointer",
    "~39~  readable r0 path              -> bytes waiting",
    "~56~  random  r0 buf, r1 length, r2 0",
    "",
    "Paths: ~0~ standard input, ~1~ standard output, ~2~ standard error.",
    "",
    "        movs r0, #1          @ standard output",
    "        adr  r1, msg",
    "        movs r2, #len",
    "        movs r7, #4          @ write",
    "        svc  0",
};

static const char *const template_text =
    "@ A first program for Blixt. F9 assembles and runs it.\n"
    "@ A system call: number in r7, arguments in r0-r2, then svc 0.\n"
    "\n"
    "        .equ    SYS_WRITE, 4\n"
    "        .equ    STDOUT, 1\n"
    "\n"
    "main:   push    {r7, lr}\n"
    "        movs    r0, #STDOUT\n"
    "        adr     r1, msg\n"
    "        movs    r2, #len\n"
    "        movs    r7, #SYS_WRITE\n"
    "        svc     0\n"
    "        pop     {r7, pc}\n"
    "\n"
    "        .align  2\n"
    "msg:    .ascii  \"Hello from Blixt!\\n\"\n"
    "        len = . - msg\n";

// --- files ----------------------------------------------------------------------

static bool save_to(const char *path)
{
    long n;
    char *t = get_text(&n);
    if (!t) { say("Save", "There is no memory to save in."); return false; }
    bool ok = plat_save(path, t, n);
    free(t);
    if (!ok) {
        char m[200];
        snprintf(m, sizeof m, "Could not write %s.", path);
        say("Save", m);
        return false;
    }
    if (path != filename) snprintf(filename, sizeof filename, "%s", path);
    modified = false;
    snprintf(notice, sizeof notice, "Saved %s", path);
    return true;
}

static bool save_as(void)
{
    char b[128];
    snprintf(b, sizeof b, "%s", filename[0] ? filename : "hello.s");
    if (!ask("Save File As", "~N~ame", b, (int)sizeof b) || !b[0]) return false;
    return save_to(b);
}

static bool save(void)
{
    if (!filename[0]) return save_as();
    return save_to(filename);
}

// Unsaved work is asked about before it is thrown away.
static bool may_discard(void)
{
    if (!modified) return true;
    static const char *const yn[] = { "Yes", "No", "Cancel" };
    char m[200];
    snprintf(m, sizeof m, "%s has been changed. Save it?", filename[0] ? filename : "NONAME.S");
    int r = message("Information", m, yn, 3);
    if (r == 0) return save();
    return r == 1;
}

static bool load(const char *path, bool quiet)
{
    long n;
    char *t = plat_load(path, &n);
    if (!t) {
        if (!quiet) {
            char m[200];
            snprintf(m, sizeof m, "Could not open %s.", path);
            say("Open", m);
        }
        return false;
    }
    set_text(t, n);
    free(t);
    snprintf(filename, sizeof filename, "%s", path);
    errbar[0] = 0;
    return true;
}

static void open_file(void)
{
    if (!may_discard()) return;
    char b[128];
    snprintf(b, sizeof b, "%s", filename);
    if (!ask("Open a File", "~N~ame", b, (int)sizeof b) || !b[0]) return;
    load(b, false);
}

static void new_file(void)
{
    if (!may_discard()) return;
    set_text(template_text, (long)strlen(template_text));
    filename[0] = 0;
}

// --- assembling and running ---------------------------------------------------

// The first error, put where the IDE put it: a red bar on top of the window,
// with the cursor on the line.
static bool compile(void)
{
    res_stale = true;
    reassemble();
    errbar[0] = 0;
    if (res.nerrors) {
        const asm_error_t *e = &res.errors[0];
        snprintf(errbar, sizeof errbar, "Error %d: %s.%s", e->line + 1, e->msg,
                 res.nerrors > 1 ? "  (and more: F4 shows where)" : "");
        cy = e->line < nl ? e->line : nl - 1;
        cx = 0;
        while (cx < L[cy].len && (L[cy].s[cx] == ' ' || L[cy].s[cx] == '\t')) cx++;
        return false;
    }
    return true;
}

static void module_name(char *out, int size)
{
    const char *b = filename[0] ? filename : "noname";
    const char *s = strrchr(b, '/');
    if (s) b = s + 1;
    int n = 0;
    for (; *b && *b != '.' && n < size - 1 && n < 15; b++) {
        char c = (char)tolower((unsigned char)*b);
        if (isalnum((unsigned char)c) || c == '_') out[n++] = c;
    }
    if (!n) { strcpy(out, "noname"); return; }
    out[n] = 0;
}

static void info(void)
{
    if (!compile()) return;
    char m[300], name[16];
    module_name(name, sizeof name);
    snprintf(m, sizeof m,
             "Lines: %d\nCode: %u bytes\nAddresses to fix: %u\nEntry: %04X\nStack: %u bytes\nPasses: %d\nModule: %s",
             nl, res.size, res.nrelocs, res.entry, res.stack ? res.stack : 4096u, res.passes, name);
    say("Information", m);
}

static void compile_cmd(void)
{
    if (!compile()) return;
    char m[200];
    snprintf(m, sizeof m, "Assembled %d lines.\n%u bytes of code, %d passes.\n\nSuccess: press any key.",
             nl, res.size, res.passes);
    say("Compiling", m);
}

static void run(void)
{
    if (!compile()) return;
    char name[16], msg[160];
    module_name(name, sizeof name);
    tui_done();
    plat_run(name, run_args, &res, msg, (int)sizeof msg);
    snprintf(notice, sizeof notice, "%s", msg);
    tui_flush(true);
}

// --- searching --------------------------------------------------------------------

static char find_what[64];

static bool find_next(bool from_here)
{
    if (!find_what[0]) return false;
    int n = (int)strlen(find_what);
    int line = cy, start = from_here ? cx : cx + 1;
    for (int k = 0; k <= nl; k++) {
        const line_t *l = &L[line];
        for (int i = start; i + n <= l->len; i++) {
            int j = 0;
            while (j < n && tolower((unsigned char)l->s[i + j]) == tolower((unsigned char)find_what[j])) j++;
            if (j == n) { cy = line; cx = i; return true; }
        }
        line = (line + 1) % nl;
        start = 0;
    }
    char m[120];
    snprintf(m, sizeof m, "Search string not found: %s", find_what);
    say("Search", m);
    return false;
}

static void go_to_line(void)
{
    char b[16] = "";
    if (!ask("Go to Line Number", "~E~nter new line number", b, (int)sizeof b)) return;
    int n = atoi(b);
    if (n < 1) n = 1;
    if (n > nl) n = nl;
    cy = n - 1;
    cx = 0;
}

// --- the menu bar --------------------------------------------------------------------

static int item_count(const item_t *it)
{
    int n = 0;
    while (it[n].cmd != -1) n++;
    return n;
}

static int run_menu(int m)
{
    int sel = 0;
    for (;;) {
        const item_t *it = menus[m].items;
        int n = item_count(it);
        if (!it[sel].label) sel++;
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (!it[i].label) continue;
            int lw = 0;
            for (const char *p = it[i].label; *p; p++) if (*p != '~') lw++;
            lw += (int)strlen(it[i].key) + 4;
            if (lw > w) w = lw;
        }
        w += 4;
        int x = menu_x(m) - 2, y = 1;
        if (x + w > scr_w) x = scr_w - w;
        redraw_all(m, " ~Enter~ Choose  ~Esc~ Close  ~Left~/~Right~ Other menus");
        tui_fill(x, y, w, n + 2, ' ', A_MENU);
        tui_box(x, y, w, n + 2, false, A_MENU);
        tui_shadow(x, y, w, n + 2);
        for (int i = 0; i < n; i++) {
            if (!it[i].label) {
                for (int k = 1; k < w - 1; k++) tui_put(x + k, y + 1 + i, G_H, A_MENU);
                tui_put(x, y + 1 + i, G_LT, A_MENU);
                tui_put(x + w - 1, y + 1 + i, G_RT, A_MENU);
                continue;
            }
            bool s = i == sel;
            uint8_t a = s ? A_MENU_SEL : A_MENU, h = s ? A_MENU_SEL_HOT : A_MENU_HOT;
            tui_fill(x + 1, y + 1 + i, w - 2, 1, ' ', a);
            // The chosen line has an arrow as well as its colour.
            if (s) tui_put(x + 1, y + 1 + i, G_RIGHT, a);
            tui_hot(x + 2, y + 1 + i, it[i].label, a, h);
            int kl = (int)strlen(it[i].key);
            tui_text(x + w - 2 - kl, y + 1 + i, it[i].key, a, kl);
        }
        tui_cursor(0, 0, false);
        tui_flush(false);
        int k = tui_key();
        if (k == K_RESIZE) { tui_flush(true); continue; }
        if (k == K_ESC || k == K_F10) return CMD_NONE;
        if (k == K_LEFT) { m = (m + NMENUS - 1) % NMENUS; sel = 0; continue; }
        if (k == K_RIGHT) { m = (m + 1) % NMENUS; sel = 0; continue; }
        if (k == K_UP) { do sel = (sel + n - 1) % n; while (!it[sel].label); continue; }
        if (k == K_DOWN) { do sel = (sel + 1) % n; while (!it[sel].label); continue; }
        if (k == '\r' || k == '\n') return it[sel].cmd;
        if ((k & K_ALT) && !(k & ~(K_ALT | 0x7f))) {
            int c = k & 0x7f;
            for (int i = 0; i < NMENUS; i++) {
                const char *t = strchr(menus[i].title, '~');
                if (t && tolower((unsigned char)t[1]) == c) { m = i; sel = 0; }
            }
            continue;
        }
        if (k < 128) {
            for (int i = 0; i < n; i++) {
                const char *t = it[i].label ? strchr(it[i].label, '~') : 0;
                if (t && tolower((unsigned char)t[1]) == tolower(k)) return it[i].cmd;
            }
        }
    }
}

// The menu bar itself, when F10 or Esc was pressed: move along it, Enter or
// Down opens one.
static int menu_bar(void)
{
    int m = 0;
    for (;;) {
        redraw_all(m, " ~Enter~ Open  ~Esc~ Back to the editor  ~Left~/~Right~ Choose");
        tui_cursor(0, 0, false);
        tui_flush(false);
        int k = tui_key();
        if (k == K_RESIZE) { tui_flush(true); continue; }
        if (k == K_ESC || k == K_F10) return CMD_NONE;
        if (k == K_LEFT) m = (m + NMENUS - 1) % NMENUS;
        else if (k == K_RIGHT) m = (m + 1) % NMENUS;
        else if (k == '\r' || k == '\n' || k == K_DOWN) return run_menu(m);
        else if (k < 128) {
            for (int i = 0; i < NMENUS; i++) {
                const char *t = strchr(menus[i].title, '~');
                if (t && tolower((unsigned char)t[1]) == tolower(k)) return run_menu(i);
            }
        }
    }
}

static void about(void)
{
    char m[300];
    snprintf(m, sizeof m,
             "Blixt %s\n\nA Thumb-2 assembler and editor\nfor UbiqOS, in the Borland manner.\n\nRunning on %s.",
             BLIXT_VERSION, plat_name());
    say("About", m);
}

static void command(int cmd)
{
    switch (cmd) {
    case CMD_NEW: new_file(); break;
    case CMD_OPEN: open_file(); break;
    case CMD_SAVE: save(); break;
    case CMD_SAVEAS: save_as(); break;
    case CMD_EXIT: if (may_discard()) quit = true; break;
    case CMD_DELLINE:
        if (nl > 1) { delete_line(cy); if (cy >= nl) cy = nl - 1; }
        else { L[0].len = 0; }
        cx = 0;
        changed();
        break;
    case CMD_DUPLINE:
        insert_line(cy + 1, L[cy].s ? L[cy].s : "", L[cy].len);
        cy++;
        changed();
        break;
    case CMD_TOP: cy = cx = 0; break;
    case CMD_BOTTOM: cy = nl - 1; cx = L[cy].len; break;
    case CMD_GOTO: go_to_line(); break;
    case CMD_FIND:
        if (ask("Find", "~T~ext to find", find_what, (int)sizeof find_what)) find_next(true);
        break;
    case CMD_FINDNEXT: if (find_what[0]) find_next(false); else command(CMD_FIND); break;
    case CMD_RUN: run(); break;
    case CMD_ARGS: ask("Program Arguments", "~P~arameters for the program", run_args, (int)sizeof run_args); break;
    case CMD_COMPILE: compile_cmd(); break;
    case CMD_LISTING: listing = !listing; res_stale = true; break;
    case CMD_INFO: info(); break;
    case CMD_HELP: show_text("Help", help_keys, (int)(sizeof help_keys / sizeof help_keys[0])); break;
    case CMD_REFERENCE: show_text("Instructions", help_ref, (int)(sizeof help_ref / sizeof help_ref[0])); break;
    case CMD_SYSCALLS: show_text("System Calls", help_sys, (int)(sizeof help_sys / sizeof help_sys[0])); break;
    case CMD_ABOUT: about(); break;
    }
}

// --- editing -----------------------------------------------------------------

static void insert_bytes(const char *b, int n)
{
    line_t *l = &L[cy];
    line_reserve(l, l->len + n);
    memmove(l->s + cx + n, l->s + cx, (size_t)(l->len - cx));
    memcpy(l->s + cx, b, (size_t)n);
    l->len += n;
    cx += n;
    changed();
}

static void insert_char(uint32_t c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); n = 3; }
    else return;
    insert_bytes(b, n);
}

static void newline(void)
{
    line_t *l = &L[cy];
    // The new line starts indented like this one.
    int ind = 0;
    while (ind < l->len && ind < cx && (l->s[ind] == ' ' || l->s[ind] == '\t')) ind++;
    char *rest = l->s + cx;
    int rest_len = l->len - cx;
    char buf[1024];
    int n = 0;
    if (ind + rest_len < (int)sizeof buf) {
        memcpy(buf, l->s, (size_t)ind);
        memcpy(buf + ind, rest, (size_t)rest_len);
        n = ind + rest_len;
    } else {
        ind = 0;
        n = rest_len < (int)sizeof buf ? rest_len : (int)sizeof buf - 1;
        memcpy(buf, rest, (size_t)n);
    }
    l->len = cx;
    insert_line(cy + 1, buf, n);
    cy++;
    cx = ind;
    changed();
}

static void backspace(void)
{
    if (cx > 0) {
        line_t *l = &L[cy];
        int from = cx - 1;
        while (from > 0 && cont((unsigned char)l->s[from])) from--;
        memmove(l->s + from, l->s + cx, (size_t)(l->len - cx));
        l->len -= cx - from;
        cx = from;
        changed();
    } else if (cy > 0) {
        line_t *p = &L[cy - 1];
        int at = p->len;
        line_reserve(p, p->len + L[cy].len);
        memcpy(p->s + p->len, L[cy].s, (size_t)L[cy].len);
        p->len += L[cy].len;
        delete_line(cy);
        cy--;
        cx = at;
        changed();
    }
}

static void delete_char(void)
{
    line_t *l = &L[cy];
    if (cx < l->len) {
        int to = cx + 1;
        while (to < l->len && cont((unsigned char)l->s[to])) to++;
        memmove(l->s + cx, l->s + to, (size_t)(l->len - to));
        l->len -= to - cx;
        changed();
    } else if (cy + 1 < nl) {
        line_reserve(l, l->len + L[cy + 1].len);
        memcpy(l->s + l->len, L[cy + 1].s, (size_t)L[cy + 1].len);
        l->len += L[cy + 1].len;
        delete_line(cy + 1);
        changed();
    }
}

static void move_vert(int d)
{
    if (want_col < 0) want_col = col_of(&L[cy], cx);
    cy += d;
    if (cy < 0) cy = 0;
    if (cy >= nl) cy = nl - 1;
    cx = byte_at_col(&L[cy], want_col);
}

static void key(int k)
{
    bool vertical = false;
    int page = ed_h() - 1;
    switch (k) {
    case K_F1: command(CMD_HELP); break;
    case K_F2: case 19: command(CMD_SAVE); break;              // Ctrl-S
    case K_F3: case 15: command(CMD_OPEN); break;              // Ctrl-O
    case K_F4: command(CMD_LISTING); break;
    case K_F9: case 2: command(CMD_RUN); break;                // Ctrl-B
    case K_ALTF | K_F9: command(CMD_COMPILE); break;
    case K_F10: case K_ESC: command(menu_bar()); break;
    case K_ALT | 'x': case 17: command(CMD_EXIT); break;       // Ctrl-Q
    case 25: command(CMD_DELLINE); break;                      // Ctrl-Y
    case 4: command(CMD_DUPLINE); break;                       // Ctrl-D
    case 6: command(CMD_FIND); break;                          // Ctrl-F
    case 12: command(CMD_FINDNEXT); break;                     // Ctrl-L
    case 7: command(CMD_GOTO); break;                          // Ctrl-G
    case K_UP: move_vert(-1); vertical = true; break;
    case K_DOWN: move_vert(1); vertical = true; break;
    case K_PGUP: move_vert(-page); top -= page; if (top < 0) top = 0; vertical = true; break;
    case K_PGDN: move_vert(page); top += page; if (top > nl - 1) top = nl - 1; vertical = true; break;
    case K_LEFT:
        if (cx > 0) { cx--; while (cx > 0 && cont((unsigned char)L[cy].s[cx])) cx--; }
        else if (cy > 0) { cy--; cx = L[cy].len; }
        break;
    case K_RIGHT:
        if (cx < L[cy].len) { cx++; while (cx < L[cy].len && cont((unsigned char)L[cy].s[cx])) cx++; }
        else if (cy + 1 < nl) { cy++; cx = 0; }
        break;
    case K_HOME: {
        // Home goes to the first character, then to the start of the line.
        int ind = 0;
        while (ind < L[cy].len && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) ind++;
        cx = (cx == ind) ? 0 : ind;
        break;
    }
    case K_END: cx = L[cy].len; break;
    case '\r': case '\n': newline(); break;
    case 8: case 127: backspace(); break;
    case K_DEL: delete_char(); break;
    case '\t': insert_char('\t'); break;
    case K_RESIZE: tui_flush(true); break;
    default:
        if (k & K_ALT) {
            int c = k & 0x7f;
            for (int i = 0; i < NMENUS; i++) {
                const char *t = strchr(menus[i].title, '~');
                if (t && tolower((unsigned char)t[1]) == c) { command(run_menu(i)); break; }
            }
        } else if (k >= 32 && k < K_UP && k != 127) {
            insert_char((uint32_t)k);
        }
        break;
    }
    if (!vertical) want_col = -1;
}

int blixt_main(int argc, char **argv)
{
    plat_init();
    int rows, cols;
    if (!tui_ask_size(&rows, &cols)) plat_default_size(&rows, &cols);
    tui_resize(rows, cols);
    lines_reserve(64);
    if (argc >= 2) {
        snprintf(filename, sizeof filename, "%s", argv[1]);
        // A name that is not there yet is a new file of that name.
        if (!load(argv[1], true)) {
            set_text(template_text, (long)strlen(template_text));
            snprintf(filename, sizeof filename, "%s", argv[1]);
        }
    } else {
        set_text(template_text, (long)strlen(template_text));
    }

    bool full = true;
    while (!quit) {
        scroll_to_cursor();
        redraw_all(-1, 0);
        if (notice[0]) {
            tui_fill(0, scr_h - 1, scr_w, 1, ' ', A_STATUS);
            tui_text(1, scr_h - 1, notice, A_STATUS, scr_w - 2);
        }
        place_cursor();
        tui_flush(full);
        full = false;
        int k = tui_key();
        notice[0] = 0;
        // The error bar stays until something is typed, as it did in the IDE.
        if (errbar[0] && k != K_RESIZE) errbar[0] = 0;
        key(k);
    }
    tui_done();
    plat_done();
    return 0;
}

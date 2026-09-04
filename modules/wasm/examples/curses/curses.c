// The implementation. Everything goes into one buffer and leaves on refresh.
//
// That buffering is not an optimisation, it is the difference between a usable
// editor and an unusable one. A redraw is a few thousand characters, and a
// write syscall for each of them is a few thousand traps -- on a console that
// draws each character into a framebuffer, the screen visibly crawls. One write
// per refresh is one trap per redraw.
#include "curses.h"
#include <unistd.h>

WINDOW *stdscr = (WINDOW *)1;   // a handle, never dereferenced
WINDOW *curscr = (WINDOW *)2;   // and the other one curses exposes
int LINES = 30;
int COLS  = 80;

// Big enough for a whole screen of text with an escape sequence on every line,
// so a redraw never has to flush halfway and tear.
static char  out[8192];
static unsigned out_len;

static void put(const char *s)
{
    while (*s) {
        if (out_len >= sizeof out) { (void)write(1, out, out_len); out_len = 0; }
        out[out_len++] = *s++;
    }
}

static void put_num(int v)
{
    char b[12];
    int n = 0;
    if (v < 0) v = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) {
        if (out_len >= sizeof out) { (void)write(1, out, out_len); out_len = 0; }
        out[out_len++] = b[--n];
    }
}

int refresh(void)
{
    if (out_len) { (void)write(1, out, out_len); out_len = 0; }
    return OK;
}

WINDOW *initscr(void) { put("\x1b[2J\x1b[H"); refresh(); return stdscr; }

// The cursor comes back, and the screen is left clean. An editor that exits
// leaving the cursor hidden and the last colour set is an editor that breaks
// the shell it returns to.
int endwin(void)
{
    put("\x1b[0m\x1b[2J\x1b[H");
    refresh();
    return OK;
}

// The myrtos console hands over each key as it is pressed and echoes nothing,
// which is what raw() and noecho() are asking for. There is nothing to do.
int raw(void)    { return OK; }
int noraw(void)  { return OK; }
int cbreak(void) { return OK; }
int noecho(void) { return OK; }
int echo(void)   { return OK; }
int nonl(void)   { return OK; }
int idlok(WINDOW *w, int f)   { (void)w; (void)f; return OK; }
int keypad(WINDOW *w, int f)  { (void)w; (void)f; return OK; }
int scrollok(WINDOW *w, int f){ (void)w; (void)f; return OK; }
int beep(void)   { put("\a"); return OK; }

int curs_set(int v) { put(v ? "\x1b[?25h" : "\x1b[?25l"); return OK; }

int clear(void)    { put("\x1b[2J\x1b[H"); return OK; }
int erase(void)    { return clear(); }
int clrtoeol(void) { put("\x1b[K"); return OK; }
int clrtobot(void) { put("\x1b[J"); return OK; }

int move(int y, int x)
{
    put("\x1b["); put_num(y + 1); put(";"); put_num(x + 1); put("H");
    return OK;
}

int addch(chtype c)  { char s[2]; s[0] = (char)(c & 0xff); s[1] = 0; put(s); return OK; }
int addstr(const char *s) { put(s); return OK; }
int mvaddstr(int y, int x, const char *s) { move(y, x); return addstr(s); }

// --- COLOUR ---------------------------------------------------------------
// Pairs are remembered rather than looked up: eight of them, set once at
// startup, and attron carries the number.
#define MAX_PAIRS 16
static short pair_fg[MAX_PAIRS], pair_bg[MAX_PAIRS];

int has_colors(void)  { return TRUE; }
int start_color(void) { return OK; }

int init_pair(short pair, short fg, short bg)
{
    if (pair < 0 || pair >= MAX_PAIRS) return ERR;
    pair_fg[pair] = fg;
    pair_bg[pair] = bg;
    return OK;
}

int attron(chtype a)
{
    if (a & A_REVERSE) { put("\x1b[7m"); return OK; }
    if (a & A_BOLD)    { put("\x1b[1m"); return OK; }

    int p = (int)((a >> 8) & 0xff);
    if (p < 0 || p >= MAX_PAIRS) return ERR;
    put("\x1b[0;");
    put_num(30 + pair_fg[p]);
    put(";");
    put_num(40 + pair_bg[p]);
    put("m");
    return OK;
}

int attroff(chtype a) { (void)a; put("\x1b[0m"); return OK; }
int standout(void)    { put("\x1b[7m"); return OK; }
int standend(void)    { put("\x1b[0m"); return OK; }

// One key. The screen is flushed first: whatever the program drew before
// asking is what the person needs to see in order to answer.
int getch(void)
{
    unsigned char c;
    refresh();
    if (read(0, &c, 1) != 1) return ERR;
    return (int)c;
}

// A control character in the form a person reads: "^A" for one, "^?" for
// delete. Atto shows it in the modeline when asked what character is under the
// point. The buffer is static and one deep, as curses' own is -- a caller that
// wants two at once must copy the first.
const char *unctrl(chtype c)
{
    static char b[5];
    unsigned ch = (unsigned)c & 0xff;

    if (ch == 127) { b[0] = '^'; b[1] = '?'; b[2] = 0; }
    else if (ch < 32) { b[0] = '^'; b[1] = (char)('@' + ch); b[2] = 0; }
    else { b[0] = (char)ch; b[1] = 0; }
    return b;
}

// Nothing is buffered on this side, and the kernel's ring is drained by the
// reader, so there is nothing to throw away.
int flushinp(void) { return OK; }

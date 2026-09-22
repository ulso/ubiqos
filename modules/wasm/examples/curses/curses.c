// The implementation. Everything goes into one buffer and leaves on refresh.
//
// That buffering is not an optimisation, it is the difference between a usable
// editor and an unusable one. A redraw is a few thousand characters, and a
// write syscall for each of them is a few thousand traps -- on a console that
// draws each character into a framebuffer, the screen visibly crawls. One write
// per refresh is one trap per redraw.
#include "curses.h"
#include <unistd.h>
#include <stdlib.h>

WINDOW *stdscr = (WINDOW *)1;   // a handle, never dereferenced
WINDOW *curscr = (WINDOW *)2;   // and the other one curses exposes
int LINES = 30;
int COLS  = 80;

// Big enough for a whole screen of text with an escape sequence on every line,
// so a redraw never has to flush halfway and tear.
// One screen of text with an escape sequence on every line. A hundred and six
// columns by forty rows is over four thousand characters before the escapes.
static char  out[16384];
static unsigned out_len;

static void emit(unsigned char c)
{
    if (out_len >= sizeof out) { (void)write(1, out, out_len); out_len = 0; }
    out[out_len++] = (char)c;
}

// Bytes go through untouched. The console reads UTF-8 and the keyboard sends
// it, so a program that thinks in UTF-8 -- which any C program calling
// setlocale does -- needs nothing translated on its behalf.
static void put(const char *s)
{
    while (*s) emit((unsigned char)*s++);
}

static void put_num(int v)
{
    char b[12];
    int n = 0;
    if (v < 0) v = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) emit((unsigned char)b[--n]);
}

int refresh(void)
{
    if (out_len) { (void)write(1, out, out_len); out_len = 0; }
    return OK;
}

// How big the screen is. There is no ioctl here and no terminal to ask, so the
// size arrives in the environment as LINES and COLUMNS -- which is where curses
// looks for it everywhere else too when a terminal will not say. The defaults
// above stand if nothing sets them.
static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v) return fallback;
    int n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    return n > 0 ? n : fallback;
}

WINDOW *initscr(void)
{
    curses_adopt_pwd();

    // The terminal first, the environment second, the defaults last. Asking
    // matters most where the editor is furthest from the screen it started on:
    // over SSH the board's own console is thirty rows whatever the window at
    // the other end is, and an editor that believes thirty in a window of
    // fifty draws in the top half and leaves the rest behind.
    int rows = 0, cols = 0;
    if (curses_term_size(&rows, &cols) && rows >= 4 && cols >= 20) {
        LINES = rows;
        COLS  = cols;
    } else {
        LINES = env_int("LINES", LINES);
        COLS  = env_int("COLUMNS", COLS);
    }
    // The attribute is cleared before the screen is: whatever colour the
    // program before us left set would otherwise be what the screen is filled
    // with, and a terminal that starts an editor should look like itself.
    put("\x1b[0m\x1b[2J\x1b[H");
    refresh();
    return stdscr;
}

// The cursor comes back, and the screen is left clean. An editor that exits
// leaving the cursor hidden and the last colour set is an editor that breaks
// the shell it returns to.
int endwin(void)
{
    put("\x1b[0m\x1b[2J\x1b[H");
    refresh();
    return OK;
}

// The UbiqOS console hands over each key as it is pressed and echoes nothing,
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

    // The bright half of the palette, except for black, which has no bright
    // form worth having -- 90 is grey, and a modeline of grey on white is not
    // readable. curses' eight colours are the dim eight, and on a black screen
    // they are hard to read: the console's own default is 15, brightwhite, so
    // text in curses' COLOR_WHITE came out dimmer than the shell it replaced.
    // COLOR_BLACK as a background means THE BACKGROUND, not black paint.
    //
    // Programs write init_pair(n, COLOR_CYAN, COLOR_BLACK) meaning "cyan on
    // whatever is behind", because on the terminals curses grew up on black is
    // what was behind. Sending 40 paints it black, which is invisible on this
    // board's own screen -- black on black -- and a black rectangle inside a
    // window with any other background, which is what it looked like over SSH.
    // 49 is the default background, and it is what that pair means.
    //
    // A black FOREGROUND is only meaningful against something, so it stands
    // when the pair names a background and becomes the default when it does
    // not: black on black is nobody's intention.
    put("\x1b[0;");
    put_num(pair_fg[p] ? 90 + pair_fg[p] : (pair_bg[p] ? 30 : 39));
    put(";");
    put_num(pair_bg[p] ? 100 + pair_bg[p] : 49);
    put("m");
    return OK;
}

int attroff(chtype a) { (void)a; put("\x1b[0m"); return OK; }
int standout(void)    { put("\x1b[7m"); return OK; }
int standend(void)    { put("\x1b[0m"); return OK; }

// One key. The screen is flushed first: whatever the program drew before
// asking is what the person needs to see in order to answer.
// A terminal's answer about its size, whenever it turns up.
//
// The question is asked at startup and most terminals reply within a few
// milliseconds -- but over a network a late one arrives after the editor has
// given up waiting, and then it is read as if somebody had typed
// `[8;40;130t` into the file. Which is exactly what happened.
//
// So the answer is recognised here as well, and acted on: the size it carries
// is the size to use. ESC [ ... t and ESC [ ... R are swallowed whole; any
// other escape is handed on as before, because Atto reads those itself.
static int swallow_report(void)
{
    unsigned char c;
    unsigned p[3] = { 0, 0, 0 };
    int np = 0;

    if (read(0, &c, 1) != 1) return ERR;
    if (c != '[') return 0x1b;                     // not a CSI; give ESC back

    for (;;) {
        if (read(0, &c, 1) != 1) return ERR;
        if (c >= '0' && c <= '9') { if (np < 3) p[np] = p[np] * 10 + (c - '0'); continue; }
        if (c == ';') { if (np < 2) np++; continue; }
        if (c == 't') {
            if (p[0] == 8 && np >= 2 && p[1] >= 4 && p[2] >= 20) {
                LINES = (int)p[1];
                COLS  = (int)p[2];
            }
            return 0;                              // eaten, and nothing typed
        }
        if (c == 'R') return 0;                    // a cursor report; also ours
        return 0;                                  // some other sequence, gone
    }
}

int getch(void)
{
    unsigned char c;
    refresh();
    if (read(0, &c, 1) != 1) return ERR;

    if (c == 0x1b) {
        const int r = swallow_report();
        if (r == ERR) return ERR;
        if (r == 0) return getch();                // it was a report: read on
        return r;                                  // an ESC the editor wanted
    }

    // Carriage return becomes newline, which is what curses does on input
    // unless a program asks it not to with nonl(). Programs lean on it: Atto
    // inserts a line break on 10 and answers "Not bound" to 13, and 13 is what
    // a keyboard sends.
    if (c == '\r') return '\n';
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

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

// The guest thinks in UTF-8 and myrtos thinks in Latin-1, and this is the seam.
//
// It is not a preference on either side: the console's font draws Latin-1 and
// the keyboard descriptor sends it, while a C program that calls setlocale and
// counts bytes -- Atto does both -- reads a byte of 0xE5 as the start of a
// three-byte sequence and swallows the two characters after it. Translating
// here costs nothing and leaves both sides believing what they already believe.
//
// Only the two leads that can hold Latin-1 are decoded. Anything above U+00FF
// has no glyph on this console, so it becomes a question mark rather than a
// scatter of them.
static int           utf8_held;   // the second byte of an incoming character
static unsigned char utf8_lead;
static unsigned      utf8_left;

static void put(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (utf8_lead) {
            emit((unsigned char)(((utf8_lead & 0x1f) << 6) | (c & 0x3f)));
            utf8_lead = 0;
        } else if (utf8_left) {
            utf8_left--;
        } else if (c < 0x80) {
            emit(c);
        } else if (c == 0xc2 || c == 0xc3) {
            utf8_lead = c;
        } else if (c >= 0xc4 && c < 0xf8) {
            utf8_left = (c < 0xe0) ? 1 : (c < 0xf0) ? 2 : 3;
            emit('?');
        }
        // A stray continuation byte is dropped: there is nothing to add it to.
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
    LINES = env_int("LINES", LINES);
    COLS  = env_int("COLUMNS", COLS);
    put("\x1b[2J\x1b[H");
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

    // The bright half of the palette, except for black, which has no bright
    // form worth having -- 90 is grey, and a modeline of grey on white is not
    // readable. curses' eight colours are the dim eight, and on a black screen
    // they are hard to read: the console's own default is 15, brightwhite, so
    // text in curses' COLOR_WHITE came out dimmer than the shell it replaced.
    put("\x1b[0;");
    put_num(pair_fg[p] ? 90 + pair_fg[p] : 30);
    put(";");
    put_num(pair_bg[p] ? 100 + pair_bg[p] : 40);
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

    // The second byte of a character the keyboard sent as one. Handed over
    // before anything is read and without flushing: nothing has changed on the
    // screen since the first byte went out.
    if (utf8_held) { int held = utf8_held; utf8_held = 0; return held; }

    refresh();
    if (read(0, &c, 1) != 1) return ERR;

    // Carriage return becomes newline, which is what curses does on input
    // unless a program asks it not to with nonl(). Programs lean on it: Atto
    // inserts a line break on 10 and answers "Not bound" to 13, and 13 is what
    // a keyboard sends.
    if (c == '\r') return '\n';

    // Latin-1 in, UTF-8 out -- the other half of the seam in put(). Everything
    // from U+0080 to U+00FF is two bytes there, so the second is held back and
    // handed over on the next call.
    if (c >= 0x80) {
        utf8_held = 0x80 | (c & 0x3f);
        return 0xc0 | (c >> 6);
    }
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

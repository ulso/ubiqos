#ifndef UBIQOS_CURSES_H
#define UBIQOS_CURSES_H

// A curses small enough to carry: the twenty-one calls Atto uses, over ANSI.
//
// Atto is an emacs in about two thousand lines, and the only thing in it that
// needs an operating system is curses. WASI has no ncurses and never will --
// terminfo is a database of files describing terminals nobody here has. But a
// program like this does not want a terminal database. It wants to move the
// cursor, write some text, clear to the end of a line and read a key, and every
// one of those is an escape sequence the UbiqOS console already understands.
//
// So this is not a port of curses. It is the part of curses that Atto uses,
// written directly, and any other curses program that stays inside these calls
// gets to run for the same reason.
//
// What is deliberately missing: windows other than stdscr, terminfo, the key
// decoding that turns an escape sequence into KEY_UP. Atto needs none of them --
// it reads raw bytes and matches sequences in its own key table, which is what
// makes it such a good first program to bring across.

#include <stdint.h>

#define OK    0
#define ERR  (-1)
#define TRUE  1
#define FALSE 0

typedef int chtype;
typedef void WINDOW;

extern WINDOW *stdscr;
extern WINDOW *curscr;   // what is on the screen now, as curses names it
extern int LINES, COLS;

// What getch returns when the terminal says its size has changed: ncurses'
// own number. Atto binds resize-terminal to 0x9A, which is this cut to a byte,
// so the editor was ready for it all along -- nothing ever sent it.
#define KEY_RESIZE 0632

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

// A pair number, shifted clear of the character in a chtype the way curses
// does it, so attron(COLOR_PAIR(n)) carries the number and nothing else.
#define COLOR_PAIR(n) ((chtype)((n) << 8))
#define A_NORMAL      ((chtype)0)
#define A_REVERSE     ((chtype)0x10000)
#define A_BOLD        ((chtype)0x20000)

WINDOW *initscr(void);
void curses_adopt_pwd(void);   // compat.c: start in the directory the shell was in

// compat.c: ask the terminal how big it is, if this build can. 1 and the size
// when it answered, 0 when there is nobody to ask -- and then the environment
// and the defaults decide, as they did before.
int curses_term_size(int *rows, int *cols);
int endwin(void);
int raw(void);
int noraw(void);
int cbreak(void);
int noecho(void);
int echo(void);
int nonl(void);
int idlok(WINDOW *w, int flag);
int keypad(WINDOW *w, int flag);
int scrollok(WINDOW *w, int flag);
int curs_set(int visibility);
int has_colors(void);
int start_color(void);
int init_pair(short pair, short fg, short bg);
int clear(void);
int erase(void);
int clrtoeol(void);
int clrtobot(void);
int move(int y, int x);
int addch(chtype c);
int addstr(const char *s);
int mvaddstr(int y, int x, const char *s);
int attron(chtype a);
int attroff(chtype a);
int standout(void);
int standend(void);
int refresh(void);
int getch(void);
int flushinp(void);
int beep(void);
const char *unctrl(chtype c);

#endif

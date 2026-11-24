// A text screen in memory, sent to the terminal as the difference from what
// it already shows. Plain ANSI: cursor positioning, sixteen colours, UTF-8.
//
// Speed is the number of bytes, nothing else. A keystroke in the editor
// changes a handful of cells, and only those go out, in one write -- which is
// what makes it as quick over USB or SSH as on the board's own screen.
#ifndef BLIXT_TUI_H
#define BLIXT_TUI_H

#include <stdbool.h>
#include <stdint.h>

// The sixteen colours, in the order of the SGR codes.
enum {
    C_BLACK, C_RED, C_GREEN, C_BROWN, C_BLUE, C_MAGENTA, C_CYAN, C_LGREY,
    C_DGREY, C_LRED, C_LGREEN, C_YELLOW, C_LBLUE, C_LMAGENTA, C_LCYAN, C_WHITE,
};
#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))

// Frames, shades and arrows. The board's font has exactly these.
#define G_H     0x2500u
#define G_V     0x2502u
#define G_TL    0x250Cu
#define G_TR    0x2510u
#define G_BL    0x2514u
#define G_BR    0x2518u
#define G_LT    0x251Cu
#define G_RT    0x2524u
#define G_DH    0x2550u
#define G_DV    0x2551u
#define G_DTL   0x2554u
#define G_DTR   0x2557u
#define G_DBL   0x255Au
#define G_DBR   0x255Du
#define G_LIGHT 0x2591u
#define G_MED   0x2592u
#define G_FULL  0x2588u
#define G_SQ    0x25A0u
#define G_UP    0x25B2u
#define G_DOWN  0x25BCu
#define G_RIGHT 0x25BAu
#define G_LEFT  0x25C4u

extern int scr_w, scr_h;

void tui_init(void);
void tui_done(void);
void tui_resize(int rows, int cols);
void tui_fill(int x, int y, int w, int h, uint32_t ch, uint8_t attr);
void tui_put(int x, int y, uint32_t ch, uint8_t attr);
// UTF-8 text, clipped to the screen and to max columns; returns the columns
// used. A '~' toggles hot: "~F~ile" draws the F in hot_attr.
int  tui_text(int x, int y, const char *s, uint8_t attr, int max);
int  tui_hot(int x, int y, const char *s, uint8_t attr, uint8_t hot_attr);
void tui_box(int x, int y, int w, int h, bool dbl, uint8_t attr);
void tui_shadow(int x, int y, int w, int h);
void tui_cursor(int x, int y, bool show);
// Send what changed. full sends everything, for a screen of unknown state.
void tui_flush(bool full);

// Keys: a character is its code point; the rest are above any code point.
enum {
    K_UP = 0x200000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INS, K_DEL, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10,
    K_F11, K_F12, K_ESC, K_RESIZE, K_NONE,
};
#define K_ALT  0x400000       // | the letter, lower case
#define K_ALTF 0x800000       // | the function key, K_F1 to K_F12

// The next key, waiting for ever. A terminal's answer to ESC [ 18 t arrives
// as K_RESIZE with the new size already applied.
int  tui_key(void);
// Ask the terminal its size; false when nothing answers (the board's screen).
bool tui_ask_size(int *rows, int *cols);

#endif

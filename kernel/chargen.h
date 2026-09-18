// The character generator: what the display shows when there is no framebuffer.
//
// A cell is a glyph index and two four-bit colour indices, so the whole screen
// is 4800 bytes instead of 307200. Nothing is drawn in advance; video.c asks
// for a scanline at a time, a little ahead of the beam, and this builds it.
#ifndef UBIQOS_CHARGEN_H
#define UBIQOS_CHARGEN_H

#include <stdint.h>
#include <stdbool.h>
// Which display, and so which geometry. The cell arithmetic below is the same
// either way; only the width and the bytes per pixel differ.
#ifdef UBIQOS_VIDEO_RGB
#include "videorgb.h"
#else
#include "video.h"
#endif

#define UBIQOS_CELL_W    8
#define UBIQOS_CELL_H    16
#define UBIQOS_CELL_COLS (UBIQOS_H_ACTIVE / UBIQOS_CELL_W)   // 80
#define UBIQOS_CELL_ROWS (UBIQOS_V_ACTIVE / UBIQOS_CELL_H)   // 30

// Rows kept, of which UBIQOS_CELL_ROWS are on the screen and the rest are
// history. The cells were always a ring; this makes the ring longer than the
// window, which is the whole of the scrollback.
//
//   256 rows * 80 columns * 2 bytes = 40960, and 8.5 screens of history.
//
// It comes out of .bss, so it is spent against the C heap between end and the
// stack rather than against the module pool. The framebuffer this replaced was
// 307200 bytes, so eight screens of history still costs an eighth of it.
#define UBIQOS_CELL_RING 256

// ch is the glyph's index into the font, not the character: the generator runs
// under a deadline and should not be deciding what to do about control codes.
// attr is the foreground index in the high nibble and the background in the low.
typedef struct { uint8_t ch; uint8_t attr; } ubiqos_cell_t;

// The character a cell holds when nothing has been written to it.
#define UBIQOS_CELL_BLANK 0

uint8_t ubiqos_chargen_glyph(char c);

void ubiqos_chargen_init(uint8_t attr);
void ubiqos_chargen_put(uint32_t row, uint32_t col, uint8_t glyph, uint8_t attr);
void ubiqos_chargen_fill(uint32_t row, uint32_t from, uint32_t to, uint8_t attr);
void ubiqos_chargen_clear(uint8_t attr);
void ubiqos_chargen_scroll(uint8_t attr);
void ubiqos_chargen_cursor(uint32_t row, uint32_t col, bool on);

// The renderers. The 8-bit pair are for a display with one byte per pixel; the
// 16-bit one is for RGB565, and exists because a mask that covers four pixels
// in a word covers two in the other format -- the trick is the same and the
// arithmetic is not.
void ubiqos_chargen_band16(uint32_t y0, uint16_t *base);
void ubiqos_chargen_line16(uint32_t y, uint16_t *dst);

// The console's palette, so a drawn scene and the console agree about colours.
uint16_t ubiqos_chargen_colour(uint8_t index);

// One row of one glyph, for drawing text outside the character generator.
uint32_t ubiqos_chargen_glyph_row(char c, uint32_t row);

// Scrollback. The view is a window onto the ring, counted in rows back from the
// live screen; writing always goes to the live screen whatever the view shows.
void     ubiqos_chargen_view_move(int32_t rows);   // negative is back in time
void     ubiqos_chargen_view_end(void);            // return to live
void     ubiqos_chargen_view_home(void);           // as far back as there is
uint32_t ubiqos_chargen_view_back(void);
uint32_t ubiqos_chargen_history(void);
uint32_t ubiqos_chargen_deep(void);

// One scanline of UBIQOS_H_ACTIVE bytes. Called from an interrupt above the
// kernel's threshold, so it must touch nothing but its own memory.
void ubiqos_chargen_peek_row(uint32_t row, uint8_t *out, uint32_t n);
void ubiqos_chargen_band(uint32_t y0, uint8_t *base);
void ubiqos_chargen_line(uint32_t y, uint8_t *dst);

#endif

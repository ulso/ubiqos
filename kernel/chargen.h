// The character generator: what the display shows when there is no framebuffer.
//
// A cell is a glyph index and two four-bit colour indices, so the whole screen
// is 4800 bytes instead of 307200. Nothing is drawn in advance; video.c asks
// for a scanline at a time, a little ahead of the beam, and this builds it.
#ifndef MYRTOS_CHARGEN_H
#define MYRTOS_CHARGEN_H

#include <stdint.h>
#include <stdbool.h>
#include "video.h"

#define MYRTOS_CELL_W    8
#define MYRTOS_CELL_H    16
#define MYRTOS_CELL_COLS (MYRTOS_H_ACTIVE / MYRTOS_CELL_W)   // 80
#define MYRTOS_CELL_ROWS (MYRTOS_V_ACTIVE / MYRTOS_CELL_H)   // 30

// ch is the glyph's index into the font, not the character: the generator runs
// under a deadline and should not be deciding what to do about control codes.
// attr is the foreground index in the high nibble and the background in the low.
typedef struct { uint8_t ch; uint8_t attr; } myrtos_cell_t;

// The character a cell holds when nothing has been written to it.
#define MYRTOS_CELL_BLANK 0

uint8_t myrtos_chargen_glyph(char c);

void myrtos_chargen_init(uint8_t attr);
void myrtos_chargen_put(uint32_t row, uint32_t col, uint8_t glyph, uint8_t attr);
void myrtos_chargen_fill(uint32_t row, uint32_t from, uint32_t to, uint8_t attr);
void myrtos_chargen_clear(uint8_t attr);
void myrtos_chargen_scroll(uint8_t attr);
void myrtos_chargen_cursor(uint32_t row, uint32_t col, bool on);

// One scanline of MYRTOS_H_ACTIVE bytes. Called from an interrupt above the
// kernel's threshold, so it must touch nothing but its own memory.
void myrtos_chargen_line(uint32_t y, uint8_t *dst);

#endif

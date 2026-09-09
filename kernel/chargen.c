// The character generator. See chargen.h for the shape and video.c for who
// calls myrtos_chargen_line and when.
#include "chargen.h"

extern const uint8_t myrtos_font8x16[224][MYRTOS_CELL_H];
extern const uint8_t myrtos_ansi_colour[16];

static myrtos_cell_t cells[MYRTOS_CELL_ROWS * MYRTOS_CELL_COLS];

// Which cell row is shown at the top. Scrolling moves this by one and clears
// the row that comes round, which is the whole job -- the same trick the
// framebuffer played with its line table, in units of rows rather than
// scanlines, and 4800 bytes instead of 307200.
static uint32_t row_origin;

static uint32_t cur_row, cur_col;
static bool     cur_on;

// Four pixels to a word, one byte each, and pixel 0 in the LOW byte: that is
// the order the DMA hands a word to HSTX. Bit 7 of a font byte is the leftmost
// pixel, which is what draw_glyph meant by `bits & (0x80 >> x)`.
static const uint32_t expand4[16] = {
    0x00000000, 0xff000000, 0x00ff0000, 0xffff0000,
    0x0000ff00, 0xff00ff00, 0x00ffff00, 0xffffff00,
    0x000000ff, 0xff0000ff, 0x00ff00ff, 0xffff00ff,
    0x0000ffff, 0xff00ffff, 0x00ffffff, 0xffffffff,
};

// Latin-1, not ASCII, and anything below space is a space -- the same mapping
// draw_glyph made, moved here so that the generator never has to make it.
uint8_t myrtos_chargen_glyph(char c)
{
    uint8_t b = (uint8_t)c;
    return (b < 32) ? 0 : (uint8_t)(b - 32);
}

static inline myrtos_cell_t *cell_at(uint32_t row, uint32_t col)
{
    uint32_t r = (row_origin + row) % MYRTOS_CELL_ROWS;
    return &cells[r * MYRTOS_CELL_COLS + col];
}

void myrtos_chargen_put(uint32_t row, uint32_t col, uint8_t glyph, uint8_t attr)
{
    if (row >= MYRTOS_CELL_ROWS || col >= MYRTOS_CELL_COLS)
        return;
    myrtos_cell_t *c = cell_at(row, col);
    c->ch = glyph;
    c->attr = attr;
}

void myrtos_chargen_fill(uint32_t row, uint32_t from, uint32_t to, uint8_t attr)
{
    if (row >= MYRTOS_CELL_ROWS || from > to)
        return;
    if (to >= MYRTOS_CELL_COLS)
        to = MYRTOS_CELL_COLS - 1;
    myrtos_cell_t *c = cell_at(row, from);
    for (uint32_t col = from; col <= to; col++, c++) {
        c->ch = MYRTOS_CELL_BLANK;
        c->attr = attr;
    }
}

void myrtos_chargen_clear(uint8_t attr)
{
    for (uint32_t r = 0; r < MYRTOS_CELL_ROWS; r++)
        myrtos_chargen_fill(r, 0, MYRTOS_CELL_COLS - 1, attr);
}

// Blank the top row and then move the origin past it, in that order: after the
// move that same ring entry IS the bottom row, so it comes round already empty.
// Doing it the other way leaves one frame in which the oldest text is showing
// at the bottom, and the generator runs from an interrupt that would catch it.
void myrtos_chargen_scroll(uint8_t attr)
{
    myrtos_chargen_fill(0, 0, MYRTOS_CELL_COLS - 1, attr);
    row_origin = (row_origin + 1) % MYRTOS_CELL_ROWS;
}

// The cursor is a register here rather than pixels flipped in place, which is
// how a CRTC did it and why it cannot be left inverted by an unmatched call.
void myrtos_chargen_cursor(uint32_t row, uint32_t col, bool on)
{
    cur_row = row;
    cur_col = col;
    cur_on  = on;
}

void myrtos_chargen_init(uint8_t attr)
{
    row_origin = 0;
    cur_on = false;
    myrtos_chargen_clear(attr);
}

void myrtos_chargen_line(uint32_t y, uint8_t *dst)
{
    uint32_t crow = y / MYRTOS_CELL_H;
    uint32_t gy   = y % MYRTOS_CELL_H;
    const myrtos_cell_t *c = cell_at(crow, 0);
    uint32_t *out = (uint32_t *)dst;
    bool on_cursor_row = cur_on && crow == cur_row;

    // The colour words are rebuilt only when the attribute changes, which on a
    // line of text is once or twice. 0x100 cannot equal a byte, so the first
    // cell always builds them.
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < MYRTOS_CELL_COLS; col++, c++) {
        uint32_t attr = c->attr;
        if (on_cursor_row && col == cur_col)
            attr = ((attr & 0x0fu) << 4) | (attr >> 4);
        if (attr != last) {
            last = attr;
            fgw = (uint32_t)myrtos_ansi_colour[attr >> 4]   * 0x01010101u;
            bgw = (uint32_t)myrtos_ansi_colour[attr & 0x0fu] * 0x01010101u;
        }

        uint32_t bits = myrtos_font8x16[c->ch][gy];
        uint32_t m = expand4[bits >> 4];
        *out++ = (fgw & m) | (bgw & ~m);
        m = expand4[bits & 0x0fu];
        *out++ = (fgw & m) | (bgw & ~m);
    }
}

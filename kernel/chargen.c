// The character generator. See chargen.h for the shape and video.c for who
// calls myrtos_chargen_line and when.
#include "chargen.h"

extern const uint8_t myrtos_font8x16[224][MYRTOS_CELL_H];
extern const uint8_t myrtos_ansi_colour[16];

static myrtos_cell_t cells[MYRTOS_CELL_RING * MYRTOS_CELL_COLS];

// Which ring row is the top of the LIVE screen. Scrolling moves this by one and
// clears the row that comes round, which is the whole job -- the same trick the
// framebuffer played with its line table, in units of rows rather than
// scanlines.
static uint32_t row_origin;

// How far back the view is from the live screen, and how much history there is
// to go back through. history saturates: once the ring is full the oldest row
// is being overwritten and there is no more to reach.
static uint32_t view_back;
static uint32_t history;

#define MAX_BACK (MYRTOS_CELL_RING - MYRTOS_CELL_ROWS)

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

// Where the console writes: always the live screen, whatever the view shows.
static inline myrtos_cell_t *cell_at(uint32_t row, uint32_t col)
{
    uint32_t r = (row_origin + row) % MYRTOS_CELL_RING;
    return &cells[r * MYRTOS_CELL_COLS + col];
}

// Where the generator reads: the same, moved back by the view.
static inline const myrtos_cell_t *view_at(uint32_t row)
{
    uint32_t r = (row_origin + MYRTOS_CELL_RING - view_back + row) % MYRTOS_CELL_RING;
    return &cells[r * MYRTOS_CELL_COLS];
}

void myrtos_chargen_put(uint32_t row, uint32_t col, uint8_t glyph, uint8_t attr)
{
    if (row >= MYRTOS_CELL_ROWS || col >= MYRTOS_CELL_COLS)
        return;
    myrtos_cell_t *c = cell_at(row, col);
    c->ch = glyph;
    c->attr = attr;
}

// row may be MYRTOS_CELL_ROWS, which is the row just below the screen: the one
// scroll blanks before moving the origin onto it.
void myrtos_chargen_fill(uint32_t row, uint32_t from, uint32_t to, uint8_t attr)
{
    if (row > MYRTOS_CELL_ROWS || from > to)
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

// --- THE VIEW -------------------------------------------------------------
// Reading back is not a mode: the console goes on writing to the live screen
// the whole time, and only what the generator reads is moved.

uint32_t myrtos_chargen_view_back(void) { return view_back; }

// How far back there is to go: what has been written, capped at what the ring
// can still hold. This is the number that says whether Shift+PgUp will do
// anything, which the ring's capacity does not.
uint32_t myrtos_chargen_history(void)
{
    return history < MAX_BACK ? history : MAX_BACK;
}

void myrtos_chargen_view_move(int32_t rows)
{
    int32_t back = (int32_t)view_back - rows;      // negative rows goes back
    int32_t cap  = (int32_t)(history < MAX_BACK ? history : MAX_BACK);
    if (back < 0)   back = 0;
    if (back > cap) back = cap;
    view_back = (uint32_t)back;
}

void myrtos_chargen_view_end(void) { view_back = 0; }

void myrtos_chargen_view_home(void)
{
    view_back = history < MAX_BACK ? history : MAX_BACK;
}

// Blank the row that is about to become the bottom one, and only then move the
// origin onto it. Doing it the other way leaves one frame in which the oldest
// text in the ring is showing at the bottom of the screen, and the generator
// runs from an interrupt that would catch it.
//
// A view that is scrolled back moves with the origin, so that the text being
// read stays where it is instead of creeping upwards as new lines arrive. Once
// the ring is full it has to creep: the row being looked at is the one about to
// be overwritten.
void myrtos_chargen_scroll(uint8_t attr)
{
    myrtos_chargen_fill(MYRTOS_CELL_ROWS, 0, MYRTOS_CELL_COLS - 1, attr);
    row_origin = (row_origin + 1) % MYRTOS_CELL_RING;

    if (history < MAX_BACK)
        history++;
    if (view_back && view_back < MAX_BACK)
        view_back++;
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
    view_back = history = 0;
    cur_on = false;

    // The whole ring, not just the screen: history that was never written must
    // read as blank rather than as whatever SRAM held at reset.
    for (uint32_t i = 0; i < MYRTOS_CELL_RING * MYRTOS_CELL_COLS; i++) {
        cells[i].ch = MYRTOS_CELL_BLANK;
        cells[i].attr = attr;
    }
}

// One screen row as characters, for looking at what the console actually wrote
// rather than at what came out of the generator. The two are different
// questions and only one of them is answered by the pixels.
void myrtos_chargen_peek_row(uint32_t row, uint8_t *out, uint32_t n)
{
    const myrtos_cell_t *c = view_at(row);
    for (uint32_t i = 0; i < n && i < MYRTOS_CELL_COLS; i++)
        out[i] = (uint8_t)(c[i].ch + 32);      // back to the character it came from
}

void myrtos_chargen_line(uint32_t y, uint8_t *dst)
{
    uint32_t crow = y / MYRTOS_CELL_H;
    uint32_t gy   = y % MYRTOS_CELL_H;
    const myrtos_cell_t *c = view_at(crow);
    uint32_t *out = (uint32_t *)dst;
    bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;

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

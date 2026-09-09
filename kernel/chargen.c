// The character generator. See chargen.h for the shape and video.c for who
// calls myrtos_chargen_line and when.
#include "chargen.h"
#include "tlsf.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

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

// --- THE DEEP HISTORY, IN PSRAM -------------------------------------------
//
// The SRAM ring is what the generator can read, and the generator runs from an
// interrupt above the kernel's priority threshold: it may not touch PSRAM, and
// in the framebuffer days a console writing scattered bytes there held the QMI
// long enough to starve the display. That is why the ring stays where it is.
//
// What PSRAM can do is catch the rows falling out of it. 4096 rows at 80 cells
// of two bytes is 640 kB of the eight megabytes, and about a hundred and thirty
// screens.
//
// Reading them back means copying: when the view reaches past the ring, the
// thirty rows it wants are assembled into an SRAM buffer and the generator is
// pointed at that instead. The copy happens in whoever moved the view -- the
// USB task, with interrupts on -- never in the generator.
//
// This is only safe because chargen took the display off the QMI. The picture
// now streams from linebuf in SRAM, so a PSRAM write no longer competes with it.
#define DEEP_ROWS 4096

extern tlsf_pool_t myrtos_bulk_pool;

static myrtos_cell_t *deep;
static uint32_t deep_head;       // next slot to write
static uint32_t deep_count;      // rows kept, saturating at DEEP_ROWS

static myrtos_cell_t view_buf[MYRTOS_CELL_ROWS * MYRTOS_CELL_COLS];
static bool view_deep;

static uint32_t cur_row, cur_col;
static bool     cur_on;

// Four pixels to a word, one byte each, and pixel 0 in the LOW byte: that is
// the order the DMA hands a word to HSTX. Bit 7 of a font byte is the leftmost
// pixel, which is what draw_glyph meant by `bits & (0x80 >> x)`.
// The three tables the inner loop reads, copied into SRAM at init.
//
// They were const, which puts them in flash, and flash on this chip is read
// through XIP: a cache miss on a font byte costs more than the arithmetic
// around it. The generator reads one font byte and two expansion words per
// cell, eighty cells a line, thirty-one thousand lines a second -- the old
// console read a glyph once when it drew the character and never again.
//
// Measured before this: 56 per cent of the processor for text alone, and a
// worst single pump of 962 microseconds against a 500 microsecond period.
static uint8_t  font_ram[224][MYRTOS_CELL_H];
static uint8_t  pal_ram[16];
static uint32_t expand_ram[16];

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

// Where the generator reads: the ring moved back by the view, or the assembled
// buffer once the view has gone past what the ring holds.
static inline const myrtos_cell_t *view_at(uint32_t row)
{
    if (view_deep)
        return &view_buf[row * MYRTOS_CELL_COLS];
    uint32_t r = (row_origin + MYRTOS_CELL_RING - view_back + row) % MYRTOS_CELL_RING;
    return &cells[r * MYRTOS_CELL_COLS];
}

// One row of history, counted back from the top of the live screen. Rows within
// the ring come from it directly; older ones come from PSRAM, where index 0 is
// the oldest still kept.
static const myrtos_cell_t *row_back(uint32_t back)
{
    if (back <= history) {
        uint32_t r = (row_origin + MYRTOS_CELL_RING - back) % MYRTOS_CELL_RING;
        return &cells[r * MYRTOS_CELL_COLS];
    }
    uint32_t d = back - history;                  // 1 .. deep_count
    uint32_t j = deep_count - d;                  // 0 is the oldest kept
    uint32_t slot = (deep_head + DEEP_ROWS - deep_count + j) % DEEP_ROWS;
    return &deep[slot * MYRTOS_CELL_COLS];
}

// Assemble the window when it reaches past the ring. Called only from thread
// context, because it reads PSRAM.
static void view_rebuild(void)
{
    if (view_back <= history) { view_deep = false; return; }

    for (uint32_t r = 0; r < MYRTOS_CELL_ROWS; r++) {
        const myrtos_cell_t *src = row_back(view_back - r);
        myrtos_cell_t *dst = &view_buf[r * MYRTOS_CELL_COLS];
        for (uint32_t i = 0; i < MYRTOS_CELL_COLS; i++)
            dst[i] = src[i];
    }
    view_deep = true;      // set last: the generator reads this one word
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
    return (history < MAX_BACK ? history : MAX_BACK) + deep_count;
}

uint32_t myrtos_chargen_deep(void) { return deep_count; }

void myrtos_chargen_view_move(int32_t rows)
{
    int32_t back = (int32_t)view_back - rows;      // negative rows goes back
    int32_t cap  = (int32_t)myrtos_chargen_history();
    if (back < 0)   back = 0;
    if (back > cap) back = cap;
    view_back = (uint32_t)back;
    view_rebuild();
}

void myrtos_chargen_view_end(void) { view_back = 0; view_deep = false; }

void myrtos_chargen_view_home(void)
{
    view_back = myrtos_chargen_history();
    view_rebuild();
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
    // The slot that is about to become the bottom row holds the oldest row in
    // the ring once the ring is full. Keep it before it is blanked, or it is
    // simply gone.
    if (deep && history >= MAX_BACK) {
        const myrtos_cell_t *old = cell_at(MYRTOS_CELL_ROWS, 0);
        myrtos_cell_t *dst = &deep[deep_head * MYRTOS_CELL_COLS];
        for (uint32_t i = 0; i < MYRTOS_CELL_COLS; i++)
            dst[i] = old[i];
        deep_head = (deep_head + 1) % DEEP_ROWS;
        if (deep_count < DEEP_ROWS)
            deep_count++;
    }

    myrtos_chargen_fill(MYRTOS_CELL_ROWS, 0, MYRTOS_CELL_COLS - 1, attr);
    row_origin = (row_origin + 1) % MYRTOS_CELL_RING;

    if (history < MAX_BACK)
        history++;

    // A view that is scrolled back moves with the origin so the text being read
    // stays put. Past the ring that also means the assembled window is now one
    // row stale, so it is built again -- which is affordable because it only
    // happens while somebody is reading history and something is printing.
    if (view_back) {
        uint32_t cap = myrtos_chargen_history();
        if (view_back < cap)
            view_back++;
        if (view_deep || view_back > history)
            view_rebuild();
    }
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
    deep_head = deep_count = 0;
    view_deep = false;
    cur_on = false;

    // Once, and only where there is PSRAM to put it. myrtos_mem_alloc_bulk
    // falls back to SRAM when the bulk pool cannot serve the request, and
    // 640 kB of SRAM is not a fallback, it is a failure to boot -- so the
    // address is checked rather than trusted, the way myrtos_pool_of_address
    // learned to.
    for (uint32_t g = 0; g < 224; g++)
        for (uint32_t r = 0; r < MYRTOS_CELL_H; r++)
            font_ram[g][r] = myrtos_font8x16[g][r];
    for (uint32_t i = 0; i < 16; i++) {
        pal_ram[i] = myrtos_ansi_colour[i];
        expand_ram[i] = expand4[i];
    }

    if (!deep && myrtos_bulk_pool) {
        // Straight out of the pool, the way k_driver_alloc does it for drivers
        // and for the same reason: this is never given back, so there is
        // nothing to remember about it. myrtos_mem_alloc_bulk cannot be used
        // here -- it goes through alloc_from, which links every block to a
        // process for cleanup and therefore refuses outright while
        // current_pid is KERNEL_PID, which is what it is this early. That
        // returned a null pointer that the first version wrote through, and
        // the machine died just after "Card ready".
        void *p = myrtos_tlsf_malloc(myrtos_bulk_pool,
                                     DEEP_ROWS * MYRTOS_CELL_COLS * sizeof(myrtos_cell_t));
        if (p && myrtos_tlsf_owns(myrtos_bulk_pool, p))
            deep = (myrtos_cell_t *)p;
        myrtos_print("chargen: ");
        if (deep) {
            myrtos_print_u32(DEEP_ROWS);
            myrtos_print(" rows of history in PSRAM at 0x");
            myrtos_print_hex((uint32_t)(uintptr_t)deep);
            myrtos_print("\n");
        } else {
            myrtos_print("history is the SRAM ring only\n");
        }
    }

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

// A whole glyph row at once: sixteen scanlines, cells in the outer loop.
//
// The per-cell work -- loading the cell, comparing the attribute, rebuilding
// the colour words -- was being redone for each of the sixteen scanlines the
// glyph occupies. Here it happens once. What is left inside is the part that
// cannot be avoided, because it writes 640 by 16 bytes.
//
// The sixteen buffers are contiguous and this relies on it: LINE_BUFS is 48,
// which is three sixteens, and V_ACTIVE is 480, which is thirty of them, so a
// glyph row's scanlines always land in one aligned block that does not wrap.
// video.c checks the alignment before calling.
void myrtos_chargen_band(uint32_t y0, uint8_t *base)
{
    uint32_t crow = y0 / MYRTOS_CELL_H;

    const myrtos_cell_t *c = view_at(crow);
    bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < MYRTOS_CELL_COLS; col++, c++) {
        uint32_t attr = c->attr;
        if (on_cursor_row && col == cur_col)
            attr = ((attr & 0x0fu) << 4) | (attr >> 4);
        if (attr != last) {
            last = attr;
            fgw = (uint32_t)pal_ram[attr >> 4]    * 0x01010101u;
            bgw = (uint32_t)pal_ram[attr & 0x0fu] * 0x01010101u;
        }

        uint32_t *out = (uint32_t *)(base + col * MYRTOS_CELL_W);
        uint32_t ch = c->ch;

        if (ch == MYRTOS_CELL_BLANK) {
            for (uint32_t gy = 0; gy < MYRTOS_CELL_H; gy++) {
                out[0] = bgw;
                out[1] = bgw;
                out += MYRTOS_H_ACTIVE / 4;
            }
            continue;
        }

        const uint8_t *g = font_ram[ch];
        for (uint32_t gy = 0; gy < MYRTOS_CELL_H; gy++) {
            uint32_t bits = g[gy];
            uint32_t m = expand_ram[bits >> 4];
            out[0] = (fgw & m) | (bgw & ~m);
            m = expand_ram[bits & 0x0fu];
            out[1] = (fgw & m) | (bgw & ~m);
            out += MYRTOS_H_ACTIVE / 4;
        }
    }
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
            fgw = (uint32_t)pal_ram[attr >> 4]   * 0x01010101u;
            bgw = (uint32_t)pal_ram[attr & 0x0fu] * 0x01010101u;
        }

        // A blank cell is all background: the font byte would be zero, both
        // masks would be zero, and both words would come out as bgw. Saying so
        // directly skips the font load and four table lookups, and most of a
        // console screen is blank.
        uint32_t ch = c->ch;
        if (ch == MYRTOS_CELL_BLANK) {
            *out++ = bgw;
            *out++ = bgw;
            continue;
        }

        uint32_t bits = font_ram[ch][gy];
        uint32_t m = expand_ram[bits >> 4];
        *out++ = (fgw & m) | (bgw & ~m);
        m = expand_ram[bits & 0x0fu];
        *out++ = (fgw & m) | (bgw & ~m);
    }
}

// The character generator. See chargen.h for the shape and video.c for who
// calls ubiqos_chargen_line and when.
#include "chargen.h"
#include "tlsf.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);

extern const uint8_t ubiqos_font8x16[224][UBIQOS_CELL_H];
extern const uint8_t ubiqos_ansi_colour[16];

static ubiqos_cell_t cells[UBIQOS_CELL_RING * UBIQOS_CELL_COLS];

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

#define MAX_BACK (UBIQOS_CELL_RING - UBIQOS_CELL_ROWS)

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

extern tlsf_pool_t ubiqos_bulk_pool;

static ubiqos_cell_t *deep;
static uint32_t deep_head;       // next slot to write
static uint32_t deep_count;      // rows kept, saturating at DEEP_ROWS

static ubiqos_cell_t view_buf[UBIQOS_CELL_ROWS * UBIQOS_CELL_COLS];
static bool view_deep;

static uint32_t cur_row, cur_col;
static bool     cur_on;

// Four pixels to a word, one byte each, and pixel 0 in the LOW byte: that is
// the order the DMA hands a word to HSTX. Bit 7 of a font byte is the leftmost
// pixel, which is what draw_glyph meant by `bits & (0x80 >> x)`.
// The palette and the expansion table live in SRAM; the FONT DOES NOT, and
// that is a measurement rather than an oversight.
//
// It was copied here once, on the theory that XIP cache misses on font bytes
// dominated -- the generator reads one per cell, eighty a line, thirty-one
// thousand lines a second, where the old console read a glyph once when it
// drew the character. The A/B said the copy changed NOTHING, and it was kept
// anyway until the 3584 bytes were counted against what they bought.
//
// The two small tables stay because they cost 80 bytes between them and are
// read four times per cell.
static uint8_t  pal_ram[16];

// The same sixteen colours as RGB565, for a panel whose pixels are two bytes.
// Converted rather than tabulated a second time: one list of colours that can
// disagree with itself is worse than a conversion nobody has to maintain.
static uint16_t pal16[16];

static uint16_t to565(uint8_t c)
{
    // RGB332, red in bits 7-5 -- see the note beside ubiqos_ansi_colour.
    const uint32_t r = (c >> 5) & 7u, g = (c >> 2) & 7u, b = c & 3u;
    const uint32_t r5 = (r << 2) | (r >> 1);
    const uint32_t g6 = (g << 3) | g;
    const uint32_t b5 = (b << 3) | (b << 1) | (b >> 1);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// The palette, for anything outside this file that draws in the same colours --
// a scene's 8-bit bitmaps, so that four means the same on a drawn screen as it
// does on the console. Sixteen entries; anything above wraps rather than reads
// past the end, because this is called from an interrupt and a bad index there
// is a stall and not a wrong pixel.
// One row of one character's glyph, for anything that draws text outside this
// file. Out of range in either direction gives a blank rather than a read past
// the font, because the caller is an interrupt.
uint32_t ubiqos_chargen_glyph_row(char c, uint32_t row)
{
    if (row >= UBIQOS_CELL_H) return 0;
    const uint8_t g = ubiqos_chargen_glyph(c);
    if (g >= 224) return 0;
    return ubiqos_font8x16[g][row];
}

uint16_t ubiqos_chargen_colour(uint8_t index)
{
    return pal16[index & 0x0fu];
}

// Two pixels to a word, so a mask covers a PAIR of font bits. The leftmost
// pixel is the low half-word, because that is the byte the display reads first.
static const uint32_t expand2[4] = {
    0x00000000u,   // both background
    0xffff0000u,   // the right-hand one lit
    0x0000ffffu,   // the left-hand one
    0xffffffffu,   // both
};
static uint32_t expand_ram[16];

static const uint32_t expand4[16] = {
    0x00000000, 0xff000000, 0x00ff0000, 0xffff0000,
    0x0000ff00, 0xff00ff00, 0x00ffff00, 0xffffff00,
    0x000000ff, 0xff0000ff, 0x00ff00ff, 0xffff00ff,
    0x0000ffff, 0xff00ffff, 0x00ffffff, 0xffffffff,
};

// Latin-1, not ASCII, and anything below space is a space -- the same mapping
// draw_glyph made, moved here so that the generator never has to make it.
uint8_t ubiqos_chargen_glyph(char c)
{
    uint8_t b = (uint8_t)c;
    return (b < 32) ? 0 : (uint8_t)(b - 32);
}

// Where the console writes: always the live screen, whatever the view shows.
static inline ubiqos_cell_t *cell_at(uint32_t row, uint32_t col)
{
    uint32_t r = (row_origin + row) % UBIQOS_CELL_RING;
    return &cells[r * UBIQOS_CELL_COLS + col];
}

// Where the generator reads: the ring moved back by the view, or the assembled
// buffer once the view has gone past what the ring holds.
static inline const ubiqos_cell_t *view_at(uint32_t row)
{
    if (view_deep)
        return &view_buf[row * UBIQOS_CELL_COLS];
    uint32_t r = (row_origin + UBIQOS_CELL_RING - view_back + row) % UBIQOS_CELL_RING;
    return &cells[r * UBIQOS_CELL_COLS];
}

// One row of history, counted back from the top of the live screen. Rows within
// the ring come from it directly; older ones come from PSRAM, where index 0 is
// the oldest still kept.
static const ubiqos_cell_t *row_back(uint32_t back)
{
    if (back <= history) {
        uint32_t r = (row_origin + UBIQOS_CELL_RING - back) % UBIQOS_CELL_RING;
        return &cells[r * UBIQOS_CELL_COLS];
    }
    uint32_t d = back - history;                  // 1 .. deep_count
    uint32_t j = deep_count - d;                  // 0 is the oldest kept
    uint32_t slot = (deep_head + DEEP_ROWS - deep_count + j) % DEEP_ROWS;
    return &deep[slot * UBIQOS_CELL_COLS];
}

// Assemble the window when it reaches past the ring. Called only from thread
// context, because it reads PSRAM.
static void view_rebuild(void)
{
    if (view_back <= history) { view_deep = false; return; }

    for (uint32_t r = 0; r < UBIQOS_CELL_ROWS; r++) {
        const ubiqos_cell_t *src = row_back(view_back - r);
        ubiqos_cell_t *dst = &view_buf[r * UBIQOS_CELL_COLS];
        for (uint32_t i = 0; i < UBIQOS_CELL_COLS; i++)
            dst[i] = src[i];
    }
    view_deep = true;      // set last: the generator reads this one word
}

void ubiqos_chargen_put(uint32_t row, uint32_t col, uint8_t glyph, uint8_t attr)
{
    if (row >= UBIQOS_CELL_ROWS || col >= UBIQOS_CELL_COLS)
        return;
    ubiqos_cell_t *c = cell_at(row, col);
    c->ch = glyph;
    c->attr = attr;
}

// row may be UBIQOS_CELL_ROWS, which is the row just below the screen: the one
// scroll blanks before moving the origin onto it.
void ubiqos_chargen_fill(uint32_t row, uint32_t from, uint32_t to, uint8_t attr)
{
    if (row > UBIQOS_CELL_ROWS || from > to)
        return;
    if (to >= UBIQOS_CELL_COLS)
        to = UBIQOS_CELL_COLS - 1;
    ubiqos_cell_t *c = cell_at(row, from);
    for (uint32_t col = from; col <= to; col++, c++) {
        c->ch = UBIQOS_CELL_BLANK;
        c->attr = attr;
    }
}

void ubiqos_chargen_clear(uint8_t attr)
{
    for (uint32_t r = 0; r < UBIQOS_CELL_ROWS; r++)
        ubiqos_chargen_fill(r, 0, UBIQOS_CELL_COLS - 1, attr);
}

// --- THE VIEW -------------------------------------------------------------
// Reading back is not a mode: the console goes on writing to the live screen
// the whole time, and only what the generator reads is moved.

uint32_t ubiqos_chargen_view_back(void) { return view_back; }

// How far back there is to go: what has been written, capped at what the ring
// can still hold. This is the number that says whether Shift+PgUp will do
// anything, which the ring's capacity does not.
uint32_t ubiqos_chargen_history(void)
{
    return (history < MAX_BACK ? history : MAX_BACK) + deep_count;
}

uint32_t ubiqos_chargen_deep(void) { return deep_count; }

void ubiqos_chargen_view_move(int32_t rows)
{
    int32_t back = (int32_t)view_back - rows;      // negative rows goes back
    int32_t cap  = (int32_t)ubiqos_chargen_history();
    if (back < 0)   back = 0;
    if (back > cap) back = cap;
    view_back = (uint32_t)back;
    view_rebuild();
}

void ubiqos_chargen_view_end(void) { view_back = 0; view_deep = false; }

void ubiqos_chargen_view_home(void)
{
    view_back = ubiqos_chargen_history();
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
void ubiqos_chargen_scroll(uint8_t attr)
{
    // The slot that is about to become the bottom row holds the oldest row in
    // the ring once the ring is full. Keep it before it is blanked, or it is
    // simply gone.
    if (deep && history >= MAX_BACK) {
        const ubiqos_cell_t *old = cell_at(UBIQOS_CELL_ROWS, 0);
        ubiqos_cell_t *dst = &deep[deep_head * UBIQOS_CELL_COLS];
        for (uint32_t i = 0; i < UBIQOS_CELL_COLS; i++)
            dst[i] = old[i];
        deep_head = (deep_head + 1) % DEEP_ROWS;
        if (deep_count < DEEP_ROWS)
            deep_count++;
    }

    ubiqos_chargen_fill(UBIQOS_CELL_ROWS, 0, UBIQOS_CELL_COLS - 1, attr);
    row_origin = (row_origin + 1) % UBIQOS_CELL_RING;

    if (history < MAX_BACK)
        history++;

    // A view that is scrolled back moves with the origin so the text being read
    // stays put. Past the ring that also means the assembled window is now one
    // row stale, so it is built again -- which is affordable because it only
    // happens while somebody is reading history and something is printing.
    if (view_back) {
        uint32_t cap = ubiqos_chargen_history();
        if (view_back < cap)
            view_back++;
        if (view_deep || view_back > history)
            view_rebuild();
    }
}

// The cursor is a register here rather than pixels flipped in place, which is
// how a CRTC did it and why it cannot be left inverted by an unmatched call.
void ubiqos_chargen_cursor(uint32_t row, uint32_t col, bool on)
{
    cur_row = row;
    cur_col = col;
    cur_on  = on;
}

void ubiqos_chargen_init(uint8_t attr)
{
    row_origin = 0;
    view_back = history = 0;
    deep_head = deep_count = 0;
    view_deep = false;
    cur_on = false;

    // Once, and only where there is PSRAM to put it. ubiqos_mem_alloc_bulk
    // falls back to SRAM when the bulk pool cannot serve the request, and
    // 640 kB of SRAM is not a fallback, it is a failure to boot -- so the
    // address is checked rather than trusted, the way ubiqos_pool_of_address
    // learned to.
    for (uint32_t i = 0; i < 16; i++) {
        pal_ram[i] = ubiqos_ansi_colour[i];
        pal16[i]   = to565(ubiqos_ansi_colour[i]);
        expand_ram[i] = expand4[i];
    }

    if (!deep && ubiqos_bulk_pool) {
        // Straight out of the pool, the way k_driver_alloc does it for drivers
        // and for the same reason: this is never given back, so there is
        // nothing to remember about it. ubiqos_mem_alloc_bulk cannot be used
        // here -- it goes through alloc_from, which links every block to a
        // process for cleanup and therefore refuses outright while
        // current_pid is KERNEL_PID, which is what it is this early. That
        // returned a null pointer that the first version wrote through, and
        // the machine died just after "Card ready".
        void *p = ubiqos_tlsf_malloc(ubiqos_bulk_pool,
                                     DEEP_ROWS * UBIQOS_CELL_COLS * sizeof(ubiqos_cell_t));
        if (p && ubiqos_tlsf_owns(ubiqos_bulk_pool, p))
            deep = (ubiqos_cell_t *)p;
        ubiqos_print("chargen: ");
        if (deep) {
            ubiqos_print_u32(DEEP_ROWS);
            ubiqos_print(" rows of history in PSRAM at 0x");
            ubiqos_print_hex((uint32_t)(uintptr_t)deep);
            ubiqos_print("\n");
        } else {
            ubiqos_print("history is the SRAM ring only\n");
        }
    }

    // The whole ring, not just the screen: history that was never written must
    // read as blank rather than as whatever SRAM held at reset.
    for (uint32_t i = 0; i < UBIQOS_CELL_RING * UBIQOS_CELL_COLS; i++) {
        cells[i].ch = UBIQOS_CELL_BLANK;
        cells[i].attr = attr;
    }
}

// One screen row as characters, for looking at what the console actually wrote
// rather than at what came out of the generator. The two are different
// questions and only one of them is answered by the pixels.
void ubiqos_chargen_peek_row(uint32_t row, uint8_t *out, uint32_t n)
{
    const ubiqos_cell_t *c = view_at(row);
    for (uint32_t i = 0; i < n && i < UBIQOS_CELL_COLS; i++)
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
void ubiqos_chargen_band(uint32_t y0, uint8_t *base)
{
    uint32_t crow = y0 / UBIQOS_CELL_H;

    const ubiqos_cell_t *c = view_at(crow);
    bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < UBIQOS_CELL_COLS; col++, c++) {
        uint32_t attr = c->attr;
        if (on_cursor_row && col == cur_col)
            attr = ((attr & 0x0fu) << 4) | (attr >> 4);
        if (attr != last) {
            last = attr;
            fgw = (uint32_t)pal_ram[attr >> 4]    * 0x01010101u;
            bgw = (uint32_t)pal_ram[attr & 0x0fu] * 0x01010101u;
        }

        uint32_t *out = (uint32_t *)(base + col * UBIQOS_CELL_W);
        uint32_t ch = c->ch;

        if (ch == UBIQOS_CELL_BLANK) {
            for (uint32_t gy = 0; gy < UBIQOS_CELL_H; gy++) {
                out[0] = bgw;
                out[1] = bgw;
                out += UBIQOS_H_ACTIVE / 4;
            }
            continue;
        }

        const uint8_t *g = ubiqos_font8x16[ch];
        for (uint32_t gy = 0; gy < UBIQOS_CELL_H; gy++) {
            uint32_t bits = g[gy];
            uint32_t m = expand_ram[bits >> 4];
            out[0] = (fgw & m) | (bgw & ~m);
            m = expand_ram[bits & 0x0fu];
            out[1] = (fgw & m) | (bgw & ~m);
            out += UBIQOS_H_ACTIVE / 4;
        }
    }
}

// The same as ubiqos_chargen_band, in RGB565. A cell is eight pixels wide,
// which is four words here rather than two, and a row of the band is
// UBIQOS_H_ACTIVE/2 words further on.
void ubiqos_chargen_band16(uint32_t y0, uint16_t *base)
{
    const uint32_t crow = y0 / UBIQOS_CELL_H;
    const ubiqos_cell_t *c = view_at(crow);
    const bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < UBIQOS_CELL_COLS; col++, c++) {
        uint32_t attr = c->attr;
        if (on_cursor_row && col == cur_col)
            attr = ((attr & 0x0fu) << 4) | (attr >> 4);
        if (attr != last) {
            last = attr;
            fgw = (uint32_t)pal16[attr >> 4]     * 0x00010001u;
            bgw = (uint32_t)pal16[attr & 0x0fu]  * 0x00010001u;
        }

        uint32_t *out = (uint32_t *)(base + col * UBIQOS_CELL_W);
        const uint32_t ch = c->ch;

        if (ch == UBIQOS_CELL_BLANK) {
            for (uint32_t gy = 0; gy < UBIQOS_CELL_H; gy++) {
                out[0] = bgw; out[1] = bgw; out[2] = bgw; out[3] = bgw;
                out += UBIQOS_H_ACTIVE / 2;
            }
            continue;
        }

        const uint8_t *g = ubiqos_font8x16[ch];
        for (uint32_t gy = 0; gy < UBIQOS_CELL_H; gy++) {
            const uint32_t bits = g[gy];
            uint32_t m;
            m = expand2[(bits >> 6) & 3u]; out[0] = (fgw & m) | (bgw & ~m);
            m = expand2[(bits >> 4) & 3u]; out[1] = (fgw & m) | (bgw & ~m);
            m = expand2[(bits >> 2) & 3u]; out[2] = (fgw & m) | (bgw & ~m);
            m = expand2[ bits       & 3u]; out[3] = (fgw & m) | (bgw & ~m);
            out += UBIQOS_H_ACTIVE / 2;
        }
    }
}

// One scanline in RGB565, for the diagnostic peek. The band renderer would
// write sixteen rows and is the wrong tool for a caller that wants one -- which
// is not a style preference: handing it a one-line buffer overruns it eightfold.
void ubiqos_chargen_line16(uint32_t y, uint16_t *dst)
{
    const uint32_t crow = y / UBIQOS_CELL_H;
    const uint32_t gy   = y % UBIQOS_CELL_H;
    const ubiqos_cell_t *c = view_at(crow);
    const bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;
    uint32_t *out = (uint32_t *)dst;
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < UBIQOS_CELL_COLS; col++, c++) {
        uint32_t attr = c->attr;
        if (on_cursor_row && col == cur_col)
            attr = ((attr & 0x0fu) << 4) | (attr >> 4);
        if (attr != last) {
            last = attr;
            fgw = (uint32_t)pal16[attr >> 4]    * 0x00010001u;
            bgw = (uint32_t)pal16[attr & 0x0fu] * 0x00010001u;
        }

        const uint32_t ch = c->ch;
        if (ch == UBIQOS_CELL_BLANK) {
            *out++ = bgw; *out++ = bgw; *out++ = bgw; *out++ = bgw;
            continue;
        }

        const uint32_t bits = ubiqos_font8x16[ch][gy];
        uint32_t m;
        m = expand2[(bits >> 6) & 3u]; *out++ = (fgw & m) | (bgw & ~m);
        m = expand2[(bits >> 4) & 3u]; *out++ = (fgw & m) | (bgw & ~m);
        m = expand2[(bits >> 2) & 3u]; *out++ = (fgw & m) | (bgw & ~m);
        m = expand2[ bits       & 3u]; *out++ = (fgw & m) | (bgw & ~m);
    }
}

void ubiqos_chargen_line(uint32_t y, uint8_t *dst)
{

    uint32_t crow = y / UBIQOS_CELL_H;
    uint32_t gy   = y % UBIQOS_CELL_H;
    const ubiqos_cell_t *c = view_at(crow);
    uint32_t *out = (uint32_t *)dst;
    bool on_cursor_row = cur_on && view_back == 0 && crow == cur_row;

    // The colour words are rebuilt only when the attribute changes, which on a
    // line of text is once or twice. 0x100 cannot equal a byte, so the first
    // cell always builds them.
    uint32_t last = 0x100, fgw = 0, bgw = 0;

    for (uint32_t col = 0; col < UBIQOS_CELL_COLS; col++, c++) {
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
        if (ch == UBIQOS_CELL_BLANK) {
            *out++ = bgw;
            *out++ = bgw;
            continue;
        }

        uint32_t bits = ubiqos_font8x16[ch][gy];
        uint32_t m = expand_ram[bits >> 4];
        *out++ = (fgw & m) | (bgw & ~m);
        m = expand_ram[bits & 0x0fu];
        *out++ = (fgw & m) | (bgw & ~m);
    }
}

// The 800x480 RGB panel on the Waveshare RP2350-Touch-LCD-4.3B.
//
// Four state machines and a DMA, and kernel/pio/rgb.pio is where the timing
// lives -- read that first, especially the note about pio_set_gpio_base.
//
// This is the first half of the port's display work. What it does now is put a
// pattern on the panel: one scanline, repeated for every line of every frame.
// That is deliberate rather than lazy. A full framebuffer here is 800 x 480 x
// two bytes = 768 kB, which does not fit in 520 kB of SRAM and must not live in
// PSRAM -- forty megabytes a second of scanout through the QMI, while code
// executes from the same QMI, is the contention that stopped the other board.
// So the panel will be driven by the character generator, and a single repeated
// line is the cheapest thing that proves the wiring first: vertical bars in the
// right colours mean the pixel clock, the data enable, all sixteen data pins
// and the bit order are each correct.

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "rgb.pio.h"

void ubiqos_print(const char *s);
uint32_t ubiqos_psram_bytes(void);
void ubiqos_print_u32(uint32_t v);

#include "chargen.h"
#include "../common/ubiqos_abi.h"
#include "../common/ubiqos_abi.h"

#define RGB_W UBIQOS_H_ACTIVE
#define RGB_H UBIQOS_V_ACTIVE

// One character row to a band, which is what makes the arithmetic disappear:
// the renderer is handed a cell row and fills it, and the interrupt rate is one
// per row rather than one per line.
//
// 16 lines x 800 pixels x two bytes is 25 kB, and there are two so that one can
// be drawn while the other is read. 51 kB of SRAM, against the 768 kB a whole
// framebuffer would want and the QMI contention it would cost.
#define BAND_LINES  UBIQOS_CELL_H
#define BAND_PIXELS (RGB_W * BAND_LINES)

// Sync on one PIO block, pixels on another. Two of the chip's three, which
// leaves exactly one for the PIO USB host and nothing at all spare.
#define PIO_SYNC  pio1
#define PIO_DATA  pio2

// PIO's pin window starts here, so PIO pin 4 is GPIO20. Everything in the .pio
// file counts from this.
#define RGB_GPIO_BASE 16u

// The one Pico-PIO-USB demands by number. Written here rather than included,
// because the library's header is not this file's business -- but the number is,
// and it must not drift from PIO_USB_DMA_TX_DEFAULT.
#define PIO_USB_DMA_TX_CHANNEL 0u

static uint16_t band[2][BAND_PIXELS] __attribute__((aligned(4)));
static int ch[2];                 // one DMA channel per band, each chaining to the other
static volatile uint32_t next_row;  // the cell row the finished band will be redrawn as
static uint32_t band_row[2];        // which row each band holds, for the check at frame start

// How many bands the panel has asked for and how many were ready in time. A
// band drawn late is a band of the previous frame shown twice, which is a
// flicker rather than a fault -- but it is the number to look at if the picture
// ever tears, and counting it costs nothing.
uint32_t ubiqos_video_pumps, ubiqos_video_late;

// --- the scene -------------------------------------------------------------
//
// An application's screen, as a list the interrupt reads rather than code it
// calls. See ubiqos_draw_item_t: a callback would put module code in flash on
// the path of an interrupt, which is the one thing the QMI forbids.
//
// Set and cleared from a thread, read from the interrupt. The count is written
// last when a scene is set and first when it is cleared, so the interrupt never
// sees a count that outlives its list.
static const ubiqos_draw_item_t *scene;
static volatile uint32_t scene_count;

// The costliest band since vidstat last asked, and which row it was. The worst
// single call says there is a problem; this says where on the screen it is.
static uint32_t band_worst_us, band_worst_row;

// And what it went on, by draw kind -- the band just drawn, and the worst. The
// costliest band's total said a curve's band cost a millisecond; which part of
// it did is the only thing that says what to change, and two guesses at that
// were wrong before this was measured.
static uint32_t band_kind_us[6], band_worst_kind_us[6];

static inline void note_band(uint32_t row, uint32_t us)
{
    if (us > band_worst_us) {
        band_worst_us = us;
        band_worst_row = row;
        for (uint32_t k = 0; k < 6; k++) band_worst_kind_us[k] = band_kind_us[k];
    }
}

// A scene must live in SRAM, all of it, and this REFUSES one that does not.
//
// The interrupt reads the list and every bitmap in it, and nothing reached from
// an interrupt may be in PSRAM or flash -- the QMI stalls one master against the
// other. A module with writable data is private, a private module is copied into
// the PSRAM pool, and so its __thread arrays are in PSRAM: the obvious way to
// hold a scene is the wrong one. Measured 13 Sep 2026, an hour after this file
// was written: a module put its glyphs in __thread, the pump read them, and core
// 0 died at 0x110d1732 with UNDEFINSTR forced to a hard fault -- a PSRAM address,
// executing PSRAM code while the interrupt read PSRAM data.
//
// So the check is here rather than in a comment somebody has to remember.
// ubiqos_alloc gives SRAM and ubiqos_alloc_bulk gives PSRAM; a scene wants the
// first. A refusal is a message, and a stalled bus is not.
//
// Both ends of the window, because SRAM is at 0x20000000 and PSRAM at
// 0x11000000: "above the PSRAM base" is true of SRAM too, and this repository
// has paid for that mistake twice already.
static bool in_psram(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    const uint32_t n = ubiqos_psram_bytes();
    return n && a >= UBIQOS_PSRAM_BASE && a < (uintptr_t)UBIQOS_PSRAM_BASE + n;
}

int32_t ubiqos_video_set_scene(const ubiqos_draw_item_t *items, uint32_t count)
{
    if (!items || !count) { scene_count = 0; scene = 0; return 0; }

    if (in_psram(items)) return -1;
    for (uint32_t i = 0; i < count; i++) {
        // A kind this kernel does not know would be drawn as whatever the last
        // branch below is, out of an array of the wrong size. Refused instead.
        if (items[i].kind > UBIQOS_DRAW_PLOT_LINE) return -1;
        if (items[i].kind >= UBIQOS_DRAW_PLOT_FILL && items[i].h > 255u) return -1;
        if (items[i].kind != UBIQOS_DRAW_RECT && in_psram(items[i].data)) return -1;
    }

    scene_count = 0;
    scene = items;
    __dmb();
    scene_count = count;
    return 0;
}

// A run of one colour, two pixels to a word where the alignment allows it.
//
// Measured: one halfword store a pixel made a scene cost 1040 us a band against
// the character generator's 213, and the generator is doing more work -- it
// packs two pixels into a word, which is why. A flat fill has no excuse to be
// slower than glyphs.
static inline void fill_span(uint16_t *p, uint32_t n, uint16_t colour)
{
    if (n && ((uintptr_t)p & 2u)) { *p++ = colour; n--; }   // odd start
    const uint32_t pair = (uint32_t)colour * 0x00010001u;
    uint32_t *w = (uint32_t *)p;
    uint32_t words = n >> 1;

    // Four at a time, because the loop itself was costing as much as the
    // stores: a compare and a branch per word doubled the price of a fill.
    // Measured across the change, a scene of one background and three bars went
    // from 457 us a band to what vidstat says now.
    while (words >= 4) { w[0] = pair; w[1] = pair; w[2] = pair; w[3] = pair;
                         w += 4; words -= 4; }
    while (words--) *w++ = pair;
    if (n & 1u) *(uint16_t *)w = colour;
}

// One item, clipped to the band. y0 is the band's first screen line.
static void draw_item_into(const ubiqos_draw_item_t *it, uint32_t y0,
                           uint32_t lines, uint16_t *base)
{
    int32_t x0 = it->x, y = it->y;
    const uint32_t text_scale = it->w ? (it->w > 8u ? 8u : it->w) : 1u;
    int32_t x1, y1;
    if (it->kind == UBIQOS_DRAW_TEXT) {
        uint32_t n = 0;
        for (const char *t = (const char *)it->data; t && *t; t++) n++;
        x1 = x0 + (int32_t)(n * 8u * text_scale);
        y1 = y + (int32_t)(UBIQOS_CELL_H * text_scale);
    } else {
        x1 = x0 + (int32_t)it->w;
        y1 = y + (int32_t)it->h;
    }

    if (x0 < 0) x0 = 0;
    if (y  < (int32_t)y0) y = (int32_t)y0;
    if (x1 > (int32_t)RGB_W) x1 = (int32_t)RGB_W;
    if (y1 > (int32_t)(y0 + lines)) y1 = (int32_t)(y0 + lines);
    if (x0 >= x1 || y >= y1) return;

    // A curve, drawn a COLUMN at a time rather than a line at a time.
    //
    // Line by line was the obvious shape, since everything else here is, and it
    // cost 1625 us a band against a deadline of 719, measured on a 656-column
    // chart: every line tested every column, twice over for the fill and the
    // stroke. A column knows at once which of the band's lines it lights, so the
    // test is made once a column and only the lit pixels are touched.
    //
    // A column is TWO pixels wide, so a lit column is one 32-bit store rather
    // than two 16-bit ones, and there are half as many columns to walk. A chart
    // of 504 readings across 656 pixels loses nothing it could show.
    //
    // And everything the loops need is copied out of the item first. A store
    // through a uint16_t pointer might, as far as the compiler can tell, change
    // the item's int16_t fields -- so it read x and y back from the item and
    // multiplied out the row again for every column. The disassembly showed it:
    // twenty instructions a column around a four-instruction store, and a curve
    // line that cost 245 us for two thousand pixels.
    if (it->kind == UBIQOS_DRAW_PLOT_FILL || it->kind == UBIQOS_DRAW_PLOT_LINE) {
        const int32_t ix = it->x, iy = it->y;
        const uint16_t colour = it->colour;
        const uint32_t pair = (uint32_t)colour * 0x00010001u;
        const uint8_t *top = (const uint8_t *)it->data;
        const int32_t first = y - iy;              // item rows this band covers
        const int32_t last  = y1 - iy;             //   (exclusive)

        // Where each of those rows starts in the band buffer. A band row is
        // RGB_W halfwords from the last, which is a whole number of words, so a
        // pixel at an even x is word-aligned on every row.
        uint16_t *row_at[BAND_LINES];
        for (int32_t r = first; r < last; r++)
            row_at[r - first] = base + (uint32_t)(r + iy - (int32_t)y0) * RGB_W;
        const uint32_t word_down = RGB_W / 2u;

        const int32_t c0 = (x0 - ix) / 2;
        const int32_t c1 = (x1 - ix + 1) / 2;

        // Rows from..to-1 of one column, clipped to x0..x1 at the chart's edges.
        #define PLOT_COLUMN(c, from, to)                                          \
            do {                                                                  \
                int32_t a = ix + 2 * (c), b = a + 2;                              \
                if (a < x0) a = x0;                                               \
                if (b > x1) b = x1;                                               \
                if (b - a == 2 && !(a & 1)) {                                     \
                    uint32_t *w = (uint32_t *)(row_at[(from) - first] + a);       \
                    for (int32_t r = (from); r < (to); r++, w += word_down)       \
                        *w = pair;                                                \
                } else {                                                          \
                    for (int32_t r = (from); r < (to); r++)                       \
                        for (int32_t x = a; x < b; x++) row_at[r - first][x] = colour; \
                }                                                                 \
            } while (0)

        if (it->kind == UBIQOS_DRAW_PLOT_FILL) {
            // The row from which every column in reach is lit. Below it the
            // band is a flat run of the fill colour, and fill_span does that
            // two pixels a word; only above it do columns differ.
            //
            // Runs along each line were tried instead -- which columns are lit
            // answered four at a time -- and cost 2.2 ms on a band this does in
            // far less: a noisy curve alternates lit and dark nearly every
            // column on the lines it crosses, and hundreds of short runs a line
            // were hundreds of calls. A store a column has no cost a run.
            int32_t all = first;
            for (int32_t c = c0; c < c1; c++) {
                const int32_t t = top[c];
                if (t == (int32_t)UBIQOS_PLOT_NONE) { all = last; break; }
                if (t > all) all = t;
            }
            if (all > last) all = last;

            for (int32_t c = c0; c < c1; c++) {
                const int32_t t = top[c];
                if (t == (int32_t)UBIQOS_PLOT_NONE) continue;
                const int32_t from = t < first ? first : t;
                if (from >= all) continue;
                PLOT_COLUMN(c, from, all);
            }
            for (int32_t r = all; r < last; r++)
                fill_span(row_at[r - first] + x0, (uint32_t)(x1 - x0), colour);
        } else {
            // Each column from the previous column's row to its own, one row
            // deeper, so a flat stretch is two pixels thick and a spike is a
            // connected stroke up and down.
            for (int32_t c = c0; c < c1; c++) {
                const int32_t t = top[c];
                if (t == (int32_t)UBIQOS_PLOT_NONE) continue;
                int32_t prev = c ? top[c - 1] : t;
                if (prev == (int32_t)UBIQOS_PLOT_NONE) prev = t;
                int32_t from = prev < t ? prev : t;
                int32_t to   = (prev < t ? t : prev) + 2;      // exclusive
                if (from < first) from = first;
                if (to > last) to = last;
                if (from >= to) continue;
                PLOT_COLUMN(c, from, to);
            }
        }
        #undef PLOT_COLUMN
        return;
    }

    for (int32_t sy = y; sy < y1; sy++) {
        uint16_t *row = base + (uint32_t)(sy - (int32_t)y0) * RGB_W;

        if (it->kind == UBIQOS_DRAW_RECT) {
            fill_span(row + x0, (uint32_t)(x1 - x0), it->colour);
            continue;
        }

        const int32_t line = sy - it->y;
        if (it->kind == UBIQOS_DRAW_MASK) {
            // One bit a pixel, the top bit leftmost, rows byte-aligned.
            //
            // A byte at a time, and an EMPTY byte skips eight pixels without
            // looking at them. Most of a glyph cell is empty -- the digits are
            // fifty pixels wide in a cell that has to hold the widest of them --
            // and testing every bit of that cost 660 us on the band a big
            // number sat in, against a deadline of 719.
            const uint8_t *bits = (const uint8_t *)it->data
                                + (uint32_t)line * ((it->w + 7u) / 8u);
            int32_t sx = x0;
            while (sx < x1) {
                const int32_t u = sx - it->x;
                const uint8_t b = bits[u >> 3];
                const int32_t bit = u & 7;
                if (!b) {                       // eight pixels of nothing
                    sx += 8 - bit;
                    continue;
                }
                const int32_t end = sx + (8 - bit) < x1 ? sx + (8 - bit) : x1;
                for (int32_t k = bit; sx < end; sx++, k++)
                    if (b & (uint8_t)(0x80u >> k)) row[sx] = it->colour;
            }
        } else if (it->kind == UBIQOS_DRAW_TEXT) {
            // The console's font, scaled by whole numbers. Each character is
            // eight wide and sixteen tall before scaling, so the row of the
            // glyph is the band line divided by the scale.
            const uint32_t scale = it->w ? (it->w > 8u ? 8u : it->w) : 1u;
            const uint32_t gy = (uint32_t)line / scale;
            if (gy >= UBIQOS_CELL_H) continue;

            const char *str = (const char *)it->data;
            for (uint32_t c = 0; str[c]; c++) {
                const int32_t cx = it->x + (int32_t)(c * 8u * scale);
                if (cx >= x1) break;
                if (cx + (int32_t)(8u * scale) <= x0) continue;

                const uint32_t bits = ubiqos_chargen_glyph_row(str[c], gy);
                if (!bits) continue;
                for (uint32_t b = 0; b < 8u; b++) {
                    if (!(bits & (0x80u >> b))) continue;
                    const int32_t px = cx + (int32_t)(b * scale);
                    for (uint32_t k = 0; k < scale; k++) {
                        const int32_t sx2 = px + (int32_t)k;
                        if (sx2 >= x0 && sx2 < x1) row[sx2] = it->colour;
                    }
                }
            }
        } else {
            // A byte a pixel, straight through the chargen palette, so a scene
            // and the console agree about what colour four means.
            const uint8_t *px = (const uint8_t *)it->data + (uint32_t)line * it->w;
            for (int32_t sx = x0; sx < x1; sx++)
                row[sx] = ubiqos_chargen_colour(px[sx - it->x]);
        }
    }
}

// One line of one item, which is draw_item with a band one line tall. Written
// as its own name because peek_line means something different from the pump and
// the two should not be read as the same call.
static void draw_item_line(const ubiqos_draw_item_t *it, uint32_t y, uint16_t *out)
{
    ubiqos_draw_item_t one_line = *it;
    draw_item_into(&one_line, y, 1u, out);
}

static void draw_band(uint32_t which, uint32_t row)
{
    const uint32_t n = scene_count;
    if (n) {
        const uint32_t y0 = row * UBIQOS_CELL_H;
        uint16_t *base = band[which];

        // Drawing starts at the LAST rectangle that covers the whole band,
        // because nothing listed before it can show, and the band is cleared
        // only when there is no such rectangle at all.
        //
        // It used to look at the first item alone, which is the background. A
        // card drawn over the background then filled the same band twice --
        // measured at 272 us of rectangles on the band of a chart where the
        // deadline is 719 -- and a scene can avoid that only if the kernel
        // looks for the covering rectangle wherever it is in the list.
        uint32_t start = 0;
        bool covered = false;
        for (uint32_t i = n; i-- > 0; ) {
            const ubiqos_draw_item_t *it = &scene[i];
            if (it->kind == UBIQOS_DRAW_RECT &&
                it->x <= 0 &&
                it->x + (int32_t)it->w >= (int32_t)RGB_W &&
                it->y <= (int32_t)y0 &&
                it->y + (int32_t)it->h >= (int32_t)(y0 + BAND_LINES)) {
                start = i;
                covered = true;
                break;
            }
        }
        for (uint32_t k = 0; k < 6; k++) band_kind_us[k] = 0;
        uint32_t then = time_us_32();
        if (!covered) fill_span(base, BAND_PIXELS, 0);
        uint32_t now = time_us_32();
        band_kind_us[UBIQOS_DRAW_RECT] += now - then;

        for (uint32_t i = start; i < n; i++) {
            const uint32_t kind = scene[i].kind <= UBIQOS_DRAW_PLOT_LINE ? scene[i].kind : 0u;
            then = now;
            draw_item_into(&scene[i], y0, BAND_LINES, base);
            now = time_us_32();
            band_kind_us[kind] += now - then;
        }
    } else {
        for (uint32_t k = 0; k < 6; k++) band_kind_us[k] = 0;
        ubiqos_chargen_band16(row * UBIQOS_CELL_H, band[which]);
    }
    ubiqos_video_pumps++;
}

#define BL_WRAP 1000u

// Nought is dark and a hundred is as bright as the panel goes. The level is
// inverted because the backlight is active low -- see the note below, which is
// the only place that fact is written down anywhere.
int32_t ubiqos_video_backlight(uint32_t percent)
{
    if (percent > 100u) percent = 100u;
    pwm_set_gpio_level(UBIQOS_LCD_BL_PIN,
                       (uint16_t)(BL_WRAP / 100u * (100u - percent)));
    return (int32_t)percent;
}

// The panel's own three pins. EN and RST are plain GPIO; the backlight is on a
// PWM slice on the real hardware but full brightness is a high pin, and dimming
// is not what this step is about.
static void panel_wake(void)
{
    gpio_init(UBIQOS_LCD_EN_PIN);
    gpio_set_dir(UBIQOS_LCD_EN_PIN, GPIO_OUT);
    gpio_put(UBIQOS_LCD_EN_PIN, 1);

    gpio_init(UBIQOS_LCD_RST_PIN);
    gpio_set_dir(UBIQOS_LCD_RST_PIN, GPIO_OUT);
    gpio_put(UBIQOS_LCD_RST_PIN, 0);
    sleep_ms(20);
    gpio_put(UBIQOS_LCD_RST_PIN, 1);
    sleep_ms(200);                      // Waveshare's own wait, and generous

    // THE BACKLIGHT IS ACTIVE LOW, and this pin driven high is what "the panel
    // is completely black" turned out to mean -- with every signal measured
    // correct, DE at 70% and the data pins changing.
    //
    // Nothing says so anywhere. It is deducible only from their brightness
    // function, which writes PWM_WRAP/100 * (100 - percent) as the level: at a
    // hundred per cent that is zero. Low is bright.
    //
    // PWM rather than a level, so the panel can be dimmed. Full brightness on a
    // 4.3 inch white screen lights a room at night, and there are eyes it simply
    // shuts out: cone dystrophy makes bright light disabling rather than merely
    // uncomfortable, and a display nobody can look at shows nothing.
    //
    // The slice runs at about 1.2 kHz -- above flicker, below the range where a
    // backlight driver starts to whine.
    gpio_set_function(UBIQOS_LCD_BL_PIN, GPIO_FUNC_PWM);
    {
        pwm_config c = pwm_get_default_config();
        pwm_config_set_clkdiv_int(&c, 100);
        pwm_config_set_wrap(&c, BL_WRAP);
        pwm_init(pwm_gpio_to_slice_num(UBIQOS_LCD_BL_PIN), &c, true);
    }
    ubiqos_video_backlight(UBIQOS_BACKLIGHT_DEFAULT);
}

// pio_sm_init refuses a configuration its block's GPIO base cannot reach, and
// says so rather than running wrong. That return value is the one check that
// catches the whole class of mistake this driver is most exposed to, so it is
// not thrown away.
static bool sm_start(PIO pio, uint sm, uint off, pio_sm_config *c, const char *what)
{
    if (pio_sm_init(pio, sm, off, c) == PICO_OK) return true;
    ubiqos_print("lcd: the ");
    ubiqos_print(what);
    ubiqos_print(" state machine would not take its pins\n");
    return false;
}

// The band that has just been read is the one to redraw, and the other is
// already going out. That is the whole of the double buffering: no ownership
// flag, no waiting, and the interrupt does its work on the buffer nothing is
// reading.
// What the pump costs, because the next thing to go in it is a rasteriser and
// nobody should find out afterwards whether there was room. Cumulative since
// boot, like the chargen driver's, so the share vidstat prints is an average
// and not a sample -- and the worst single call is kept separately, because an
// average that fits says nothing about a deadline that does not.
static uint32_t pump_us_total;
static uint32_t pump_us_worst;
static uint32_t pump_period_us;
static uint32_t pump_last_us;

// The bands that have finished since last asked. Shared by the band interrupt
// and the frame-start one below, which has to account for a band that finished
// just before it before it can judge whether the count is right.
static void service_bands(void)
{
    for (uint32_t i = 0; i < 2; i++) {
        if (!(dma_hw->ints0 & (1u << ch[i]))) continue;
        dma_hw->ints0 = 1u << ch[i];

        // Back to the start of its own band. A chained trigger reloads the
        // transfer COUNT and not the read address, so without this each channel
        // carried on from wherever it had got to: after one frame the two were
        // reading 0x2007de4c and 0x20082000 -- the second exactly one byte past
        // the end of SRAM -- and the DMA raised a read error and stopped. Thirty
        // pumps, one frame, then nothing. That is what the address channel in
        // the single-line version was for, and taking it away with the bands was
        // the mistake.
        dma_channel_set_read_addr(ch[i], band[i], false);

        // Two rows ahead: this band will be read again after the other one, so
        // it must hold the row after the row now going out.
        const uint32_t row = (next_row + 1u) % UBIQOS_CELL_ROWS;
        const uint32_t d0 = time_us_32();
        draw_band(i, row);
        note_band(row, time_us_32() - d0);
        band_row[i] = row;
        next_row = row;
    }
}

static void pump_account(uint32_t t0)
{
    const uint32_t took = time_us_32() - t0;
    pump_us_total += took;
    if (took > pump_us_worst) pump_us_worst = took;
}

static void on_band_done(void)
{
    const uint32_t t0 = time_us_32();
    if (pump_last_us) pump_period_us = t0 - pump_last_us;
    pump_last_us = t0;
    service_bands();
    pump_account(t0);
}

// Once a frame, as vertical blanking begins: is the row count still right?
//
// It is a count of interrupts, and an interrupt can be lost. Each DMA channel
// has ONE completion bit, so a pump call that runs longer than two bands -- a
// scene with a chart took 2482 us, switching tabs -- lets a channel finish twice
// and be counted once. From then on every band was drawn as the row above the
// one the beam was on, and the picture stood sixteen lines down for good; a
// second loss made it thirty-two. Nothing noticed, because ubiqos_video_late was
// declared and never incremented.
//
// So the count is checked once a frame, at the one place its answer is known.
// The vsync machine raises IRQ 0 when its count of active lines runs out, which
// is as the last line begins; the pixels then stand still for about four
// milliseconds of blanking. Depending on how quickly this runs, the busy
// channel is either finishing row 29 or already stalled on row 0, and the
// channel's read address says which. If the bands and the count disagree with
// that, the bands nobody is reading are drawn again, the count is put right, and
// it is counted as late. In step, this costs a few register reads a frame.
static void on_frame_start(void)
{
    const uint32_t t0 = time_us_32();
    pio_interrupt_clear(PIO_SYNC, 0);
    service_bands();

    const bool busy0 = dma_channel_is_busy(ch[0]);
    const bool busy1 = dma_channel_is_busy(ch[1]);
    if (busy0 == busy1) return;              // the pixels have not started yet

    const uint32_t going = busy0 ? 0u : 1u;
    const uint32_t other = 1u - going;

    // Which side of the frame's last band this is, from how far the busy
    // channel has read. A few pixels in, it is row 0, stalled for the blanking.
    // Most of the way through, it is still row 29: the interrupt came as the
    // last line began, which is when vsync's count of lines runs out.
    //
    // Assuming the first cost a band every frame. Measured on the history
    // page: 459 corrections in twelve seconds, a redraw of the band the beam
    // was reading each time, a flicker above the arrows, and a correction long
    // enough -- 2466 us -- to lose a real interrupt of its own.
    const uint32_t read =
        (uint32_t)((uintptr_t)dma_hw->ch[ch[going]].read_addr - (uintptr_t)band[going]) / 2u;

    if (read < BAND_PIXELS / 2u) {
        if (band_row[going] == 0u && band_row[other] == 1u && next_row == 1u) {
            pump_account(t0);
            return;
        }
        // Both are still: nothing reads either until the first line.
        ubiqos_video_late++;
        draw_band(going, 0);
        band_row[going] = 0;
        draw_band(other, 1);
        band_row[other] = 1;
        next_row = 1;
    } else {
        const uint32_t last = UBIQOS_CELL_ROWS - 1u;
        if (band_row[going] == last && band_row[other] == 0u && next_row == 0u) {
            pump_account(t0);
            return;
        }
        // The last band is going out and is left alone, whatever it holds; the
        // one to follow it is made row 0 of the next frame.
        ubiqos_video_late++;
        draw_band(other, 0);
        band_row[other] = 0;
        next_row = 0;
    }
    pump_account(t0);
}

// Configured, NOT started. Starting it before the state machines cost a boot:
// the DMA filled the pixel machine's TX FIFO while that machine was still
// stopped, and pio_sm_put_blocking then waited for ever for room that nothing
// would make. The probe found it in pio_sm_is_tx_fifo_full, which was a better
// clue than a black screen.
static void dma_setup(uint data_sm)
{
    // NOT channel 0, where a board with a PIO USB host is concerned.
    //
    // Pico-PIO-USB takes its transmit channel by FIXED index -- PIO_USB_DMA_TX
    // _DEFAULT is 0 -- with dma_claim_mask, and it does so on core 1, started
    // fourteen lines before this in main. dma_claim_unused_channel hands out the
    // lowest free one, so core 0 got 0 and 1 for the bands and core 1 then
    // claimed a channel it did not own. The assertion that would have said so is
    // compiled out under -DNDEBUG, so what happened instead was two masters
    // programming one channel: the band pump froze and the render interrupt was
    // left standing in it.
    //
    // Holding 0 across our own two claims settles it, and giving it straight
    // back leaves it for the library that insists on it. Asking whether it is
    // already taken makes the order the two cores arrive in stop mattering: if
    // core 1 got there first the channel is its own, and we must neither claim
    // nor release what we do not hold.
    const bool reserve_for_pio_usb = UBIQOS_HAS_PIO_USB_HOST &&
                                     !dma_channel_is_claimed(PIO_USB_DMA_TX_CHANNEL);
    if (reserve_for_pio_usb) dma_channel_claim(PIO_USB_DMA_TX_CHANNEL);
    ch[0] = dma_claim_unused_channel(true);
    ch[1] = dma_claim_unused_channel(true);
    if (reserve_for_pio_usb) dma_channel_unclaim(PIO_USB_DMA_TX_CHANNEL);

    // Each band feeds the pixel machine a halfword at a time and then hands
    // over to the other, so the two run round for ever and the picture never
    // stops even if a redraw is late.
    for (uint32_t i = 0; i < 2; i++) {
        dma_channel_config d = dma_channel_get_default_config(ch[i]);
        channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
        channel_config_set_read_increment(&d, true);
        channel_config_set_write_increment(&d, false);
        channel_config_set_dreq(&d, pio_get_dreq(PIO_DATA, data_sm, true));
        channel_config_set_chain_to(&d, ch[1u - i]);
        dma_channel_configure(ch[i], &d, &PIO_DATA->txf[data_sm],
                              band[i], BAND_PIXELS, false);
        dma_channel_set_irq0_enabled(ch[i], true);
    }

    // BELOW THE KERNEL, at the same 0xC0 kernel/video.c settled on, and for the
    // same reason stated at greater length there. Left at the default, which is
    // zero and therefore the highest, this handler fires inside the kernel's
    // own critical sections -- and the first thing it did was deadlock the
    // machine in spin_lock_unsafe_blocking with the tick frozen at 13.
    //
    // The ordering follows from who has slack. A band is 717 microseconds of
    // it; a USB frame, due every millisecond, has none. So this sits below both
    // PIO-USB's timer and the kernel's threshold, and a band drawn late shows
    // the previous one again -- which ubiqos_video_late counts.
    //
    // Like the pump on the other board, it calls nothing: two DMA registers,
    // cells, font bytes, pixels.
    irq_add_shared_handler(DMA_IRQ_0, on_band_done, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_priority(DMA_IRQ_0, 0xc0);
    irq_set_enabled(DMA_IRQ_0, true);
}

// What `vidstat` asks for. The same sixteen slots as video.c fills for the other
// display, and the ones that have no meaning here are left at zero rather than
// filled with a number that would read as a measurement.
void ubiqos_video_stats_fill(uint32_t *sixteen)
{
    for (uint32_t i = 0; i < 16; i++) sixteen[i] = 0;
    sixteen[1]  = ubiqos_video_pumps;
    sixteen[3]  = 2;                          // bands, not scanline buffers
    sixteen[6]  = ubiqos_chargen_view_back();
    sixteen[7]  = ubiqos_chargen_history();
    sixteen[8]  = ubiqos_chargen_deep();
    sixteen[10] = pump_us_total;
    sixteen[11] = pump_us_worst;
    // Slot 0 is what vidstat prints as underruns, and a frame whose row count
    // had to be put right is this panel's underrun. It sat in slot 13, which
    // vidstat labels "Glyph rows built" -- where nobody would have looked.
    sixteen[0]  = ubiqos_video_late;

    // The costliest band since the last ask, as its microseconds above the row
    // plus one, so that zero still means nothing to report. Cleared on reading:
    // a worst-since-boot hides whether the page on the screen now is the costly
    // one.
    sixteen[12] = band_worst_us ? (band_worst_us << 8) | (band_worst_row + 1u) : 0u;

    // Its breakdown, in slots the character generator's own figures use on the
    // other board; vidstat prints these instead of those when slot 12 is set.
    sixteen[2]  = band_worst_kind_us[UBIQOS_DRAW_RECT];
    sixteen[4]  = band_worst_kind_us[UBIQOS_DRAW_MASK];
    sixteen[5]  = band_worst_kind_us[UBIQOS_DRAW_TEXT];
    sixteen[13] = band_worst_kind_us[UBIQOS_DRAW_PLOT_FILL];
    sixteen[14] = band_worst_kind_us[UBIQOS_DRAW_PLOT_LINE];
    band_worst_us = 0;
    sixteen[15] = pump_period_us;
}

// A scanline, rendered on demand rather than read back out of a band: the bands
// hold two character rows between them and any other line is not in memory at
// all. Rendering it is both cheaper than keeping it and always current.
// One scanline exactly as the panel shows it, RGB565, the whole width into out.
// What a screenshot is made of: the rasteriser the pump uses, given a band one
// line tall, or the character generator when no scene is set. It runs in a
// thread, so it may take its time.
//
// Every item is offered, and the rasteriser clips each to the line itself. The
// peek below used to skip an item whose h did not cover the line first -- and
// text keeps its scale in w and leaves h at zero, so a peek of a scene had no
// text in it at all.
//
// The list is read once, pointer and count, the way the pump reads it. An
// application that replaces its list rather than rewriting it in place could
// still be caught between the two; airview never does.
uint32_t ubiqos_video_capture_line(uint32_t y, uint16_t *out)
{
    if (y >= RGB_H) {
        for (uint32_t i = 0; i < RGB_W; i++) out[i] = 0;
        return RGB_W;
    }
    const ubiqos_draw_item_t *items = scene;
    const uint32_t n = scene_count;
    if (n && items) {
        for (uint32_t i = 0; i < RGB_W; i++) out[i] = 0;
        for (uint32_t i = 0; i < n; i++) {
            ubiqos_draw_item_t it = items[i];
            draw_item_line(&it, y, out);
        }
    } else {
        ubiqos_chargen_line16(y, out);
    }
    return RGB_W;
}

void ubiqos_video_peek_line(uint32_t y, uint8_t *out, uint32_t n)
{
    static uint16_t one[UBIQOS_H_ACTIVE];
    ubiqos_video_capture_line(y, one);
    const uint16_t *src = one;

    // Handed back a byte per pixel, because that is what the caller's buffer is
    // and what the other display gives it. The high byte of each pixel is
    // enough to tell lit from unlit, which is what a peek is for.
    for (uint32_t i = 0; i < n; i++)
        out[i] = (i < UBIQOS_H_ACTIVE) ? (uint8_t)(src[i] >> 8) : 0u;
}

void ubiqos_video_init(void)
{
    // The first two rows, before anything is scanning them out.
    draw_band(0, 0);
    draw_band(1, 1);
    band_row[0] = 0;
    band_row[1] = 1;
    next_row = 1;

    panel_wake();

    // The window, before anything is configured against it.
    pio_set_gpio_base(PIO_SYNC, RGB_GPIO_BASE);
    pio_set_gpio_base(PIO_DATA, RGB_GPIO_BASE);

    const uint sm_hsync = pio_claim_unused_sm(PIO_SYNC, true);
    const uint sm_vsync = pio_claim_unused_sm(PIO_SYNC, true);
    const uint sm_de    = pio_claim_unused_sm(PIO_DATA, true);
    const uint sm_data  = pio_claim_unused_sm(PIO_DATA, true);

    const uint off_hsync = pio_add_program(PIO_SYNC, &ubiqos_rgb_hsync_program);
    const uint off_vsync = pio_add_program(PIO_SYNC, &ubiqos_rgb_vsync_program);
    const uint off_de    = pio_add_program(PIO_DATA, &ubiqos_rgb_de_program);
    const uint off_data  = pio_add_program(PIO_DATA, &ubiqos_rgb_data_program);

    // Only hsync is divided: it owns the time, at two instructions per pixel.
    // The other three follow its edges and its interrupt, so they run flat out.
    const float div = (float)clock_get_hz(clk_sys) / (float)(UBIQOS_LCD_PCLK_HZ * 2u);

    bool ok = true;
    {
        pio_sm_config c = ubiqos_rgb_hsync_program_get_default_config(off_hsync);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, UBIQOS_LCD_HSYNC_PIN);   // and PCLK, next along
        sm_config_set_clkdiv(&c, div);
        pio_gpio_init(PIO_SYNC, UBIQOS_LCD_HSYNC_PIN);
        pio_gpio_init(PIO_SYNC, UBIQOS_LCD_PCLK_PIN);
        pio_sm_set_consecutive_pindirs(PIO_SYNC, sm_hsync, UBIQOS_LCD_HSYNC_PIN, 2, true);
        ok &= sm_start(PIO_SYNC, sm_hsync, off_hsync, &c, "hsync");
    }
    {
        pio_sm_config c = ubiqos_rgb_vsync_program_get_default_config(off_vsync);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, UBIQOS_LCD_VSYNC_PIN);
        pio_gpio_init(PIO_SYNC, UBIQOS_LCD_VSYNC_PIN);
        pio_sm_set_consecutive_pindirs(PIO_SYNC, sm_vsync, UBIQOS_LCD_VSYNC_PIN, 1, true);
        ok &= sm_start(PIO_SYNC, sm_vsync, off_vsync, &c, "vsync");
    }
    {
        pio_sm_config c = ubiqos_rgb_de_program_get_default_config(off_de);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, UBIQOS_LCD_DE_PIN);
        pio_gpio_init(PIO_DATA, UBIQOS_LCD_DE_PIN);
        pio_sm_set_consecutive_pindirs(PIO_DATA, sm_de, UBIQOS_LCD_DE_PIN, 1, true);
        ok &= sm_start(PIO_DATA, sm_de, off_de, &c, "data enable");
    }
    {
        pio_sm_config c = ubiqos_rgb_data_program_get_default_config(off_data);
        sm_config_set_out_pins(&c, UBIQOS_LCD_DATA0_PIN, 16);
        for (uint i = 0; i < 16; i++) pio_gpio_init(PIO_DATA, UBIQOS_LCD_DATA0_PIN + i);
        pio_sm_set_consecutive_pindirs(PIO_DATA, sm_data, UBIQOS_LCD_DATA0_PIN, 16, true);
        ok &= sm_start(PIO_DATA, sm_data, off_data, &c, "pixel");
    }
    if (!ok) { ubiqos_print("lcd: not started\n"); return; }

    dma_setup(sm_data);

    // The frame-start check, on IRQ 0 of the sync block -- see on_frame_start.
    // At the band interrupt's priority, so neither can run inside the other and
    // the row count has one writer at a time.
    pio_set_irq0_source_enabled(PIO_SYNC, pis_interrupt0, true);
    irq_add_shared_handler(PIO_IRQ_NUM(PIO_SYNC, 0), on_frame_start,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_priority(PIO_IRQ_NUM(PIO_SYNC, 0), 0xc0);
    irq_set_enabled(PIO_IRQ_NUM(PIO_SYNC, 0), true);

    // Each machine is told how far to count before any of them runs.
    pio_sm_put_blocking(PIO_SYNC, sm_hsync, RGB_W - 1u);
    pio_sm_put_blocking(PIO_SYNC, sm_vsync, RGB_H - 1u);
    // Two words: the height it counts lines with, then the width it counts the
    // data enable window with. See the note in rgb.pio about where each lives.
    pio_sm_put_blocking(PIO_DATA, sm_de,    RGB_H - 1u);
    pio_sm_put_blocking(PIO_DATA, sm_de,    RGB_W - 1u);
    pio_sm_put_blocking(PIO_DATA, sm_data,  RGB_W - 1u);

    // The followers first, so they are already waiting when time starts.
    pio_enable_sm_mask_in_sync(PIO_DATA, (1u << sm_de) | (1u << sm_data));
    pio_enable_sm_mask_in_sync(PIO_SYNC, (1u << sm_hsync) | (1u << sm_vsync));

    // And only now the pixels. The machines have taken their counts and are
    // waiting on the first data enable, so the first thing the DMA feeds is a
    // pixel rather than something the program was going to read as a width.
    dma_channel_start(ch[0]);

    ubiqos_print("lcd: 800x480 RGB565, pixel clock ");
    ubiqos_print_u32(UBIQOS_LCD_PCLK_HZ / 1000000u);
    ubiqos_print(" MHz, ");
    ubiqos_print_u32(UBIQOS_CELL_COLS);
    ubiqos_print(" by ");
    ubiqos_print_u32(UBIQOS_CELL_ROWS);
    ubiqos_print(" characters\n");
}

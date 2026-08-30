// A text console on the framebuffer: 80 columns by 30 rows of 8x16 glyphs.
//
// Scrolling does not move pixels. The display is played from a table of line
// addresses, so the framebuffer is treated as a ring and scrolling advances the
// origin -- 480 pointer stores in SRAM instead of 300 kB of copying in PSRAM.
// Only the row that comes round to the bottom has to be cleared.

#include <stdint.h>
#include <stdbool.h>
#include "video.h"

#define CELL_W 8
#define CELL_H 16
// A margin, because the monitor does not show the whole picture. This Samsung
// cuts a few pixels off the left, which is invisible for a solid block like the
// cursor and eats the first letter of every line. Overscan varies by display, so
// the text keeps a character's width clear on each side rather than assuming the
// edge is reachable.
#define MARGIN_X 8
#define MARGIN_Y 8
#define COLS   ((MYRTOS_H_ACTIVE - 2 * MARGIN_X) / CELL_W)   // 78
#define ROWS   ((MYRTOS_V_ACTIVE - 2 * MARGIN_Y) / CELL_H)   // 29

#define FG 0xff     // white
#define BG 0x00     // black

extern const uint8_t myrtos_font8x16[95][16];

static uint32_t cur_col, cur_row;
static bool ready;

// Row and glyph-line to a scanline in the framebuffer, through the origin.
static inline uint8_t *cell_line(uint32_t row, uint32_t y) {
    uint32_t fb = (myrtos_video_origin + MARGIN_Y + row * CELL_H + y) % MYRTOS_V_ACTIVE;
    return &myrtos_framebuf[fb * MYRTOS_H_ACTIVE];
}

static void draw_glyph(uint32_t col, uint32_t row, char c, bool invert) {
    uint32_t idx = (c < 32 || c > 126) ? 0 : (uint32_t)(c - 32);
    for (uint32_t y = 0; y < CELL_H; y++) {
        uint8_t bits = myrtos_font8x16[idx][y];
        uint8_t *p = cell_line(row, y) + MARGIN_X + col * CELL_W;
        for (uint32_t x = 0; x < CELL_W; x++) {
            bool on = (bits & (0x80u >> x)) != 0;
            p[x] = (on != invert) ? FG : BG;
        }
    }
}

static void clear_row(uint32_t row) {
    for (uint32_t y = 0; y < CELL_H; y++) {
        uint8_t *p = cell_line(row, y);
        for (uint32_t x = 0; x < MYRTOS_H_ACTIVE; x++) p[x] = BG;   // full width
    }
}

static void cursor(bool on) { draw_glyph(cur_col, cur_row, ' ', on); }

static void newline(void) {
    cur_col = 0;
    if (++cur_row >= ROWS) {
        cur_row = ROWS - 1;
        myrtos_video_set_origin(myrtos_video_origin + CELL_H);
        clear_row(ROWS - 1);            // the row that just came round
    }
}

void myrtos_console_putc(char c) {
    if (!ready) return;
    cursor(false);
    switch (c) {
    case '\n': newline();                                  break;
    case '\r': cur_col = 0;                                break;
    case '\b': if (cur_col) { cur_col--; draw_glyph(cur_col, cur_row, ' ', false); } break;
    case '\t': do { draw_glyph(cur_col, cur_row, ' ', false);
                    if (++cur_col >= COLS) newline();
               } while (cur_col % 8);                      break;
    default:
        if ((unsigned char)c < 32) break;
        draw_glyph(cur_col, cur_row, c, false);
        if (++cur_col >= COLS) newline();
        break;
    }
    cursor(true);
}

void myrtos_console_init(void) {
    myrtos_video_set_origin(0);
    for (uint32_t i = 0; i < MYRTOS_H_ACTIVE * MYRTOS_V_ACTIVE; i++)
        myrtos_framebuf[i] = BG;                 // margins included
    cur_col = cur_row = 0;
    ready = true;
    cursor(true);
}

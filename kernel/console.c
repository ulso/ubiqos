// A text console on the framebuffer: 80 columns by 30 rows of 8x16 glyphs.
//
// Scrolling does not move pixels. The display is played from a table of line
// addresses, so the framebuffer is treated as a ring and scrolling advances the
// origin -- 480 pointer stores in SRAM instead of 300 kB of copying in PSRAM.
// Only the row that comes round to the bottom has to be cleared.

#include <stdint.h>
#include <stdbool.h>
#include "video.h"
#include "hardware/sync.h"

#include "../common/modules.h"   // myrtos_sleep, through the shared ABI

void myrtos_print(const char *s);

#define CELL_W 8
#define CELL_H 16
// No margin. There was one for a while, on the theory that the monitor cut a few
// pixels off the left -- every line was missing its first character. It was not
// overscan; it was the cursor eating them, see below. With that fixed the screen
// divides exactly: 640 by 8 and 480 by 16, with nothing left over.
#define COLS   (MYRTOS_H_ACTIVE / CELL_W)   // 80
#define ROWS   (MYRTOS_V_ACTIVE / CELL_H)   // 30

#define FG 0xff     // white
#define BG 0x00     // black

extern const uint8_t myrtos_font8x16[224][16];

// What is in each cell. The cursor used to be drawn as a solid block and lifted
// by drawing a space, which destroyed whatever was under it: myrtos_print emits
// a carriage return before every newline, so the cursor landed on column zero of
// the line just written and ate its first character, on every line. Keeping the
// text means the cursor can be lifted by redrawing what was really there.
static char cell_char[ROWS][COLS];
static uint32_t cur_col, cur_row;

// A line of exactly eighty characters used to break twice: once when the
// eightieth was written and again when the newline arrived, leaving a blank
// line behind. Real terminals hold the wrap back -- the cursor stays on the
// last column and only moves when another character actually turns up, so a
// newline right after a full line does what it says and nothing more.
static bool wrap_pending;
static bool ready;

// Row and glyph-line to a scanline in the framebuffer, through the origin.
static inline uint8_t *cell_line(uint32_t row, uint32_t y) {
    uint32_t fb = (myrtos_video_origin + row * CELL_H + y) % MYRTOS_V_ACTIVE;
    return &myrtos_framebuf[fb * MYRTOS_H_ACTIVE];
}

// Four pixels per word, looked up a nibble at a time: two stores per scanline
// instead of eight. A cell is eight pixels wide and every line of the
// framebuffer is a multiple of four bytes, so the alignment always works out.
static uint32_t nibble[16], nibble_inv[16];

static void build_nibbles(void) {
    for (uint32_t n = 0; n < 16; n++) {
        uint32_t w = 0, wi = 0;
        for (uint32_t b = 0; b < 4; b++) {
            bool on = (n >> (3 - b)) & 1;
            w  |= (uint32_t)(on ? FG : BG) << (b * 8);
            wi |= (uint32_t)(on ? BG : FG) << (b * 8);
        }
        nibble[n] = w; nibble_inv[n] = wi;
    }
}

static void draw_glyph(uint32_t col, uint32_t row, char c, bool invert) {
    // Latin-1, not ASCII: a Swedish keyboard produces letters above 126 and
    // they have to land somewhere. Anything below space is drawn as one.
    uint8_t b = (uint8_t)c;
    uint32_t idx = (b < 32) ? 0 : (uint32_t)(b - 32);
    const uint32_t *t = invert ? nibble_inv : nibble;
    for (uint32_t y = 0; y < CELL_H; y++) {
        uint8_t bits = myrtos_font8x16[idx][y];
        uint32_t *p = (uint32_t*)(cell_line(row, y) + col * CELL_W);
        p[0] = t[bits >> 4];
        p[1] = t[bits & 0x0f];
    }
}

static void clear_row(uint32_t row) {
    for (uint32_t y = 0; y < CELL_H; y++) {
        uint32_t *p = (uint32_t*)cell_line(row, y);
        for (uint32_t x = 0; x < MYRTOS_H_ACTIVE / 4; x++) p[x] = nibble[0];
    }
    for (uint32_t c = 0; c < COLS; c++) cell_char[row][c] = ' ';
}

static void put_cell(uint32_t col, uint32_t row, char c) {
    cell_char[row][col] = c;
    draw_glyph(col, row, c, false);
}

static void cursor(bool on) {
    draw_glyph(cur_col, cur_row, cell_char[cur_row][cur_col], on);
}

static void newline(void) {
    cur_col = 0;
    if (++cur_row >= ROWS) {
        cur_row = ROWS - 1;
        myrtos_video_set_origin(myrtos_video_origin + CELL_H);
        for (uint32_t r = 1; r < ROWS; r++)          // the text moves up with it
            for (uint32_t c = 0; c < COLS; c++) cell_char[r - 1][c] = cell_char[r][c];
        clear_row(ROWS - 1);            // the row that just came round
    }
}

static void draw_char(char c) {
    if (!ready) return;
    cursor(false);
    switch (c) {
    case '\n':
        wrap_pending = false;
        newline();
        break;
    case '\r':
        wrap_pending = false;
        cur_col = 0;
        break;
    case '\b':
        if (wrap_pending) { wrap_pending = false; put_cell(cur_col, cur_row, ' '); }
        else if (cur_col)  { cur_col--; put_cell(cur_col, cur_row, ' '); }
        break;
    case '\t':
        do {
            if (wrap_pending) { wrap_pending = false; newline(); }
            put_cell(cur_col, cur_row, ' ');
            if (++cur_col >= COLS) { cur_col = COLS - 1; wrap_pending = true; }
        } while (cur_col % 8);
        break;
    default:
        if ((unsigned char)c < 32) break;
        if (wrap_pending) { wrap_pending = false; newline(); }
        put_cell(cur_col, cur_row, c);
        if (++cur_col >= COLS) { cur_col = COLS - 1; wrap_pending = true; }
        break;
    }
    cursor(true);
}

// --- THE RING -------------------------------------------------------------
// Drawing a character is expensive and used to happen wherever the write came
// from -- which for a process meant inside the trap, with interrupts off. Now
// nothing draws there. A write copies bytes into this ring and returns; a kernel
// thread drains it and does the drawing in process context, where the scheduler
// can take the processor away from it whenever the USB task wants it.
//
// The critical section is around the copy alone, a few microseconds, rather than
// around the drawing. That is the difference between protecting the bookkeeping,
// which genuinely needs it, and holding the machine for the duration of some
// hundred and fifty kilobytes of pixels, which never did.
//
// It also settles an old race: the kernel prints with interrupts on and a
// process writes from inside a trap, so both could once be halfway through a
// glyph at the same time. Only the server draws now, so cur_col and cur_row have
// exactly one writer.
#define RING_SIZE 4096u
#define RING_MASK (RING_SIZE - 1u)

static uint8_t ring[RING_SIZE];
static volatile uint32_t ring_head, ring_tail;
static volatile bool server_up;

static uint32_t ring_used(void) { return (ring_head - ring_tail) & RING_MASK; }

uint32_t myrtos_console_room(void) { return RING_SIZE - 1u - ring_used(); }

// Returns how much was taken. Nothing taken means full, and the caller's write
// blocks on WAIT_WRITE exactly as it does for a full USB endpoint -- machinery
// that already existed and needed no changing.
uint32_t myrtos_console_put(const uint8_t *buf, uint32_t len) {
    uint32_t st = save_and_disable_interrupts();
    uint32_t room = RING_SIZE - 1u - ((ring_head - ring_tail) & RING_MASK);
    if (len > room) len = room;
    for (uint32_t i = 0; i < len; i++)
        ring[(ring_head + i) & RING_MASK] = buf[i];
    ring_head = (ring_head + len) & RING_MASK;
    restore_interrupts(st);
    return len;
}

// The kernel's own printing goes the same way, so that it too is drawn by the
// server and cannot interleave with a module's output mid-character. Before the
// server exists there is nothing else running, so drawing directly is safe.
void myrtos_console_putc(char c) {
    if (!server_up) { draw_char(c); return; }
    uint8_t b = (uint8_t)c;
    while (myrtos_console_put(&b, 1) == 0) { /* the kernel waits; it is rare */ }
}

static void console_thread(void) {
    server_up = true;
    for (;;) {
        while (ring_tail != ring_head) {
            draw_char((char)ring[ring_tail]);
            ring_tail = (ring_tail + 1) & RING_MASK;
        }
        myrtos_sleep(1);
    }
}

void myrtos_console_start_server(void) {
    extern int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes,
                                        uint32_t priority);
    // Below the USB task, which must never wait for pixels, and above a shell,
    // so output drains rather than queuing behind whatever asked for it.
    if (myrtos_kernel_thread(console_thread, 2048, MYRTOS_PRIO_CONSOLE) < 0)
        myrtos_print("Console: could not start its service process\n");
}

void myrtos_console_init(void) {
    build_nibbles();
    myrtos_video_set_origin(0);
    for (uint32_t i = 0; i < MYRTOS_H_ACTIVE * MYRTOS_V_ACTIVE; i++)
        myrtos_framebuf[i] = BG;                 // margins included
    cur_col = cur_row = 0;
    wrap_pending = false;
    ready = true;
    cursor(true);
}

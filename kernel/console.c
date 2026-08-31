// A text console on the framebuffer, in whichever of the kernel's fonts is
// current: 80 columns by 30 rows of 8x16 glyphs, or 106 by 40 of 6x12.
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

// Both tables have the same shape: one byte per scanline, leftmost pixel in bit
// 7, so a six pixel cell leaves the low two bits clear. That is what lets one
// piece of drawing code serve both -- see tools/make_font.py.
extern const uint8_t myrtos_font8x16[224][16];
extern const uint8_t myrtos_font6x12[224][12];

typedef struct {
    const uint8_t *bits;
    uint8_t        w, h;
} console_font_t;

// A font has no name beyond its cell: "6x12" is what the two numbers say, and
// leaving it at that means nothing has to be kept in step with anything.
static const console_font_t fonts[] = {
    { &myrtos_font8x16[0][0], 8, 16 },
    { &myrtos_font6x12[0][0], 6, 12 },
};
#define NFONTS (sizeof(fonts) / sizeof(fonts[0]))

// The most rows the smallest cell gives. Both cell heights divide 480 exactly,
// which matters: the framebuffer is a ring and a height that did not divide
// would leave a partial row straddling the join.
#define ROWS_MAX (MYRTOS_V_ACTIVE / 12)    // 40

// The framebuffer is RGB332 -- red in bits 7-5, green in 4-2, blue in 1-0 --
// which is not guesswork: video.c's test card draws its bars from these very
// values and the pattern was checked against a monitor. The bright half is
// those same eight bars; the normal half is the same hues at about half
// intensity, and colour 8 is a grey rather than a second black.
static const uint8_t ansi_colour[16] = {
    0x00, 0x80, 0x10, 0x90, 0x02, 0x82, 0x12, 0x92,
    0x49, 0xe0, 0x1c, 0xfc, 0x03, 0xe3, 0x1f, 0xff,
};

#define COL_DEFAULT_FG 15   // white
#define COL_DEFAULT_BG 0    // black

static uint8_t fg_index = COL_DEFAULT_FG, bg_index = COL_DEFAULT_BG;
static bool    reverse_video;

// Reverse swaps the two everywhere rather than at each use, so nothing has to
// remember to honour it.
static inline uint8_t eff_fg(void) {
    return ansi_colour[reverse_video ? bg_index : fg_index];
}
static inline uint8_t eff_bg(void) {
    return ansi_colour[reverse_video ? fg_index : bg_index];
}

static const console_font_t *font = &fonts[0];
static uint32_t cell_w = 8, cell_h = 16;
static uint32_t cols = MYRTOS_H_ACTIVE / 8, rows = MYRTOS_V_ACTIVE / 16;
// 106 columns of six pixels come to 636, four short of the line. Split them, so
// what is left over sits as two pixels at each edge rather than four at one.
// Eight divides 640 exactly and leaves none.
//
// There was once a margin of whole characters, added on the theory that the
// monitor was cutting the first one off every line. It was not the monitor; it
// was the cursor, see below, and the margin came out again when that was fixed.
static uint32_t x_margin;

static uint32_t cur_col, cur_row;

// The cursor is whatever is under it, inverted; lifting it inverts the same
// pixels back. That restores exactly what was there and needs no record of it.
//
// It was drawn as a solid block once, and lifted by drawing a space -- which
// destroyed whatever it had covered. myrtos_print emits a carriage return
// before every newline, so the cursor landed on column zero of the line just
// written and ate its first character, on every line. The fix then was to keep
// a copy of the text; inverting is the fix that needs no copy, and four
// kilobytes of screen buffer went back to the processes.
static bool cursor_shown;

// A line of exactly eighty characters used to break twice: once when the
// eightieth was written and again when the newline arrived, leaving a blank
// line behind. Real terminals hold the wrap back -- the cursor stays on the
// last column and only moves when another character actually turns up, so a
// newline right after a full line does what it says and nothing more.
static bool wrap_pending;

// Which rows ended because the text ran off the edge, rather than because a
// newline arrived. Backspace may walk back up through the first kind and must
// not walk up through the second: what is above an explicit newline is a line
// that was finished, and nobody is editing it any more.
static bool row_wrapped[ROWS_MAX];
static bool ready;

// Row and glyph-line to a scanline in the framebuffer, through the origin.
static inline uint8_t *cell_line(uint32_t row, uint32_t y) {
    uint32_t fb = (myrtos_video_origin + row * cell_h + y) % MYRTOS_V_ACTIVE;
    return &myrtos_framebuf[fb * MYRTOS_H_ACTIVE];
}

// Four background pixels in a word, for clearing.
static inline uint32_t bg_word(void) { return (uint32_t)eff_bg() * 0x01010101u; }

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    // Latin-1, not ASCII: a Swedish keyboard produces letters above 126 and
    // they have to land somewhere. Anything below space is drawn as one.
    uint8_t b = (uint8_t)c;
    uint32_t idx = (b < 32) ? 0 : (uint32_t)(b - 32);
    const uint8_t *g = font->bits + idx * cell_h;
    uint8_t on = eff_fg(), off = eff_bg();
    // A byte per pixel. The eight pixel cell used to go out as two words, four
    // pixels at a time through a nibble table; six pixels do not divide into
    // words and the trick went with the cell. It costs a few dozen stores per
    // character in a kernel thread that draws with interrupts on, which is not
    // where the console's time goes.
    for (uint32_t y = 0; y < cell_h; y++) {
        uint8_t bits = g[y];
        uint8_t *p = cell_line(row, y) + x_margin + col * cell_w;
        for (uint32_t x = 0; x < cell_w; x++)
            p[x] = (bits & (0x80u >> x)) ? on : off;
    }
}

static void clear_row(uint32_t row) {
    uint32_t w = bg_word();
    for (uint32_t y = 0; y < cell_h; y++) {
        uint32_t *p = (uint32_t*)cell_line(row, y);
        for (uint32_t x = 0; x < MYRTOS_H_ACTIVE / 4; x++) p[x] = w;
    }
    row_wrapped[row] = false;
}

// Idempotent, because it has to be: inverting twice by mistake would leave the
// cell wrong side out with nothing to say so.
static void cursor(bool on) {
    if (on == cursor_shown) return;
    for (uint32_t y = 0; y < cell_h; y++) {
        uint8_t *p = cell_line(cur_row, y) + x_margin + cur_col * cell_w;
        for (uint32_t x = 0; x < cell_w; x++) p[x] = (uint8_t)~p[x];
    }
    cursor_shown = on;
}

static void newline(void) {
    cur_col = 0;
    if (++cur_row >= rows) {
        cur_row = rows - 1;
        myrtos_video_set_origin(myrtos_video_origin + cell_h);
        for (uint32_t r = 1; r < rows; r++)          // the flags move up with it
            row_wrapped[r - 1] = row_wrapped[r];
        clear_row(rows - 1);            // the row that just came round
    }
}

static void draw_char(char c) {
    switch (c) {
    case '\n':
        wrap_pending = false;
        row_wrapped[cur_row] = false;
        newline();
        break;
    case '\r':
        wrap_pending = false;
        cur_col = 0;
        break;
    case '\b':
        if (wrap_pending) {
            wrap_pending = false;
            draw_glyph(cur_col, cur_row, ' ');
        } else if (cur_col) {
            cur_col--;
            draw_glyph(cur_col, cur_row, ' ');
        } else if (cur_row && row_wrapped[cur_row - 1]) {
            cur_row--;
            cur_col = cols - 1;
            row_wrapped[cur_row] = false;
            draw_glyph(cur_col, cur_row, ' ');
        }
        break;
    case '\t':
        do {
            if (wrap_pending) { wrap_pending = false; row_wrapped[cur_row] = true; newline(); }
            draw_glyph(cur_col, cur_row, ' ');
            if (++cur_col >= cols) { cur_col = cols - 1; wrap_pending = true; }
        } while (cur_col % 8);
        break;
    default:
        if ((unsigned char)c < 32) break;
        if (wrap_pending) { wrap_pending = false; row_wrapped[cur_row] = true; newline(); }
        draw_glyph(cur_col, cur_row, c);
        if (++cur_col >= cols) { cur_col = cols - 1; wrap_pending = true; }
        break;
    }
    // The cursor is not put back here. It goes back once the run of characters
    // is done -- see the server -- because putting it back after every one of
    // them means drawing it twice per character and watching it flicker its way
    // across a screen of output.
}

// --- ESCAPE SEQUENCES -----------------------------------------------------
// Enough ANSI to edit a command line on: move the cursor, erase part of a line
// or the screen, and set colours. The shell needs it because it has two very
// different terminals to talk to -- a real emulator over the serial port and
// this console on the monitor -- and it can only have one idea of how to redraw
// a line. Teaching this end the same language the other end already speaks is
// what makes one idea enough.
//
// Unknown sequences are dropped rather than drawn. A terminal that prints the
// escapes it does not understand turns one unsupported sequence into a screen
// of rubbish.

#define MAX_PARAMS 8

static enum { ST_NORMAL, ST_ESC, ST_CSI } esc_state;
static uint32_t params[MAX_PARAMS];
static uint32_t nparams;
static uint32_t saved_col, saved_row;

// Inclusive, and clamped, because a parameter arrives from whoever is writing.
static void erase_cells(uint32_t row, uint32_t from, uint32_t to) {
    if (row >= rows || from > to) return;
    if (to >= cols) to = cols - 1;
    uint8_t b = eff_bg();
    for (uint32_t y = 0; y < cell_h; y++) {
        uint8_t *p = cell_line(row, y) + x_margin + from * cell_w;
        for (uint32_t x = 0; x < (to - from + 1) * cell_w; x++) p[x] = b;
    }
}

static uint32_t param(uint32_t i, uint32_t dflt) {
    return (i < nparams && params[i]) ? params[i] : dflt;
}

// 30-37 and 40-47 are the eight colours, 90-97 and 100-107 the bright ones,
// which is the whole of the sixteen the framebuffer table holds.
static void set_graphics(void) {
    if (!nparams) { params[0] = 0; nparams = 1; }
    for (uint32_t i = 0; i < nparams; i++) {
        uint32_t v = params[i];
        if (v == 0) { fg_index = COL_DEFAULT_FG; bg_index = COL_DEFAULT_BG;
                      reverse_video = false; }
        else if (v == 1)  fg_index |= 8;          // bold is bright here
        else if (v == 7)  reverse_video = true;
        else if (v == 22) fg_index &= 7;
        else if (v == 27) reverse_video = false;
        else if (v >= 30  && v <= 37)   fg_index = (uint8_t)(v - 30);
        else if (v == 39) fg_index = COL_DEFAULT_FG;
        else if (v >= 40  && v <= 47)   bg_index = (uint8_t)(v - 40);
        else if (v == 49) bg_index = COL_DEFAULT_BG;
        else if (v >= 90  && v <= 97)   fg_index = (uint8_t)(v - 90 + 8);
        else if (v >= 100 && v <= 107)  bg_index = (uint8_t)(v - 100 + 8);
    }
}

// ESC [ row ; col R, back to whoever is reading this console -- which is the
// keyboard, because that is what the console reads.
void myrtos_usbhost_push_str(const char *s);

static void report_position(void) {
    char buf[16], *p = buf;
    uint32_t v[2] = { cur_row + 1, cur_col + 1 };
    *p++ = 0x1b; *p++ = '[';
    for (uint32_t i = 0; i < 2; i++) {
        char tmp[6];
        uint32_t n = 0, x = v[i];
        do { tmp[n++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (n) *p++ = tmp[--n];
        *p++ = i ? 'R' : ';';
    }
    *p = 0;
    myrtos_usbhost_push_str(buf);
}

static void do_csi(uint8_t final) {
    uint32_t n;
    switch (final) {
    case 'A': n = param(0, 1); cur_row = (n > cur_row) ? 0 : cur_row - n; break;
    case 'B': n = param(0, 1); cur_row = (cur_row + n >= rows) ? rows - 1 : cur_row + n; break;
    case 'C': n = param(0, 1); cur_col = (cur_col + n >= cols) ? cols - 1 : cur_col + n; break;
    case 'D': n = param(0, 1); cur_col = (n > cur_col) ? 0 : cur_col - n; break;
    case 'G': n = param(0, 1); cur_col = (n - 1 >= cols) ? cols - 1 : n - 1; break;
    case 'H': case 'f':
        n = param(0, 1); cur_row = (n - 1 >= rows) ? rows - 1 : n - 1;
        n = param(1, 1); cur_col = (n - 1 >= cols) ? cols - 1 : n - 1;
        break;
    case 'J':                                    // erase in display
        n = param(0, 0);
        if (n == 0) {
            erase_cells(cur_row, cur_col, cols - 1);
            for (uint32_t r = cur_row + 1; r < rows; r++) clear_row(r);
        } else if (n == 1) {
            for (uint32_t r = 0; r < cur_row; r++) clear_row(r);
            erase_cells(cur_row, 0, cur_col);
        } else {
            for (uint32_t r = 0; r < rows; r++) clear_row(r);
        }
        break;
    case 'K':                                    // erase in line
        n = param(0, 0);
        if (n == 0)      erase_cells(cur_row, cur_col, cols - 1);
        else if (n == 1) erase_cells(cur_row, 0, cur_col);
        else             erase_cells(cur_row, 0, cols - 1);
        break;
    case 'm': set_graphics(); break;
    case 'n':
        // Device status report. Only the cursor position is asked for in
        // practice, and it is asked for because "move a long way right, then
        // tell me where you are" is how a program finds out how wide the
        // terminal is without being told.
        if (param(0, 0) == 6) report_position();
        break;
    case 's': saved_col = cur_col; saved_row = cur_row; break;
    case 'u':
        cur_col = saved_col < cols ? saved_col : cols - 1;
        cur_row = saved_row < rows ? saved_row : rows - 1;
        break;
    default: break;                              // dropped, not drawn
    }
    // Moving the cursor by hand ends any wrap that was being held back: the
    // column it was waiting on is not where we are any more.
    if (final != 'm' && final != 's') wrap_pending = false;
}

// One byte into the terminal. The cursor is lifted here rather than in
// draw_char so that an escape that moves or erases lifts it too -- otherwise it
// would be left behind, inverted, wherever it happened to be standing.
static void console_feed(uint8_t c) {
    if (!ready) return;
    cursor(false);

    switch (esc_state) {
    case ST_NORMAL:
        if (c == 0x1b) esc_state = ST_ESC;
        else           draw_char((char)c);
        return;
    case ST_ESC:
        if (c == '[') {
            esc_state = ST_CSI;
            nparams = 0;
            params[0] = 0;
        } else {
            esc_state = ST_NORMAL;               // not ours; both bytes go
        }
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (!nparams) nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (uint32_t)(c - '0');
        } else if (c == ';') {
            if (nparams < MAX_PARAMS) params[nparams++] = 0;
            else                      params[MAX_PARAMS - 1] = 0;
        } else if (c >= '@' && c <= '~') {
            do_csi(c);
            esc_state = ST_NORMAL;
        } else if (c < '0') {
            /* an intermediate byte, or a private marker like '?'; keep reading */
        } else {
            esc_state = ST_NORMAL;
        }
        return;
    }
}

// --- THE GRID -------------------------------------------------------------
// Changing font changes how many characters fit, so it cannot be done under the
// feet of whatever is drawing. The request is left here and the server picks it
// up when the ring is empty: everything written before the switch is drawn on
// the old grid first, and the switch then clears the screen. Doing it the other
// way round would drop the last few lines of output.
static volatile int32_t font_request = -1;

static void set_grid(const console_font_t *f) {
    font   = f;
    cell_w = f->w;
    cell_h = f->h;
    cols   = MYRTOS_H_ACTIVE / cell_w;
    rows   = MYRTOS_V_ACTIVE / cell_h;
    if (rows > ROWS_MAX) rows = ROWS_MAX;   // row_wrapped is sized for this
    x_margin = (MYRTOS_H_ACTIVE - cols * cell_w) / 2;

    // The origin is a multiple of the old cell height and need not be one of the
    // new. Start the ring over rather than leave a row straddling the join.
    myrtos_video_set_origin(0);
    uint8_t b = eff_bg();
    for (uint32_t i = 0; i < MYRTOS_H_ACTIVE * MYRTOS_V_ACTIVE; i++)
        myrtos_framebuf[i] = b;
    for (uint32_t r = 0; r < rows; r++) row_wrapped[r] = false;
    cur_col = cur_row = 0;
    wrap_pending = false;
    cursor_shown = false;        // the clear took it with the rest of the pixels
}

// Asked from a process, applied by the server. Reports the grid that font
// gives, whether or not it was asked to switch to it -- which is what lets a
// caller find out what it would get before deciding.
int32_t myrtos_console_select_font(int32_t index, myrtos_confont_t *out,
                                   bool look_only) {
    if (index >= (int32_t)NFONTS) return -1;
    uint32_t i = (index >= 0) ? (uint32_t)index : (uint32_t)(font - fonts);
    if (index >= 0 && !look_only) font_request = index;
    if (out) {
        out->index  = (uint8_t)i;
        out->cell_w = fonts[i].w;
        out->cell_h = fonts[i].h;
        out->count  = (uint8_t)NFONTS;
        out->cols   = (uint16_t)(MYRTOS_H_ACTIVE / fonts[i].w);
        out->rows   = (uint16_t)(MYRTOS_V_ACTIVE / fonts[i].h);
    }
    return 0;
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
    if (!server_up) { console_feed((uint8_t)c); cursor(true); return; }
    uint8_t b = (uint8_t)c;
    while (myrtos_console_put(&b, 1) == 0) { /* the kernel waits; it is rare */ }
}

static void console_thread(void) {
    server_up = true;
    for (;;) {
        while (ring_tail != ring_head) {
            console_feed(ring[ring_tail]);
            ring_tail = (ring_tail + 1) & RING_MASK;
        }
        cursor(true);
        if (font_request >= 0) {
            set_grid(&fonts[font_request]);
            font_request = -1;
            cursor(true);
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
    set_grid(&fonts[0]);
    ready = true;
    cursor(true);
}

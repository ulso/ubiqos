#include "tlsf.h"
#include "board.h"
#include "video.h"
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "chargen.h"
#include "hardware/regs/xip.h"
#include "hardware/xip_cache.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);

// DVI out of the HSTX peripheral, 640x480 at one byte per pixel.
//
// RP2350 does the hard part in hardware: HSTX has a TMDS encoder in its command
// expander, so the CPU never touches a pixel. DMA walks a list of commands and
// scanlines into the HSTX FIFO, and an interrupt per line hands it the next.
//
// Derived from pico-examples/hstx/dvi_out_hstx_encoder, with the pinout changed
// -- that example is wired for the Pico DVI sock, where each pair is P then N.
// The Fruit Jam is the other way round, and its clock is on the first pair
// rather than the second.

#if UBIQOS_VIDEO_800X600
// VESA's 800x600 at 60 Hz, exactly: 1056 x 628 at 40 MHz, both syncs
// positive. 40 MHz comes from the USB PLL, which kernel/main.c sets to 200
// MHz for HSTX alone once USB, the ADC and the peripherals have been moved
// to the system PLL -- see the note there.
//
// It was 48 MHz (240 / 5) first, with the blanking stretched to land on a
// standard line and frame rate. A standard clock is what any monitor's
// table expects, so it is what is used -- though see the note in
// docs/video-800x600.md: the only receiver tried so far, an MS2130 capture dongle,
// showed 800x600 once in many attempts at either clock.
#define H_FRONT_PORCH   40
#define H_SYNC_WIDTH    128
#define H_BACK_PORCH    88
#define V_FRONT_PORCH   1
#define V_SYNC_WIDTH    4
#define V_BACK_PORCH    23
#define SYNC_POSITIVE   1
#define PIXEL_HZ        40000000u
#define MODE_NAME       "800x600"
#elif UBIQOS_VIDEO_960X540
// 960x540 at 59.95 Hz: 1200 x 556 at the same 40 MHz as 800x600. Not a VESA
// or CEA mode, so there is no table to copy; the blanking is 240 pixels and
// 16 lines, both syncs positive as 1080p has them. It is here to see whether
// a 1080p monitor takes it and scales it by exactly two.
#define H_FRONT_PORCH   32
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    112
#define V_FRONT_PORCH   3
#define V_SYNC_WIDTH    5
#define V_BACK_PORCH    8
#define SYNC_POSITIVE   1
#define PIXEL_HZ        40000000u
#define MODE_NAME       "960x540"
#else
#define H_FRONT_PORCH   16
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    48
#define V_FRONT_PORCH   10
#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    52      // 33 by the standard; see the note on the table
#define SYNC_POSITIVE   0
#define PIXEL_HZ        24000000u
#define MODE_NAME       "640x480"
#endif
#define H_ACTIVE        UBIQOS_H_ACTIVE
#define V_ACTIVE        UBIQOS_V_ACTIVE
#define V_TEXT          UBIQOS_V_TEXT
#define V_BORDER        ((V_ACTIVE - V_TEXT) / 2)   // blank lines above the text
#define H_TOTAL (H_FRONT_PORCH + H_SYNC_WIDTH + H_BACK_PORCH + H_ACTIVE)
#define V_TOTAL (V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH + V_ACTIVE)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// The control symbols. Each is a ten-bit TMDS character chosen for its
// transition count, and a sync word carries one per lane: the syncs ride on
// lane 0 while the other two idle. These are not values to be reasoned out --
// the first version of this file had them invented, and produced no signal at
// all.
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Which level is "in sync". 640x480 has both syncs negative, so they idle
// high and pulse low; 800x600 is the other way round.
#if SYNC_POSITIVE
#define IDLE        SYNC_V0_H0
#define HSYNC       SYNC_V0_H1
#define VSYNC       SYNC_V1_H0
#define HSYNC_VSYNC SYNC_V1_H1
#else
#define IDLE        SYNC_V1_H1
#define HSYNC       SYNC_V1_H0
#define VSYNC       SYNC_V0_H1
#define HSYNC_VSYNC SYNC_V0_H0
#endif

#if !UBIQOS_VIDEO_HIRES
static uint32_t vblank_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, IDLE,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  HSYNC,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE), IDLE,
    HSTX_CMD_NOP
};
static uint32_t vblank_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, VSYNC,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  HSYNC_VSYNC,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE), VSYNC,
    HSTX_CMD_NOP
};
#endif
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, IDLE,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  HSYNC,
    HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,  IDLE,
    HSTX_CMD_TMDS       | H_ACTIVE
};

// The pieces a blank line is cut into when the frame table needs padding --
// see the note on the table. The head is the front porch and the sync; the
// rest of the line is chunks of BLANK_CHUNK pixels and one tail.
#if UBIQOS_VIDEO_HIRES
static uint32_t blank_head[2][4] = {
    { HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, IDLE,  HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH, HSYNC },
    { HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, VSYNC, HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH, HSYNC_VSYNC },
};
static uint32_t blank_chunk[2][2];
static uint32_t blank_tail[2][2][2];     // [vsync][one chunk more or not][words]
#endif

#if V_BORDER
// The lines above and below the text, all of them this one.
static uint8_t border_line[H_ACTIVE] __attribute__((aligned(4)));
#endif

// The framebuffer. Static rather than allocated: it is one permanent thing the
// kernel owns, and it should not be able to fail at an awkward moment. In SRAM
// for now, where its timing is never the question -- PSRAM once the picture is
// steady.
// The framebuffer is in SRAM, and it was in PSRAM for exactly one experiment.
//
// A static image survives PSRAM fine -- the test card wrote 300 kB sequentially
// and disturbed nothing. A text console does not. A glyph is eight bytes on each
// of sixteen scanlines 640 bytes apart, so every character touches sixteen cache
// lines and each one must be fetched from PSRAM before it can be written back.
// That holds the QMI far longer per byte than a sequential write does, the
// display's own reads stall behind it, the HSTX FIFO underruns, and the monitor
// drops sync and spends a couple of seconds finding it again. It looked like
// flicker; it was the picture dying and being reacquired, once per line printed.
//
// So the 300 kB stays here. PSRAM is for bulk that is not streamed 57 times a
// second: module data, file buffers, whatever the shell wants.
#if UBIQOS_VIDEO_RING

// Character cells instead, and a handful of scanlines in flight.
//
// LINE_BUFS divides V_TEXT, which is what makes `line % LINE_BUFS` mean the
// same thing in the frame table as it does in the renderer: 480 is ten bands of
// forty-eight, and 576 (800x600's text) is twelve. It buys 48 * 32 us = 1.5 ms of slack between the generator and
// the beam, against a pump that runs every 500 us from a handler the kernel
// cannot mask. The first version of this display took an interrupt per scanline
// and died when pre-emption arrived; the slack is the answer to that.
#define LINE_BUFS 48
static uint8_t linebuf[LINE_BUFS][UBIQOS_H_ACTIVE] __attribute__((aligned(4)));

#else

static uint8_t framebuf_store[UBIQOS_H_ACTIVE * UBIQOS_V_ACTIVE]
    __attribute__((aligned(4)));
uint8_t *ubiqos_framebuf = framebuf_store;

#endif

#if UBIQOS_VIDEO_PSRAM
// The picture lives in PSRAM and is copied a run of lines at a time into the
// scanline ring, by DMA out of the XIP streaming FIFO: the stream reads the
// chip sequentially and fast (about 45 MB/s measured, where 640x480 needs 19)
// and the ring between it and HSTX absorbs whatever the QMI is busy with.
// That ring is the difference from the one time a framebuffer lived in PSRAM
// before: HSTX read it directly then, and every stall was a lost line.
//
// The stream reads the chip, not the cache, so everything that draws must
// write past the cache too: ubiqos_framebuf is the no-cache alias of the
// block. A write through the cached address would sit in the cache where the
// display cannot see it.
uint8_t *ubiqos_framebuf;
static uint32_t fb_stream;      // the block's cached address, what the stream takes
static int ch_copy = -1;
static uint32_t copy_from;      // first line of the copy in flight
static uint32_t copies, copy_waits;
#define FB_NOCACHE_OFFSET (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE)
#define XIP_STREAM_ADDR_REG (*(volatile uint32_t *)(XIP_CTRL_BASE + XIP_STREAM_ADDR_OFFSET))
#define XIP_STREAM_CTR_REG  (*(volatile uint32_t *)(XIP_CTRL_BASE + XIP_STREAM_CTR_OFFSET))
#define XIP_AUX_STREAM_REG  ((volatile uint32_t *)XIP_AUX_BASE)
#endif

// --- HOW THE FRAME IS PLAYED ----------------------------------------------
// Two channels, and no interrupt at all.
//
// The data channel writes into the HSTX FIFO. When it finishes it chains to the
// control channel, which writes the next transfer's length and source into the
// data channel's alias-3 registers -- length first, address second, and the
// address write is the one that triggers. Then the data channel runs again.
//
// The control channel reads those pairs from a table holding an entire frame,
// and a read ring wraps it back to the start, so the pair runs forever without
// anyone telling it to.
//
// The first version used one interrupt per scanline instead, and worked --
// until pre-emption started. From then on every system call held interrupts off
// for longer than a scanline lasts, the chain starved, and the picture died
// about five seconds into every boot. A display does not need to react to
// anything; it needs to be fed, and feeding can be arranged in advance.
//
// The ring is why the vertical back porch is 52 lines rather than the standard
// 33. A ring must be a power of two, and a frame is two transfers per active
// line plus one per blank line: 480*2 + 64 = 1024 exactly. It costs refresh
// rate -- 57 Hz rather than 60 -- and buys freedom from the kernel entirely.
//
// 800x600 could not do the same: 600*2 + 25 is 1225, and stretching the
// blanking to 2048 would halve the refresh. So the table is padded instead,
// by cutting each blank line's long stretch of idle into some thirty pieces
// of about 33 pixels, each its own transfer.
//
// NOT with NOPs, which was the first version and gave no picture at all. A
// NOP takes no time on the wire, so the expander eats them as fast as they
// come, and with a FIFO eight words deep the DMA has to deliver every one of
// 823 of them while the output waits: microseconds of nothing once a frame,
// and a monitor that never locks. A piece of idle that LASTS 0.7 us gives the
// DMA far more time than it needs to fetch the next.

// The DMA has one ring, not two: RING_SEL chooses whether it applies to the
// read side or the write side. Trying to have both -- a read ring to wrap the
// table and a write ring to bounce between two registers -- silently kept only
// the second, and the read pointer walked off the end of the table after one
// frame.
//
// So the write side is split in two. One channel writes the length, one writes
// the source address, each to a fixed register with no increment and no ring,
// and each walks its own table with a read ring. Three channels, two tables,
// and still not one interrupt.

#define BLANK_LINES   (V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH)
#define FRAME_USED    (V_ACTIVE * 2 + BLANK_LINES)
#if FRAME_USED <= 1024
#define FRAME_ENTRIES 1024
#define FRAME_RING    12            // log2 of the table's size in bytes
#else
#define FRAME_ENTRIES 2048
#define FRAME_RING    13
#endif
#define FILLERS       (FRAME_ENTRIES - FRAME_USED)
#define FIRST_ACTIVE  (FILLERS + BLANK_LINES)   // the table entry of line 0
_Static_assert(FRAME_USED <= FRAME_ENTRIES, "the frame does not fit its table");
#if (FILLERS != 0) != (UBIQOS_VIDEO_HIRES + 0 != 0)
#error "only 800x600 and 960x540 pad their frame tables; the pieces are defined for them alone"
#endif

static uint32_t     frame_counts[FRAME_ENTRIES]
    __attribute__((aligned(FRAME_ENTRIES * 4)));
static const void  *frame_addrs[FRAME_ENTRIES]
    __attribute__((aligned(FRAME_ENTRIES * 4)));

static int ch_data = -1, ch_count = -1, ch_addr = -1;

#if UBIQOS_VIDEO_PSRAM
uint32_t ubiqos_video_origin;

// The pump reads it for every copy, so moving it is all scrolling takes.
void ubiqos_video_set_origin(uint32_t line) {
    ubiqos_video_origin = line % V_ACTIVE;
}
#elif !UBIQOS_VIDEO_CHARGEN
uint32_t ubiqos_video_origin;

// Point every active display row at a framebuffer line, offset by the origin.
// The control channel may be reading the table while this runs; it reads about
// one entry per 32 us and the rewrite takes some sixteen, so at worst a single
// scanline shows the wrong content for a single frame. That is the same tear any
// unsynchronised scroll has, and it is not visible.
void ubiqos_video_set_origin(uint32_t line) {
    ubiqos_video_origin = line % V_ACTIVE;
    for (uint r = 0; r < V_ACTIVE; r++) {
        uint fb = (ubiqos_video_origin + r) % V_ACTIVE;
        frame_addrs[FIRST_ACTIVE + r * 2 + 1] = &ubiqos_framebuf[fb * H_ACTIVE];
    }
}
#endif

#if FILLERS
// Pieces per blank line: FILLERS more entries spread over BLANK_LINES lines,
// the first FILLERS % BLANK_LINES of them taking one more.
#define CHUNKS_LO     (FILLERS / BLANK_LINES)
#define CHUNKS_EXTRA  (FILLERS % BLANK_LINES)
#define BLANK_REST    (H_BACK_PORCH + H_ACTIVE)
#define BLANK_CHUNK   (BLANK_REST / (CHUNKS_LO + 1))
_Static_assert(CHUNKS_LO >= 2, "a blank line is cut into at least two pieces");

static uint add_blank_line(uint n, uint line, bool vsync) {
    const uint v = vsync ? 1u : 0u;
    const uint more = line < CHUNKS_EXTRA ? 1u : 0u;
    const uint chunks = CHUNKS_LO + more;
    frame_counts[n] = 4;
    frame_addrs[n++] = blank_head[v];
    for (uint i = 0; i + 1 < chunks; i++) {
        frame_counts[n] = 2;
        frame_addrs[n++] = blank_chunk[v];
    }
    frame_counts[n] = 2;
    frame_addrs[n++] = blank_tail[v][more];
    return n;
}
#endif

static void build_frame_list(void) {
    uint n = 0;
#if FILLERS
    for (uint v = 0; v < 2; v++) {
        const uint32_t level = v ? VSYNC : IDLE;
        blank_chunk[v][0] = HSTX_CMD_RAW_REPEAT | BLANK_CHUNK;
        blank_chunk[v][1] = level;
        for (uint more = 0; more < 2; more++) {
            const uint chunks = CHUNKS_LO + more;
            blank_tail[v][more][0] = HSTX_CMD_RAW_REPEAT | (BLANK_REST - BLANK_CHUNK * (chunks - 1));
            blank_tail[v][more][1] = level;
        }
    }
    for (uint line = 0; line < V_TOTAL; line++) {
        if (line < BLANK_LINES) {
            const bool vs = line >= V_FRONT_PORCH && line < V_FRONT_PORCH + V_SYNC_WIDTH;
            n = add_blank_line(n, line, vs);
            continue;
        }
        {
#else
    for (uint line = 0; line < V_TOTAL; line++) {
        if (line < V_FRONT_PORCH || line >= V_FRONT_PORCH + V_SYNC_WIDTH) {
            if (line < BLANK_LINES) {
                frame_counts[n] = count_of(vblank_vsync_off);
                frame_addrs[n++] = vblank_vsync_off;
            }
        } else {
            frame_counts[n] = count_of(vblank_vsync_on);
            frame_addrs[n++] = vblank_vsync_on;
        }
        if (line >= BLANK_LINES) {
#endif
            frame_counts[n] = count_of(vactive_line);
            frame_addrs[n++] = vactive_line;
            frame_counts[n] = H_ACTIVE / sizeof(uint32_t);
#if UBIQOS_VIDEO_RING
            const uint a = line - BLANK_LINES;
#if V_BORDER
            if (a < V_BORDER || a >= V_BORDER + V_TEXT) {
                frame_addrs[n++] = border_line;
                continue;
            }
#endif
            frame_addrs[n++] = linebuf[(a - V_BORDER) % LINE_BUFS];
#else
            frame_addrs[n++] = &ubiqos_framebuf[(line - BLANK_LINES) * H_ACTIVE];
#endif
        }
    }
}

#if UBIQOS_VIDEO_RING

// --- KEEPING AHEAD OF THE BEAM --------------------------------------------
//
// The display still runs from the three chained channels and still asks nobody
// for anything. What changed is that the addresses it plays are forty-eight
// buffers rather than four hundred and eighty framebuffer lines, so somebody
// has to write each one again before it comes round.
//
// Where the beam is needs no interrupt either: the address channel's read
// pointer walks frame_addrs, so subtracting the table's own address says which
// entry is playing. Two entries per active line, after BLANK_LINES of them.
//
// THE PRIORITY IS BELOW THE KERNEL, AND THAT IS DELIBERATE.
//
// It was 0x40, above UBIQOS_CRITICAL_BASEPRI, on the theory that the display
// must never be delayed. That was wrong, and `vec demo` proved it: PIO-USB
// drives its SOF from a repeating timer at the SDK's default 0x80
// (alarm_pool_add_repeating_timer_us, -1000 us, in pio_usb_host.c), so a pump
// at 0x40 preempts the one interrupt on this board that genuinely cannot be
// late. A USB frame is due every millisecond and the bus falls apart without
// it -- the keyboard died while the picture carried on, which is exactly what
// a display monopolising the processor looks like.
//
// The ordering follows from who has slack. The display has 48 buffers, which
// is 1.5 milliseconds of it; a USB frame has none. So the pump sits at 0xC0,
// below both PIO-USB and the kernel's own threshold, and the ring absorbs
// whatever delay that costs. The underrun counter is what says whether it did.
//
// This also explains the intermittent CDC drops before any of this: the pump
// was already taking about a third of the processor at 0x40 and landing on the
// SOF timer from time to time.
//
// It still calls nothing -- two DMA registers, cells, font bytes, pixels -- and
// that stays true whatever its priority, because it is the honest description
// of what a scanline builder needs.

// ON RISC-V THIS IS EXPECTED TO TEAR, and the counter is there to say so.
// kernel/critical.h only takes the BASEPRI path on Arm; Hazard3 gets the hammer
// and masks everything, so a critical section stops the pump exactly the way it
// stopped the per-scanline interrupt the first time. Prioritised traps on
// RISC-V were tried once and reverted, and a trap stack is the prerequisite --
// so on RISC-V the honest answer today is UBIQOS_VIDEO=framebuffer.

// 700, not 500, and the reason is the glyph row. A pump renders whatever the
// beam has advanced since the last one, so at 500 microseconds that is about
// fifteen scanlines -- one short of the sixteen a glyph row needs, and the band
// path was taken 3 times against 113337 single lines. At 700 it is about
// twenty-two, so most lines go through the band.
//
// The ring is 48 buffers, 1.5 milliseconds, so this still leaves about 0.8 of
// slack. The underrun counter is what says whether that was enough.
//
// 800x600 draws 46875 lines a second against 480's 28600, so 700 there is 33
// lines a pump against a ring of 46 -- 280 us of slack, and it underran about
// 200 lines a second. 400 is a glyph row and a bit per pump, which keeps the
// band path and leaves the ring about 580 us of slack.
//
// A picture in PSRAM is copied, not drawn, and a copy has to be one run in
// both the ring and the picture: it stops at the end of the 48 buffers, and
// at line 480 of a picture the console has scrolled. The rest waits for the
// next pump. At 700 us that halved the slack every time it happened and the
// beam caught up about 28 lines a second, on the screen row where the
// picture wraps. A pump here costs four microseconds, so it runs every 250.
#if UBIQOS_VIDEO_PSRAM
#define PUMP_US 250
#elif UBIQOS_VIDEO_800X600
#define PUMP_US 400
#elif UBIQOS_VIDEO_960X540
#define PUMP_US 500     // 30 us lines: 16.7 a pump, a glyph row, 940 us of slack
#else
#define PUMP_US 700
#endif

uint32_t ubiqos_video_underruns, ubiqos_video_pumps, ubiqos_video_lines;
static uint32_t first_underrun_pump;

// What the display actually costs, in microseconds, because guessing it has now
// been wrong twice. total against elapsed gives the share of the processor this
// interrupt is taking, and max is the one number that says whether a single
// call can sit on top of something that cannot wait.
uint32_t ubiqos_video_us_total, ubiqos_video_us_max;
#if !UBIQOS_VIDEO_PSRAM
static uint32_t bands_done, singles_done;
#endif

static int      pump_alarm = -1;
static uint32_t beam_epoch;     // active lines completed in whole frames
static uint32_t beam_last;      // the line seen last time, to catch the wrap
static uint32_t rendered_to;    // the next line to build, on the same scale

// How far beam_epoch and rendered_to count before both are taken back by the
// same amount: a whole number of frames, about five hours of them.
//
// They used to count for ever, and 32 bits of active lines is 43 hours of
// them. At the wrap the target, now + 46, sat just under 2^32 while
// rendered_to went past it to 0 inside the loop -- and the loop then had four
// billion lines to build. On 7 Oct 2026 a Fruit Jam 45 hours up was found
// with beam_epoch at FFFFFF00, one pump that had never ended, the tick
// starved and the screen half-drawn. Taking both back together leaves their
// difference alone, and a multiple of V_TEXT is a multiple of LINE_BUFS and
// of the glyph height too, so every line keeps its buffer and its glyph row.
// UBIQOS_VIDEO_REBASE_FRAMES is there to be made small in a test build, so
// that the rebase happens many times a second rather than once in five hours.
#ifndef UBIQOS_VIDEO_REBASE_FRAMES
#define UBIQOS_VIDEO_REBASE_FRAMES 1048576u
#endif
#define REBASE_LINES (V_TEXT * UBIQOS_VIDEO_REBASE_FRAMES)
_Static_assert(REBASE_LINES % LINE_BUFS == 0, "a rebase must keep each line's buffer");
_Static_assert(REBASE_LINES % UBIQOS_CELL_H == 0, "a rebase must keep the bands aligned");

uint32_t ubiqos_video_buffers(void) { return LINE_BUFS; }

// Everything the one measurement needs, in the order vidstat prints it. beam
// and rendered say whether the display is moving at all and whether anything is
// being built for it; alarm and irq say the pump was wired to something.

static inline uint32_t beam_line(void)
{
    uint32_t idx = (uint32_t)(((uintptr_t)dma_hw->ch[ch_addr].read_addr
                               - (uintptr_t)frame_addrs) / sizeof(frame_addrs[0]));
    if (idx >= FRAME_ENTRIES || idx < FIRST_ACTIVE)
        return 0;                       // still in the vertical blanking
    idx = (idx - FIRST_ACTIVE) / 2;
    // The border above the text counts as its first line, the one below as
    // its last: what the beam is showing there was never built.
    if (idx < V_BORDER) return 0;
    idx -= V_BORDER;
    return idx < V_TEXT ? idx : V_TEXT - 1;
}

// What the display is actually playing for one line. The buffer is live: this
// is the bytes the DMA hands to HSTX, not a re-rendering of them.
void ubiqos_video_peek_line(uint32_t line, uint8_t *out, uint32_t n)
{
    const uint8_t *p = linebuf[line % LINE_BUFS];
    for (uint32_t i = 0; i < n; i++)
        out[i] = p[i];
}

void ubiqos_video_stats_fill(uint32_t *sixteen)
{
    sixteen[0] = ubiqos_video_underruns;
    sixteen[1] = ubiqos_video_pumps;
    sixteen[2] = ubiqos_video_lines;
    sixteen[3] = LINE_BUFS;
    sixteen[4] = beam_line();
    sixteen[5] = rendered_to;
#if UBIQOS_VIDEO_CHARGEN
    sixteen[6] = ubiqos_chargen_view_back();
    sixteen[7] = ubiqos_chargen_history();
    sixteen[8] = ubiqos_chargen_deep();
#endif
    sixteen[9]  = first_underrun_pump;
    sixteen[10] = ubiqos_video_us_total;
    sixteen[11] = ubiqos_video_us_max;
    sixteen[12] = 0;
#if UBIQOS_VIDEO_PSRAM
    sixteen[13] = copies;           // vidstat calls them glyph rows
    sixteen[14] = copy_waits;       // and these single lines
#else
    sixteen[13] = bands_done;
    sixteen[14] = singles_done;
#endif
    sixteen[15] = PUMP_US;   // reported, not assumed: it has been changed once
}

static void video_pump(void)
{
    uint32_t t0 = timer_hw->timerawl;
    uint32_t cur = beam_line();
    if (cur < beam_last)
        beam_epoch += V_TEXT;           // a frame went by
    beam_last = cur;

    uint32_t now = beam_epoch + cur;

    // Behind the beam: the display has already shown a line this never wrote.
    // Say so and start again from where it is, rather than racing to catch up
    // with work whose result is already on the screen.
    if (rendered_to < now) {
        if (!ubiqos_video_underruns)
            first_underrun_pump = ubiqos_video_pumps;   // when, not just how many
        ubiqos_video_underruns += now - rendered_to;
        rendered_to = now;
    }

    // rendered_to is not behind now here, so both come down together.
    if (beam_epoch >= REBASE_LINES) {
        beam_epoch  -= REBASE_LINES;
        rendered_to -= REBASE_LINES;
        now         -= REBASE_LINES;
    }

    // One buffer of margin at each end: the one being played, and the one the
    // DMA may have already latched the address of.
    //
    // And never more than a ring's worth in one pump, whatever the counters
    // say. A pump that cannot end takes the machine with it -- this one runs
    // above everything the kernel can mask -- where a pump that stops short
    // costs, at worst, a frame of wrong lines.
    uint32_t target = now + LINE_BUFS - 2;
#if UBIQOS_VIDEO_PSRAM
    // One copy at a time: the stream is a single engine. If the last one is
    // still going, say so -- and if the beam has reached the lines it is
    // still bringing, that is the underrun this design can have.
    if (ch_copy >= 0 && ubiqos_framebuf) {
        if (dma_channel_is_busy((uint)ch_copy)) {
            copy_waits++;
            if (now > copy_from)
                ubiqos_video_underruns += now - copy_from;
        } else if (rendered_to < target) {
            const uint32_t y = rendered_to % V_TEXT;
            const uint32_t b = rendered_to % LINE_BUFS;
            const uint32_t src = (ubiqos_video_origin + y) % V_ACTIVE;
            uint32_t n = target - rendered_to;
            if (n > LINE_BUFS - b) n = LINE_BUFS - b;   // the ring wraps
            if (n > V_ACTIVE - src) n = V_ACTIVE - src;  // the picture wraps
            const uint32_t words = n * H_ACTIVE / 4u;

            // The FIFO is empty: the last stream was read to its end.
            XIP_STREAM_CTR_REG = 0;
            XIP_STREAM_ADDR_REG = fb_stream + src * H_ACTIVE;
            XIP_STREAM_CTR_REG = words;
            dma_channel_set_write_addr((uint)ch_copy, linebuf[b], false);
            dma_channel_set_trans_count((uint)ch_copy, words, true);
            copy_from = rendered_to;
            rendered_to += n;
            ubiqos_video_lines += n;
            copies++;
        }
    }
#else
    for (uint32_t built = 0; rendered_to < target && built < LINE_BUFS; ) {
        uint32_t y = rendered_to % V_TEXT;
        uint8_t *buf = linebuf[rendered_to % LINE_BUFS];

        // Whole glyph rows, and STOPPING at the boundary rather than filling
        // the last few lines singly. Filling them lost the alignment, so the
        // next pump had to spend ten lines getting it back and the band ran on
        // every other pump: 28 per cent of lines instead of nearly all.
        //
        // Stopping short is free. target is how far ahead this would LIKE to
        // be, not a debt -- the next pump carries on, and the ring is what
        // absorbs the unevenness. 48 buffers is three sixteens and 480 lines is
        // thirty of them, so a 16-aligned line is a 16-aligned buffer and the
        // block never wraps.
        if ((rendered_to % UBIQOS_CELL_H) == 0) {
            if (rendered_to + UBIQOS_CELL_H > target)
                break;
            ubiqos_chargen_band(y, buf);
            rendered_to += UBIQOS_CELL_H;
            ubiqos_video_lines += UBIQOS_CELL_H;
            bands_done++;
            built += UBIQOS_CELL_H;
            continue;
        }

        ubiqos_chargen_line(y, buf);
        rendered_to++;
        singles_done++;
        ubiqos_video_lines++;
        built++;
    }
#endif
    ubiqos_video_pumps++;

    uint32_t dt = timer_hw->timerawl - t0;
    ubiqos_video_us_total += dt;
    if (dt > ubiqos_video_us_max) ubiqos_video_us_max = dt;
}

static void pump_isr(void)
{
    timer_hw->intr = 1u << pump_alarm;                     // acknowledge
    timer_hw->alarm[pump_alarm] = timer_hw->timerawl + PUMP_US;
    video_pump();
}

static void pump_start(void)
{
    // Fill every buffer once so the first frame is not a screenful of whatever
    // SRAM held at reset. The cells are blank at this point, so which line
    // index each buffer was built for does not matter.
#if UBIQOS_VIDEO_PSRAM
    for (uint32_t i = 0; i < LINE_BUFS; i++)
        for (uint32_t x = 0; x < H_ACTIVE; x++)
            linebuf[i][x] = 0;
#else
    for (uint32_t i = 0; i < LINE_BUFS; i++)
        ubiqos_chargen_line(i, linebuf[i]);
#endif

    // THEN start the bookkeeping from where the beam actually is. The DMA chain
    // has been running since dma_channel_start, so claiming that forty-eight
    // lines are already accounted for makes the first pump compare that against
    // a beam somewhere else entirely and book the difference as lateness. It
    // read as 77 underruns at pump zero, and it had been reading zero only
    // because the timing happened to fall the other way -- which is worse,
    // because the whole design is judged by this counter.
    beam_epoch = 0;
    beam_last = rendered_to = beam_line();
    video_pump();                    // and fill the real window before arming

    pump_alarm = (int)hardware_alarm_claim_unused(true);
    uint irq = hardware_alarm_get_irq_num(pump_alarm);
    irq_set_exclusive_handler(irq, pump_isr);
    irq_set_priority(irq, 0xc0);
    irq_set_enabled(irq, true);
    hw_set_bits(&timer_hw->inte, 1u << pump_alarm);
    timer_hw->alarm[pump_alarm] = timer_hw->timerawl + PUMP_US;
}

#endif

void ubiqos_video_init(void) {
    // set_sys_clock_khz does not touch clk_hstx: it kept its own source, the
    // system PLL at 150 MHz, while the processor went down to 125. The pixel
    // clock is clk_hstx/5, so the display was being driven at 30 MHz instead of
    // 25 and no monitor recognised what came out. Point it at clk_sys and say
    // so, rather than trusting a default that was never chosen.
    //
    // 120 MHz whatever clk_sys is: at 240 the clock divides by two on the way
    // in (clk_hstx has an integer divider), and the picture is the same
    // 640x480 at 57 Hz either way. A faster mode is a separate decision.
#if UBIQOS_VIDEO_HIRES
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    PIXEL_HZ * 5u, PIXEL_HZ * 5u);   // the USB PLL, set to this in main.c
#else
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), PIXEL_HZ * 5u);
#endif

    // RGB332 out of one byte: two bits of blue, three of green, three of red,
    // each rotated into place for its lane.
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // A pixel word carries four pixels; a control symbol is one whole word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Ten bits per TMDS character, two shifted per cycle, so five cycles each --
    // which fixes the pixel clock at clk_hstx/5. The system runs at 120 MHz for
    // PIO-USB's sake rather than at the 125 that would give exactly 25 MHz here,
    // so the picture is 640x480 at about 57 Hz. See the note in main.c.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Fruit Jam: GP12 CKN, GP13 CKP, then D0, D1, D2 as N,P pairs. HSTX output
    // bit N is GPIO 12+N, and the negative half of each pair is the inverted
    // one -- the opposite order to the example this came from.
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;

    for (uint lane = 0; lane < 3; ++lane) {
        uint bit = 2 + lane * 2;
        uint32_t sel = (lane * 10)     << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit]     = sel | HSTX_CTRL_BIT0_INV_BITS;   // N
        hstx_ctrl_hw->bit[bit + 1] = sel;                             // P
    }
    for (int i = UBIQOS_VIDEO_PIN_FIRST; i <= UBIQOS_VIDEO_PIN_LAST; ++i) {
        gpio_set_function(i, 0);
#if UBIQOS_VIDEO_HIRES
        // 400 Mbit/s a pair rather than 240. Fast edges and 12 mA were set
        // while bringing this up; whether the defaults (4 mA, slow) would do
        // is not known -- the one receiver tried could not tell either way.
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
#endif
    }

    build_frame_list();

    ch_data  = dma_claim_unused_channel(true);
    ch_count = dma_claim_unused_channel(true);
    ch_addr  = dma_claim_unused_channel(true);

    // Data: memory to the FIFO, paced by HSTX. When it finishes it hands back
    // to the first control channel rather than to an interrupt.
    dma_channel_config d = dma_channel_get_default_config(ch_data);
    channel_config_set_dreq(&d, DREQ_HSTX);
    channel_config_set_chain_to(&d, ch_count);
    dma_channel_configure(ch_data, &d, &hstx_fifo_hw->fifo,
                          frame_addrs[0], frame_counts[0], false);

    // Length, then address. The address register is the trigger, so it must be
    // written second -- which is why these are two channels in this order and
    // not one channel writing a pair.
    dma_channel_config c = dma_channel_get_default_config(ch_count);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, FRAME_RING);  // read wraps once a frame
    channel_config_set_chain_to(&c, ch_addr);
    dma_channel_configure(ch_count, &c,
                          &dma_hw->ch[ch_data].al3_transfer_count,
                          frame_counts, 1, false);

    dma_channel_config a = dma_channel_get_default_config(ch_addr);
    channel_config_set_write_increment(&a, false);
    channel_config_set_ring(&a, false, FRAME_RING);
    channel_config_set_chain_to(&a, ch_addr);        // chain to self means none
    dma_channel_configure(ch_addr, &a,
                          &dma_hw->ch[ch_data].al3_read_addr_trig,
                          frame_addrs, 1, false);

    // The display cannot wait; anything else can.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(ch_count);

#if UBIQOS_VIDEO_CHARGEN
    ubiqos_chargen_init(0xf0);      // white on black, before anything prints
    pump_start();
#endif
#if UBIQOS_VIDEO_PSRAM
    {
        // Straight from the pool, as chargen takes its history: this is kernel
        // memory for the life of the machine, and ubiqos_mem_alloc_bulk
        // refuses in kernel context.
        extern tlsf_pool_t ubiqos_bulk_pool;
        const uint32_t size = H_ACTIVE * V_ACTIVE;
        void *p = ubiqos_bulk_pool ? ubiqos_tlsf_malloc(ubiqos_bulk_pool, size) : 0;
        if (p && ubiqos_tlsf_owns(ubiqos_bulk_pool, p) && ((uintptr_t)p & 7u) == 0) {
            fb_stream = (uint32_t)(uintptr_t)p;
            // Nothing may stay in the cache for it: a dirty line written back
            // later would land on top of what the console drew past the cache.
            xip_cache_invalidate_range(fb_stream - XIP_BASE, (size + 7u) & ~7u);
            ubiqos_framebuf = (uint8_t *)(uintptr_t)(fb_stream + FB_NOCACHE_OFFSET);
            for (uint32_t i = 0; i < size / 4u; i++)
                ((volatile uint32_t *)ubiqos_framebuf)[i] = 0;

            ch_copy = dma_claim_unused_channel(true);
            dma_channel_config cc = dma_channel_get_default_config((uint)ch_copy);
            channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
            channel_config_set_read_increment(&cc, false);
            channel_config_set_write_increment(&cc, true);
            channel_config_set_dreq(&cc, DREQ_XIP_STREAM);
            dma_channel_configure((uint)ch_copy, &cc, linebuf[0],
                                  (const void *)XIP_AUX_STREAM_REG, 0, false);
            ubiqos_print("Video: picture in PSRAM at 0x");
            ubiqos_print_hex(fb_stream);
            ubiqos_print(", copied by DMA ");
            ubiqos_print_u32((uint32_t)ch_copy);
            ubiqos_print("\n");
        } else {
            ubiqos_print("Video: no PSRAM for the picture; the screen stays black\n");
        }
        pump_start();
    }
#endif

    ubiqos_print("Video: clk_sys ");
    ubiqos_print_u32(clock_get_hz(clk_sys) / 1000000);
    ubiqos_print(" MHz, clk_hstx ");
    ubiqos_print_u32(clock_get_hz(clk_hstx) / 1000000);
    ubiqos_print(" MHz\n");
    ubiqos_print("Video: csr 0x");
    ubiqos_print_hex(hstx_ctrl_hw->csr);
    ubiqos_print(" (want 0x50050203), fifo stat 0x");
    ubiqos_print_hex(hstx_fifo_hw->stat);
    ubiqos_print(", " MODE_NAME ", DMA ");
    ubiqos_print_u32((uint32_t)ch_data);
    ubiqos_print(", ");
    ubiqos_print_u32((uint32_t)ch_count);
    ubiqos_print(", ");
    ubiqos_print_u32((uint32_t)ch_addr);
    ubiqos_print("\n");
}

#if !UBIQOS_VIDEO_CHARGEN
// Something recognisable, so the first picture says whether the pinout and the
// timing are right rather than merely that something came out.
void ubiqos_video_testcard(void) {
    // Below the bars, all 256 colours in a 16x16 grid, ordered by byte value.
    // The previous ramp built its colour with ((x >> 5) << 5), which wraps at
    // 256 pixels, and ((y >> 5) << 2), which grows past the three bits of the
    // green field and bleeds into red. It repeated two and a half times across
    // the screen and looked like a fault in the video path when it was only bad
    // arithmetic. Every cell here is one distinct value, so a wrong bit shows up
    // as a cell out of order rather than as a pattern that is hard to read.
    const uint grid_top = 64;
    const uint cell_w = H_ACTIVE / 16;
    const uint cell_h = (V_ACTIVE - grid_top) / 16;

    for (uint y = 0; y < V_ACTIVE; y++) {
        for (uint x = 0; x < H_ACTIVE; x++) {
            uint8_t c;
            if (y < grid_top) {
                static const uint8_t bars[8] = {0xff,0xfc,0x1f,0x1c,0xe3,0xe0,0x03,0x00};
                c = bars[(x * 8) / H_ACTIVE];
            } else if (x < 8 || x >= H_ACTIVE - 8 || y >= V_ACTIVE - 8) {
                c = 0xff;                       // a border, to show the edges
            } else {
                uint col = x / cell_w;
                uint row = (y - grid_top) / cell_h;
                if (col > 15) col = 15;
                if (row > 15) row = 15;
                c = (uint8_t)(row * 16 + col);
            }
            ubiqos_framebuf[y * H_ACTIVE + x] = c;
        }
    }
}
#endif

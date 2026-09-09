#include "tlsf.h"
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

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

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

#define H_FRONT_PORCH   16
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    48
#define H_ACTIVE        MYRTOS_H_ACTIVE
#define V_FRONT_PORCH   10
#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    52      // 33 by the standard; see the note on the table
#define V_ACTIVE        MYRTOS_V_ACTIVE
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

static uint32_t vblank_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE), SYNC_V1_H1,
    HSTX_CMD_NOP
};
static uint32_t vblank_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE), SYNC_V0_H1,
    HSTX_CMD_NOP
};
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,  SYNC_V1_H1,
    HSTX_CMD_TMDS       | H_ACTIVE
};

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
#if MYRTOS_VIDEO_CHARGEN

// Character cells instead, and a handful of scanlines in flight.
//
// LINE_BUFS divides V_ACTIVE, which is what makes `line % LINE_BUFS` mean the
// same thing in the frame table as it does in the renderer: 480 is ten bands of
// forty-eight. It buys 48 * 32 us = 1.5 ms of slack between the generator and
// the beam, against a pump that runs every 500 us from a handler the kernel
// cannot mask. The first version of this display took an interrupt per scanline
// and died when pre-emption arrived; the slack is the answer to that.
#define LINE_BUFS 48
static uint8_t linebuf[LINE_BUFS][MYRTOS_H_ACTIVE] __attribute__((aligned(4)));

#else

static uint8_t framebuf_store[MYRTOS_H_ACTIVE * MYRTOS_V_ACTIVE]
    __attribute__((aligned(4)));
uint8_t *myrtos_framebuf = framebuf_store;

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

#define BLANK_LINES   (V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH)
#define FRAME_ENTRIES (V_ACTIVE * 2 + BLANK_LINES)

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
#define FRAME_ENTRIES (V_ACTIVE * 2 + BLANK_LINES)

static uint32_t     frame_counts[FRAME_ENTRIES]
    __attribute__((aligned(FRAME_ENTRIES * 4)));
static const void  *frame_addrs[FRAME_ENTRIES]
    __attribute__((aligned(FRAME_ENTRIES * 4)));

static int ch_data = -1, ch_count = -1, ch_addr = -1;

#if !MYRTOS_VIDEO_CHARGEN
uint32_t myrtos_video_origin;

// Point every active display row at a framebuffer line, offset by the origin.
// The control channel may be reading the table while this runs; it reads about
// one entry per 32 us and the rewrite takes some sixteen, so at worst a single
// scanline shows the wrong content for a single frame. That is the same tear any
// unsynchronised scroll has, and it is not visible.
void myrtos_video_set_origin(uint32_t line) {
    myrtos_video_origin = line % V_ACTIVE;
    for (uint r = 0; r < V_ACTIVE; r++) {
        uint fb = (myrtos_video_origin + r) % V_ACTIVE;
        frame_addrs[BLANK_LINES + r * 2 + 1] = &myrtos_framebuf[fb * H_ACTIVE];
    }
}
#endif

static void build_frame_list(void) {
    uint n = 0;
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
            frame_counts[n] = count_of(vactive_line);
            frame_addrs[n++] = vactive_line;
            frame_counts[n] = H_ACTIVE / sizeof(uint32_t);
#if MYRTOS_VIDEO_CHARGEN
            frame_addrs[n++] = linebuf[(line - BLANK_LINES) % LINE_BUFS];
#else
            frame_addrs[n++] = &myrtos_framebuf[(line - BLANK_LINES) * H_ACTIVE];
#endif
        }
    }
}

#if MYRTOS_VIDEO_CHARGEN

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
// The pump runs from a timer alarm at priority 0x40, which is above
// MYRTOS_CRITICAL_BASEPRI. That is the whole design: a kernel critical section
// cannot delay it, and the measurement behind the 0x40 choice says the worst
// case it does see is 127 microseconds against 1.5 milliseconds of slack.
//
// THE RULE FOR A HANDLER ABOVE THE KERNEL APPLIES HERE. This one reads two DMA
// registers, reads cells and font bytes, and writes pixels. It calls nothing.

// ON RISC-V THIS IS EXPECTED TO TEAR, and the counter is there to say so.
// kernel/critical.h only takes the BASEPRI path on Arm; Hazard3 gets the hammer
// and masks everything, so a critical section stops the pump exactly the way it
// stopped the per-scanline interrupt the first time. Prioritised traps on
// RISC-V were tried once and reverted, and a trap stack is the prerequisite --
// so on RISC-V the honest answer today is MYRTOS_VIDEO=framebuffer.

#define PUMP_US 500

uint32_t myrtos_video_underruns, myrtos_video_pumps, myrtos_video_lines;

static int      pump_alarm = -1;
static uint32_t beam_epoch;     // active lines completed in whole frames
static uint32_t beam_last;      // the line seen last time, to catch the wrap
static uint32_t rendered_to;    // the next line to build, on the same scale

uint32_t myrtos_video_buffers(void) { return LINE_BUFS; }

// Everything the one measurement needs, in the order vidstat prints it. beam
// and rendered say whether the display is moving at all and whether anything is
// being built for it; alarm and irq say the pump was wired to something.

static inline uint32_t beam_line(void)
{
    uint32_t idx = (uint32_t)(((uintptr_t)dma_hw->ch[ch_addr].read_addr
                               - (uintptr_t)frame_addrs) / sizeof(frame_addrs[0]));
    if (idx >= FRAME_ENTRIES || idx < BLANK_LINES)
        return 0;                       // still in the vertical blanking
    idx = (idx - BLANK_LINES) / 2;
    return idx < V_ACTIVE ? idx : V_ACTIVE - 1;
}

// What the display is actually playing for one line. The buffer is live: this
// is the bytes the DMA hands to HSTX, not a re-rendering of them.
void myrtos_video_peek_line(uint32_t line, uint8_t *out, uint32_t n)
{
    const uint8_t *p = linebuf[line % LINE_BUFS];
    for (uint32_t i = 0; i < n; i++)
        out[i] = p[i];
}

void myrtos_video_stats_fill(uint32_t *eight)
{
    eight[0] = myrtos_video_underruns;
    eight[1] = myrtos_video_pumps;
    eight[2] = myrtos_video_lines;
    eight[3] = LINE_BUFS;
    eight[4] = beam_line();
    eight[5] = rendered_to;
    eight[6] = myrtos_chargen_view_back();
    eight[7] = myrtos_chargen_history();
}

static void video_pump(void)
{
    uint32_t cur = beam_line();
    if (cur < beam_last)
        beam_epoch += V_ACTIVE;         // a frame went by
    beam_last = cur;

    uint32_t now = beam_epoch + cur;

    // Behind the beam: the display has already shown a line this never wrote.
    // Say so and start again from where it is, rather than racing to catch up
    // with work whose result is already on the screen.
    if (rendered_to < now) {
        myrtos_video_underruns += now - rendered_to;
        rendered_to = now;
    }

    // One buffer of margin at each end: the one being played, and the one the
    // DMA may have already latched the address of.
    uint32_t target = now + LINE_BUFS - 2;
    while (rendered_to < target) {
        myrtos_chargen_line(rendered_to % V_ACTIVE, linebuf[rendered_to % LINE_BUFS]);
        rendered_to++;
        myrtos_video_lines++;
    }
    myrtos_video_pumps++;
}

static void pump_isr(void)
{
    timer_hw->intr = 1u << pump_alarm;                     // acknowledge
    timer_hw->alarm[pump_alarm] = timer_hw->timerawl + PUMP_US;
    video_pump();
}

static void pump_start(void)
{
    // Fill every buffer once before the first alarm, so the first frame is not
    // a screenful of whatever SRAM held at reset.
    beam_epoch = beam_last = rendered_to = 0;
    for (uint32_t i = 0; i < LINE_BUFS; i++)
        myrtos_chargen_line(i, linebuf[i]);
    rendered_to = LINE_BUFS;

    pump_alarm = (int)hardware_alarm_claim_unused(true);
    uint irq = hardware_alarm_get_irq_num(pump_alarm);
    irq_set_exclusive_handler(irq, pump_isr);
    irq_set_priority(irq, 0x40);
    irq_set_enabled(irq, true);
    hw_set_bits(&timer_hw->inte, 1u << pump_alarm);
    timer_hw->alarm[pump_alarm] = timer_hw->timerawl + PUMP_US;
}

#endif

void myrtos_video_init(void) {
    // set_sys_clock_khz does not touch clk_hstx: it kept its own source, the
    // system PLL at 150 MHz, while the processor went down to 125. The pixel
    // clock is clk_hstx/5, so the display was being driven at 30 MHz instead of
    // 25 and no monitor recognised what came out. Point it at clk_sys and say
    // so, rather than trusting a default that was never chosen.
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

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
    for (int i = 12; i <= 19; ++i) gpio_set_function(i, 0);

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
    channel_config_set_ring(&c, false, 12);          // read wraps every 4096 bytes
    channel_config_set_chain_to(&c, ch_addr);
    dma_channel_configure(ch_count, &c,
                          &dma_hw->ch[ch_data].al3_transfer_count,
                          frame_counts, 1, false);

    dma_channel_config a = dma_channel_get_default_config(ch_addr);
    channel_config_set_write_increment(&a, false);
    channel_config_set_ring(&a, false, 12);
    channel_config_set_chain_to(&a, ch_addr);        // chain to self means none
    dma_channel_configure(ch_addr, &a,
                          &dma_hw->ch[ch_data].al3_read_addr_trig,
                          frame_addrs, 1, false);

    // The display cannot wait; anything else can.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(ch_count);

#if MYRTOS_VIDEO_CHARGEN
    myrtos_chargen_init(0xf0);      // white on black, before anything prints
    pump_start();
#endif

    myrtos_print("Video: clk_sys ");
    myrtos_print_u32(clock_get_hz(clk_sys) / 1000000);
    myrtos_print(" MHz, clk_hstx ");
    myrtos_print_u32(clock_get_hz(clk_hstx) / 1000000);
    myrtos_print(" MHz\n");
    myrtos_print("Video: csr 0x");
    myrtos_print_hex(hstx_ctrl_hw->csr);
    myrtos_print(" (want 0x50050203), fifo stat 0x");
    myrtos_print_hex(hstx_fifo_hw->stat);
    myrtos_print(", 640x480, DMA ");
    myrtos_print_u32((uint32_t)ch_data);
    myrtos_print(", ");
    myrtos_print_u32((uint32_t)ch_count);
    myrtos_print(", ");
    myrtos_print_u32((uint32_t)ch_addr);
    myrtos_print("\n");
}

#if !MYRTOS_VIDEO_CHARGEN
// Something recognisable, so the first picture says whether the pinout and the
// timing are right rather than merely that something came out.
void myrtos_video_testcard(void) {
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
            myrtos_framebuf[y * H_ACTIVE + x] = c;
        }
    }
}
#endif

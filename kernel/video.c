#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/clocks.h"

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
#define H_ACTIVE        640
#define V_FRONT_PORCH   10
#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    33
#define V_ACTIVE        480
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
uint8_t myrtos_framebuf[H_ACTIVE * V_ACTIVE] __attribute__((aligned(4)));

static int ch_ping = -1, ch_pong = -1;
static bool pong, cmdlist_posted;
static uint v_scanline = V_FRONT_PORCH;
volatile uint32_t myrtos_video_irqs;   // hur många rastelinjer som lämnat oss

static void dma_irq_handler(void) {
    myrtos_video_irqs++;
    uint ch = pong ? (uint)ch_pong : (uint)ch_ping;
    dma_channel_hw_t *c = &dma_hw->ch[ch];
    dma_hw->intr = 1u << ch;
    pong = !pong;

    if (v_scanline >= V_FRONT_PORCH && v_scanline < V_FRONT_PORCH + V_SYNC_WIDTH) {
        c->read_addr = (uintptr_t)vblank_vsync_on;
        c->transfer_count = count_of(vblank_vsync_on);
    } else if (v_scanline < V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH) {
        c->read_addr = (uintptr_t)vblank_vsync_off;
        c->transfer_count = count_of(vblank_vsync_off);
    } else if (!cmdlist_posted) {
        c->read_addr = (uintptr_t)vactive_line;
        c->transfer_count = count_of(vactive_line);
        cmdlist_posted = true;
    } else {
        c->read_addr = (uintptr_t)&myrtos_framebuf[
            (v_scanline - (V_TOTAL - V_ACTIVE)) * H_ACTIVE];
        c->transfer_count = H_ACTIVE / sizeof(uint32_t);
        cmdlist_posted = false;
    }

    if (!cmdlist_posted) v_scanline = (v_scanline + 1) % V_TOTAL;
}

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
    // which fixes the pixel clock at clk_hstx/5, and is why the system runs at
    // 125 MHz rather than the SDK's 150.
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

    // Two channels feeding the same FIFO, each chaining to the other, so one is
    // always in flight while the interrupt reloads the one that just finished.
    ch_ping = dma_claim_unused_channel(true);
    ch_pong = dma_claim_unused_channel(true);

    for (int i = 0; i < 2; i++) {
        int ch    = i ? ch_pong : ch_ping;
        int other = i ? ch_ping : ch_pong;
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_chain_to(&c, other);
        channel_config_set_dreq(&c, DREQ_HSTX);
        dma_channel_configure(ch, &c, &hstx_fifo_hw->fifo,
                              vblank_vsync_off, count_of(vblank_vsync_off), false);
    }

    dma_hw->ints0 = (1u << ch_ping) | (1u << ch_pong);
    dma_hw->inte0 = (1u << ch_ping) | (1u << ch_pong);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // The display cannot wait; anything else can.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(ch_ping);

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
    myrtos_print_u32((uint32_t)ch_ping);
    myrtos_print(" and ");
    myrtos_print_u32((uint32_t)ch_pong);
    myrtos_print("\n");
}

// Something recognisable, so the first picture says whether the pinout and the
// timing are right rather than merely that something came out.
void myrtos_video_testcard(void) {
    for (uint y = 0; y < V_ACTIVE; y++) {
        for (uint x = 0; x < H_ACTIVE; x++) {
            uint8_t c;
            if (y < 64) {
                static const uint8_t bars[8] = {0xff,0xfc,0x1f,0x1c,0xe3,0xe0,0x03,0x00};
                c = bars[(x * 8) / H_ACTIVE];
            } else if (x < 8 || x >= H_ACTIVE - 8 || y >= V_ACTIVE - 8) {
                c = 0xff;                       // a border, to show the edges
            } else {
                c = (uint8_t)(((x >> 5) << 5) | ((y >> 5) << 2));
            }
            myrtos_framebuf[y * H_ACTIVE + x] = c;
        }
    }
}

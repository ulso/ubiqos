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
#include "hardware/pio.h"
#include "pico/time.h"
#include "rgb.pio.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define RGB_W 800u
#define RGB_H 480u

// Sync on one PIO block, pixels on another. Two of the chip's three, which
// leaves exactly one for the PIO USB host and nothing at all spare.
#define PIO_SYNC  pio1
#define PIO_DATA  pio2

// PIO's pin window starts here, so PIO pin 4 is GPIO20. Everything in the .pio
// file counts from this.
#define RGB_GPIO_BASE 16u

static uint16_t line[RGB_W] __attribute__((aligned(4)));
static const volatile void *line_start = line;   // what the address channel feeds back

static int ch_data, ch_addr;

// Vertical bars, eight of them, in RGB565. Nothing subtle: the point is that a
// wrong bit order or a swapped pin is visible at a glance rather than as a
// slightly wrong shade.
static void fill_bars(void)
{
    static const uint16_t bar[8] = {
        0xffffu,  // white
        0xffe0u,  // yellow
        0x07ffu,  // cyan
        0x07e0u,  // green
        0xf81fu,  // magenta
        0xf800u,  // red
        0x001fu,  // blue
        0x0000u,  // black
    };
    for (uint32_t x = 0; x < RGB_W; x++) line[x] = bar[(x * 8u) / RGB_W];
}

// The panel's own three pins. EN and RST are plain GPIO; the backlight is on a
// PWM slice on the real hardware but full brightness is a high pin, and dimming
// is not what this step is about.
static void panel_wake(void)
{
    gpio_init(MYRTOS_LCD_EN_PIN);
    gpio_set_dir(MYRTOS_LCD_EN_PIN, GPIO_OUT);
    gpio_put(MYRTOS_LCD_EN_PIN, 1);

    gpio_init(MYRTOS_LCD_RST_PIN);
    gpio_set_dir(MYRTOS_LCD_RST_PIN, GPIO_OUT);
    gpio_put(MYRTOS_LCD_RST_PIN, 0);
    sleep_ms(20);
    gpio_put(MYRTOS_LCD_RST_PIN, 1);
    sleep_ms(200);                      // Waveshare's own wait, and generous

    // THE BACKLIGHT IS ACTIVE LOW, and this pin driven high is what "the panel
    // is completely black" turned out to mean -- with every signal measured
    // correct, DE at 70% and the data pins changing.
    //
    // Nothing says so anywhere. It is deducible only from their brightness
    // function, which writes PWM_WRAP/100 * (100 - percent) as the level: at a
    // hundred per cent that is zero. Low is bright.
    //
    // A level rather than PWM, because full brightness needs no modulation.
    // Dimming would want the PWM slice this pin has, and is not what this step
    // is about.
    gpio_init(MYRTOS_LCD_BL_PIN);
    gpio_set_dir(MYRTOS_LCD_BL_PIN, GPIO_OUT);
    gpio_put(MYRTOS_LCD_BL_PIN, 0);
}

// pio_sm_init refuses a configuration its block's GPIO base cannot reach, and
// says so rather than running wrong. That return value is the one check that
// catches the whole class of mistake this driver is most exposed to, so it is
// not thrown away.
static bool sm_start(PIO pio, uint sm, uint off, pio_sm_config *c, const char *what)
{
    if (pio_sm_init(pio, sm, off, c) == PICO_OK) return true;
    myrtos_print("lcd: the ");
    myrtos_print(what);
    myrtos_print(" state machine would not take its pins\n");
    return false;
}

// Configured, NOT started. Starting it here cost a boot: the DMA filled the
// pixel machine's TX FIFO while that machine was still stopped, and the
// pio_sm_put_blocking below then waited for ever for room that nothing would
// ever make. The probe found it sitting in pio_sm_is_tx_fifo_full, which is a
// better clue than a black screen would have been.
static void dma_setup(uint data_sm)
{
    ch_data = dma_claim_unused_channel(true);
    ch_addr = dma_claim_unused_channel(true);

    // One line, halfword at a time -- the pixels are 16 bits and the state
    // machine takes one per pull -- then hand over to the address channel.
    dma_channel_config d = dma_channel_get_default_config(ch_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(PIO_DATA, data_sm, true));
    channel_config_set_chain_to(&d, ch_addr);
    dma_channel_configure(ch_data, &d, &PIO_DATA->txf[data_sm], line, RGB_W, false);

    // And the address channel puts the start of the line back, which retriggers
    // the data channel: two channels that restart each other for ever, so no
    // interrupt is needed for a picture that never changes.
    dma_channel_config a = dma_channel_get_default_config(ch_addr);
    channel_config_set_transfer_data_size(&a, DMA_SIZE_32);
    channel_config_set_read_increment(&a, false);
    channel_config_set_write_increment(&a, false);
    dma_channel_configure(ch_addr, &a, &dma_hw->ch[ch_data].al3_read_addr_trig,
                          &line_start, 1, false);
}

void myrtos_video_init(void)
{
    fill_bars();
    panel_wake();

    // The window, before anything is configured against it.
    pio_set_gpio_base(PIO_SYNC, RGB_GPIO_BASE);
    pio_set_gpio_base(PIO_DATA, RGB_GPIO_BASE);

    const uint sm_hsync = pio_claim_unused_sm(PIO_SYNC, true);
    const uint sm_vsync = pio_claim_unused_sm(PIO_SYNC, true);
    const uint sm_de    = pio_claim_unused_sm(PIO_DATA, true);
    const uint sm_data  = pio_claim_unused_sm(PIO_DATA, true);

    const uint off_hsync = pio_add_program(PIO_SYNC, &myrtos_rgb_hsync_program);
    const uint off_vsync = pio_add_program(PIO_SYNC, &myrtos_rgb_vsync_program);
    const uint off_de    = pio_add_program(PIO_DATA, &myrtos_rgb_de_program);
    const uint off_data  = pio_add_program(PIO_DATA, &myrtos_rgb_data_program);

    // Only hsync is divided: it owns the time, at two instructions per pixel.
    // The other three follow its edges and its interrupt, so they run flat out.
    const float div = (float)clock_get_hz(clk_sys) / (float)(MYRTOS_LCD_PCLK_HZ * 2u);

    bool ok = true;
    {
        pio_sm_config c = myrtos_rgb_hsync_program_get_default_config(off_hsync);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, MYRTOS_LCD_HSYNC_PIN);   // and PCLK, next along
        sm_config_set_clkdiv(&c, div);
        pio_gpio_init(PIO_SYNC, MYRTOS_LCD_HSYNC_PIN);
        pio_gpio_init(PIO_SYNC, MYRTOS_LCD_PCLK_PIN);
        pio_sm_set_consecutive_pindirs(PIO_SYNC, sm_hsync, MYRTOS_LCD_HSYNC_PIN, 2, true);
        ok &= sm_start(PIO_SYNC, sm_hsync, off_hsync, &c, "hsync");
    }
    {
        pio_sm_config c = myrtos_rgb_vsync_program_get_default_config(off_vsync);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, MYRTOS_LCD_VSYNC_PIN);
        pio_gpio_init(PIO_SYNC, MYRTOS_LCD_VSYNC_PIN);
        pio_sm_set_consecutive_pindirs(PIO_SYNC, sm_vsync, MYRTOS_LCD_VSYNC_PIN, 1, true);
        ok &= sm_start(PIO_SYNC, sm_vsync, off_vsync, &c, "vsync");
    }
    {
        pio_sm_config c = myrtos_rgb_de_program_get_default_config(off_de);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_sideset_pins(&c, MYRTOS_LCD_DE_PIN);
        pio_gpio_init(PIO_DATA, MYRTOS_LCD_DE_PIN);
        pio_sm_set_consecutive_pindirs(PIO_DATA, sm_de, MYRTOS_LCD_DE_PIN, 1, true);
        ok &= sm_start(PIO_DATA, sm_de, off_de, &c, "data enable");
    }
    {
        pio_sm_config c = myrtos_rgb_data_program_get_default_config(off_data);
        sm_config_set_out_pins(&c, MYRTOS_LCD_DATA0_PIN, 16);
        for (uint i = 0; i < 16; i++) pio_gpio_init(PIO_DATA, MYRTOS_LCD_DATA0_PIN + i);
        pio_sm_set_consecutive_pindirs(PIO_DATA, sm_data, MYRTOS_LCD_DATA0_PIN, 16, true);
        ok &= sm_start(PIO_DATA, sm_data, off_data, &c, "pixel");
    }
    if (!ok) { myrtos_print("lcd: not started\n"); return; }

    dma_setup(sm_data);

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
    dma_channel_start(ch_data);

    myrtos_print("lcd: 800x480 RGB565, pixel clock ");
    myrtos_print_u32(MYRTOS_LCD_PCLK_HZ / 1000000u);
    myrtos_print(" MHz, colour bars\n");
}

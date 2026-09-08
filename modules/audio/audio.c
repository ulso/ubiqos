// I2S out, as a driver module.
//
// A process opens /dev/audio and writes 16-bit stereo PCM, left sample then
// right, little endian -- which is what a WAV file already holds, so `cat` of
// the right kind of file is most of a player.
//
// PIO, because the RP2350 has no I2S peripheral. PIO2, which the neopixel
// driver already uses for state machine 0: a block has four of them and
// thirty-two instruction slots, and WS2812 takes four. This takes eight and
// state machine 1.
//
// The clocks come from here and the DAC follows them. That order matters: the
// TLV320DAC3100 locks its PLL to BCLK, so it cannot even report that it is
// well until something is driving the bus. Which is why this exists before any
// of the DAC's configuration does.
//
// Blocking writes into the FIFO, not DMA, and that is a first cut rather than
// the answer. DMA would need the buffer to be somewhere DMA can reach, and a
// module's memory is PSRAM, which it cannot -- the same wall sdlib hit, with
// the same way round it. See the note at the bottom.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "audio_i2s.pio.h"

#define I2S_DIN    24
#define I2S_BCLK   26        // and WS on 27, which must be BCLK + 1
#define I2S_WS     27
#define I2S_PIO    pio2
#define I2S_SM     1         // 0 is the neopixel's

// 48 kHz, and the number is not arbitrary: sys is 120 MHz here, and
// 120e6 * 4 / 48000 is exactly 10000, so the 8.8 divider comes out at 39.0625
// with no rounding at all. 44100 does not divide as kindly.
#define I2S_HZ     48000

static const myrtos_kernel_api_t *K;
static bool ready;

// --- THE DAC ---------------------------------------------------------------
// The TLV320DAC3100 at 0x18, which is the other half of "audio" and has no
// business being somebody else's problem: a sound device that needs twenty
// register writes typed at it before it makes a noise is not a device.
//
// It goes over the same I2C the i2cbus driver owns, and that is safe rather
// than lucky: this runs once, inside configure, at boot, before any process
// exists to be using the bus.
//
// The clocks are the whole difficulty and they were worked out rather than
// copied. The PLL takes BCLK, which the state machine above makes at
// 32 x 48000 = 1.536 MHz. P=1, R=1, J=64 puts the PLL at 98.304 MHz, inside
// the 80-110 MHz the part requires. Then NDAC 8, MDAC 2 and DOSR 128 divide it
// down: 98.304e6 / (8 x 2 x 128) is exactly 48000, with nothing rounded
// anywhere in the chain.
//
// The proof that it is right is readable: the DACs refuse to power up without
// a valid clock, so page 0 register 37 coming back 0x88 -- left powered, right
// powered -- says the PLL locked.
#define DAC_ADDR 0x18

typedef struct { uint8_t page, reg, val, wait_ms; } dac_step_t;

static const dac_step_t dac_init[] = {
    { 0, 0x01, 0x01, 10 },   // software reset, and give it a moment

    // Clocks. PLL from BCLK, codec from the PLL.
    { 0, 0x04, 0x07,  0 },
    { 0, 0x05, 0x91,  0 },   // PLL on, P=1, R=1
    { 0, 0x06, 0x40,  0 },   // J=64
    { 0, 0x07, 0x00,  0 },   // D=0
    { 0, 0x08, 0x00, 10 },
    { 0, 0x0b, 0x88,  0 },   // NDAC on, 8
    { 0, 0x0c, 0x82,  0 },   // MDAC on, 2
    { 0, 0x0d, 0x00,  0 },   // DOSR high
    { 0, 0x0e, 0x80,  0 },   // DOSR = 128

    { 0, 0x1b, 0x00,  0 },   // I2S, 16 bit, and we are the master of the clocks

    { 0, 0x3f, 0xd4, 10 },   // both DACs on, left to left and right to right
    { 0, 0x40, 0x00,  0 },   // unmuted
    { 0, 0x41, 0x00,  0 },   // 0 dB
    { 0, 0x42, 0x00,  0 },

    // The analogue side.
    { 1, 0x01, 0x08,  0 },   // no weak AVDD-to-DVDD tie
    { 1, 0x02, 0x01, 10 },   // analogue blocks powered
    { 1, 0x1f, 0xc4, 50 },   // headphone drivers up; they take a while
    { 1, 0x21, 0x4e,  0 },   // de-pop on the way up
    { 1, 0x23, 0x44,  0 },   // DAC left to HPL, DAC right to HPR
    { 1, 0x24, 0x80,  0 },   // and the volume path in circuit
    { 1, 0x25, 0x80,  0 },
    { 1, 0x28, 0x06,  0 },   // drivers unmuted at 0 dB
    { 1, 0x29, 0x06, 10 },
    { 0, 0x00, 0x00,  0 },   // leave it on page 0, where a reader expects it
};

static bool dac_configure(void)
{
    uint8_t page = 0xff;
    for (uint32_t i = 0; i < sizeof dac_init / sizeof dac_init[0]; i++) {
        const dac_step_t *st = &dac_init[i];
        if (st->page != page) {
            uint8_t sel[2] = { 0x00, st->page };
            if (K->i2c_write(K->i2c, DAC_ADDR, sel, 2, false) < 0) return false;
            page = st->page;
        }
        uint8_t w[2] = { st->reg, st->val };
        if (K->i2c_write(K->i2c, DAC_ADDR, w, 2, false) < 0) return false;
        if (st->wait_ms) K->busy_wait_us((uint64_t)st->wait_ms * 1000u);
    }
    return true;
}

static int32_t audio_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;

    // The upper GPIO window, as the neopixel driver already asked for -- one
    // PIO block reaches 0-31 or 16-47, and every pin here is above 16.
    K->pio_set_gpio_base(I2S_PIO, 16);

    int32_t offset = K->pio_add_program(I2S_PIO, &audio_i2s_program);
    if (offset < 0) {
        K->print("audio: no room in PIO2 for the program\n");
        return -1;
    }

    pio_sm_config c = audio_i2s_program_get_default_config((uint)offset);
    sm_config_set_out_pins(&c, I2S_DIN, 1);
    sm_config_set_sideset_pins(&c, I2S_BCLK);
    // Shifted left, because I2S puts the most significant bit first, and
    // pulled automatically every 32 bits -- one word is one stereo frame.
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // 8.8 fixed point, in integers. sys * 4 / fs is the whole of it: a frame is
    // 64 PIO cycles, so the divider is sys / (fs * 64), and multiplying by 256
    // for the fixed point leaves sys * 4 / fs. No float anywhere -- see
    // modules/neopixel, where reaching for one linked on Arm and would not
    // link on RISC-V.
    uint32_t div256 = (K->clock_hz() / I2S_HZ) * 4u;
    sm_config_set_clkdiv_int_frac8(&c, div256 >> 8, (uint8_t)(div256 & 0xffu));

    K->gpio_set_function(I2S_DIN, GPIO_FUNC_PIO2);
    K->gpio_set_function(I2S_BCLK, GPIO_FUNC_PIO2);
    K->gpio_set_function(I2S_WS, GPIO_FUNC_PIO2);

    uint64_t pins = (1ull << I2S_DIN) | (1ull << I2S_BCLK) | (1ull << I2S_WS);
    K->pio_sm_set_pindirs_with_mask64(I2S_PIO, I2S_SM, pins, pins);

    K->pio_sm_init(I2S_PIO, I2S_SM, (uint32_t)offset + audio_i2s_offset_entry_point, &c);
    pio_sm_set_enabled(I2S_PIO, I2S_SM, true);

    // The clocks are running now, which is the order the DAC needs: its PLL
    // locks to BCLK, so it cannot be configured -- or even report that it is
    // well -- until something is driving the bus.
    if (!dac_configure()) {
        K->print("  audio driver: no DAC answering at 0x18\n");
        // The state machine stays running. A board with no codec still has an
        // I2S output, and somebody with a logic analyser would rather have the
        // clocks than a driver that gave up.
    }

    ready = true;
    K->print("  audio driver: I2S on GP24/26/27, 48 kHz, PIO2, TLV320 at 0x18\n");
    return 0;
}

static int32_t audio_open(void)  { return ready ? 0 : -1; }
static int32_t audio_close(void) { return 0; }

// One 32-bit word is one stereo frame: left in the high half, right in the
// low, because the shift register goes out most significant bit first and I2S
// sends the left channel first.
static int32_t audio_write(const uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;

    uint32_t frames = len / 4;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t l = (uint32_t)buf[i * 4 + 0] | ((uint32_t)buf[i * 4 + 1] << 8);
        uint32_t r = (uint32_t)buf[i * 4 + 2] | ((uint32_t)buf[i * 4 + 3] << 8);
        pio_sm_put_blocking(I2S_PIO, I2S_SM, (l << 16) | (r & 0xffffu));
    }
    // Whatever was left over was not a whole frame and is not played. Claiming
    // it anyway keeps a caller that writes in odd-sized pieces from looping for
    // ever on a remainder it can never make bigger.
    return (int32_t)len;
}

// Room in the FIFO, in bytes. A caller that asks before writing does not block
// at all; one that does not, blocks in the write, which is the ordinary
// contract for a device that can fill up.
static int32_t audio_writable(void)
{
    if (!ready) return 0;
    uint32_t free_words = 8u - pio_sm_get_tx_fifo_level(I2S_PIO, I2S_SM);
    return (int32_t)(free_words * 4u);
}

static bool audio_init(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = audio_init,
    .ops = {
        .module_name = "i2sout",
        .configure = audio_configure,
        .open = audio_open, .write = audio_write, .read = 0,
        .close = audio_close, .writable = audio_writable,
    },
};

// --- WHAT THIS IS NOT -----------------------------------------------------
// Every frame goes through a system call and a blocking FIFO push, so playing
// anything continuously spends the machine on it. The answer is DMA with a
// ring, and the obstacle is known rather than guessed: this module's memory is
// PSRAM and DMA cannot reach it, so the ring has to come from the kernel's
// mem_alloc -- exactly what sdlib's control blocks had to do, and for exactly
// the same reason.

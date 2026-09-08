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

    ready = true;
    K->print("  audio driver: I2S on GP24/26/27, 48 kHz, PIO2\n");
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

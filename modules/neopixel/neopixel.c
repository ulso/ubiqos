// The five RGB LEDs, as a driver module.
//
// A process opens /dev/leds and writes three bytes per pixel -- red, green,
// blue -- so `echo` cannot drive it but anything that can write bytes can. Five
// pixels is fifteen bytes; a shorter write sets the pixels it reaches and
// leaves the rest alone, which is what makes "light the first one" a three-byte
// write rather than a protocol.
//
// PIO, and not a choice. The wire carries no clock: a bit is a pulse whose
// length says which bit it is, 1.25 us each. Bit-banging fifteen bytes means
// 150 microseconds of holding the processor still, and this machine has a USB
// host on PIO that loses a packet in a fraction of that -- a lesson already
// paid for twice here. A state machine does it while the processor is
// elsewhere.
//
// PIO2, because PIO0 is the USB host and PIO1 is the SD card. The RP2350 has
// three, and this is the first thing to want the third.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

#define LED_PIN      32
#define LED_COUNT     5
#define LED_PIO      pio2
#define LED_SM        0

// 800 kHz on the wire, ten PIO cycles a bit, so eight million cycles a second.
// clk_sys is 120 MHz here -- chosen for PIO-USB, see the note in video.c -- and
// 120/8 divides exactly, which is the sort of thing worth checking rather than
// assuming.
#define LED_HZ       800000
#define PIO_CYCLES   (ws2812_T1 + ws2812_T2 + ws2812_T3)

static const myrtos_kernel_api_t *K;
static bool ready;
static uint8_t pixels[LED_COUNT * 3];

static int32_t leds_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;

    // The pin is above 31, so this PIO has to be told to look at the upper
    // window -- one block reaches GPIO 0-31 or 16-47, never both. The SD driver
    // learned this the hard way: state machines configured for pins they cannot
    // see drive nothing and wait for ever.
    K->pio_set_gpio_base(LED_PIO, 16);

    int32_t offset = K->pio_add_program(LED_PIO, &ws2812_program);
    if (offset < 0) {
        K->print("neopixel: no room in PIO2 for the program\n");
        return -1;
    }

    pio_sm_config c = ws2812_program_get_default_config((uint)offset);
    sm_config_set_sideset_pins(&c, LED_PIN);
    // Shifted left and pulled automatically at 24 bits, so a pixel is one word
    // in the FIFO with its colour in the high three bytes.
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // Integer arithmetic, not float, and RISC-V is what found that out.
    //
    // sm_config_set_clkdiv takes a float. On Arm with an FPU that compiles to
    // instructions; on RV32IMAC it compiles to calls into libgcc -- __addsf3,
    // __mulsf3, __fixunssfsi -- which a module built -nostdlib does not have,
    // so this linked on one machine and would not link on the other. The
    // divider is a 16.8 fixed-point number underneath anyway, so computing it
    // that way is both portable and closer to what the hardware wants.
    //
    // And in 32 bits, which took a second try. Multiplying by 256 first
    // overflows a word -- 120 MHz times 256 is thirty billion -- and doing it
    // in 64 wants __udivdi3, which is libgcc again and gets us nowhere. But
    // clock_hz * 256 / 8000000 is exactly clock_hz / 31250, because 8000000
    // divides by 256 without remainder, so the whole thing is one 32-bit
    // division.
    _Static_assert((LED_HZ * PIO_CYCLES) % 256 == 0,
                   "the divisor must reduce exactly, or this arithmetic drifts");
    uint32_t div256 = K->clock_hz() / (LED_HZ * PIO_CYCLES / 256);
    sm_config_set_clkdiv_int_frac8(&c, div256 >> 8, (uint8_t)(div256 & 0xffu));

    // pio_gpio_init is inline but calls gpio_set_function, which is a real
    // function this module does not have -- so it goes through the table like
    // everything else. GPIO_FUNC_PIO2 is 8 on this part; the SDK's own enum
    // says so, and the host build's copy says 11, which is why the number is
    // taken from the target header and not from memory.
    K->gpio_set_function(LED_PIN, GPIO_FUNC_PIO2);
    K->pio_sm_set_pindirs_with_mask64(LED_PIO, LED_SM, 1ull << LED_PIN, 1ull << LED_PIN);
    K->pio_sm_init(LED_PIO, LED_SM, (uint32_t)offset, &c);
    pio_sm_set_enabled(LED_PIO, LED_SM, true);

    ready = true;
    K->print("  neopixel driver: 5 LEDs on GP32, PIO2\n");
    return 0;
}

static int32_t leds_open(void)  { return ready ? 0 : -1; }
static int32_t leds_close(void) { return 0; }

// The chip wants green first, then red, then blue -- which is not the order
// anybody writes a colour in, so the swap happens here rather than in every
// caller.
static void show(void)
{
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        uint32_t grb = ((uint32_t)pixels[i * 3 + 1] << 16)
                     | ((uint32_t)pixels[i * 3 + 0] << 8)
                     |  (uint32_t)pixels[i * 3 + 2];
        pio_sm_put_blocking(LED_PIO, LED_SM, grb << 8u);
    }
    // The strip latches on a gap of more than 50 microseconds. This is well
    // over it and costs nothing anybody can see; without it two writes in quick
    // succession run together and the second is read as more pixels.
    K->busy_wait_us(300);
}

static int32_t leds_write(const uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;
    uint32_t n = len > sizeof pixels ? sizeof pixels : len;
    for (uint32_t i = 0; i < n; i++) pixels[i] = buf[i];
    show();
    // The whole write is claimed even when it was longer than there are
    // pixels. A caller writing a picture of a hundred lamps at five of them is
    // making a mistake, but blocking for ever is not the way to tell them.
    return (int32_t)len;
}

// Read gives back what is showing, which is what makes a device a device rather
// than a command: the state can be asked for.
static int32_t leds_read(uint8_t *buf, uint32_t len)
{
    uint32_t n = len > sizeof pixels ? sizeof pixels : len;
    for (uint32_t i = 0; i < n; i++) buf[i] = pixels[i];
    return (int32_t)n;
}

static int32_t leds_readable(void) { return (int32_t)sizeof pixels; }

static bool leds_init(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = leds_init,
    .ops = {
        .module_name = "neopixel",
        .configure = leds_configure,
        .open = leds_open, .write = leds_write, .read = leds_read,
        .close = leds_close, .readable = leds_readable,
    },
};

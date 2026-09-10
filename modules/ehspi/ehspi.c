// ESP-Hosted's SPI full-duplex transport, host side.
//
// The ESP32-C6 stopped being a TCP/IP stack on 10 September 2026 and became a
// radio. What crosses this bus is not sockets any more but frames: Ethernet
// frames on one interface number and RPC on another. See docs/esp-hosted.
//
// --- WHY THIS HAS A THREAD --------------------------------------------------
//
// Every exchange is a FIXED 1600 bytes, whatever the payload, because the
// co-processor arms its SPI slave for that much and a shorter clocking leaves
// it half fed. At 8 MHz that is 1.6 milliseconds.
//
// A driver's read and write are called from the trap handler, with interrupts
// off. Sixteen hundred microseconds there would starve the display and the USB
// controller, which is the rule kernel/syscalls.c states and this would break
// more thoroughly than anything yet. So the transaction happens in a kernel
// thread of this driver's own -- the shape modules/wifilib already uses for
// the same reason -- and read and write only move bytes to and from rings,
// which is short enough for a trap.
//
// --- HOW THE CO-PROCESSOR ASKS ----------------------------------------------
//
// Two pins, both driven by the C6:
//
//   HANDSHAKE (GP3)   high when it has armed its SPI and may be clocked
//   DATA READY (GP23) high when what it has armed is real rather than a dummy
//
// The host clocks a transaction when HANDSHAKE is high AND either DATA READY
// is high or the host itself has something to send. Any other moment is an
// interrupt to ignore. Because the exchange is full duplex, one transaction
// carries a frame each way, and when either side has nothing it sends a dummy:
// interface type ESP_MAX_IF, length zero.
//
// This polls both pins on the millisecond tick rather than taking an interrupt
// on HANDSHAKE. That is a first version and it is honest about it: a tick is
// 1600 bytes of headroom at this clock, which is plenty to bring the link up
// and not enough to run a network at speed. The interrupt is the next thing.
//
// --- THE PINS ARE SHARED ----------------------------------------------------
//
// GP23 is DATA READY here and the C6's download strap in modules/esplink, and
// GP3 was WiFiNINA's ACK. Nothing is claimed twice: the NINA driver is only
// linked when somebody types `wifi`, and espflash only drives GP23 while it is
// resetting the chip. Running either while this is up would be running two
// drivers against one chip, which no amount of pin ownership would make sane.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/modules.h"   // myrtos_sleep, which yields
#include "hardware/spi.h"
#include "hardware/structs/spi.h"
#include "hardware/gpio.h"

static const myrtos_kernel_api_t *K;

#define MYRTOS_PRIO_EH 21          // just under the filesystem, just over a shell

// Not a choice: it is the size the co-processor arms its slave for.
#define EH_BUF 1600u

// The V1 header, which is what the link speaks until somebody negotiates V2.
// Twelve bytes, little-endian, and byte 0 carries two fields.
#define HDR_V1 12u
#define HDR_V2 20u
#define V2_MAGIC 0xe9u

#define IF_PRIV     5u
#define IF_MAX      8u             // the dummy's interface type

static uint32_t pin_sck, pin_mosi, pin_miso, pin_cs, pin_hs, pin_dr;

static uint8_t  txbuf[EH_BUF];
static uint8_t  rxbuf[EH_BUF];
static volatile bool tx_pending;   // txbuf holds something worth sending

static myrtos_eh_stats_t stats;
static volatile bool running;

// --- THE WIRE ---------------------------------------------------------------

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// A plain 16-bit sum of every byte, wrapping, with the checksum field itself
// counted as zero. Not a CRC, and not worth mistaking for one: it catches a
// bit flip and would not notice two that cancel. SPI has no error detection of
// its own, which is the only reason it is here at all.
static uint16_t frame_checksum(const uint8_t *buf, uint16_t len, uint16_t at)
{
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum += (i == at || i == at + 1) ? 0 : buf[i];
    }
    return sum;
}

// What the host sends when it has nothing: a well-formed frame that says so.
// Zeroes would read as interface type 0, which is ESP_INVALID_IF and means
// something different -- "this is broken" rather than "this is empty".
static void build_dummy(void)
{
    for (uint32_t i = 0; i < HDR_V1; i++) txbuf[i] = 0;
    txbuf[0] = (uint8_t)(IF_MAX | (0xf << 4));    // if_type, and if_num 0xF
    put16(txbuf + 2, 0);                          // no payload
    put16(txbuf + 4, HDR_V1);                     // where a payload would start
    put16(txbuf + 6, frame_checksum(txbuf, HDR_V1, 6));
}

static void transact(void)
{
    K->gpio_put(pin_cs, 0);
    K->spi_write_read(K->spi, txbuf, rxbuf, EH_BUF);
    K->gpio_put(pin_cs, 1);
    stats.transactions++;
}

static void take_frame(void)
{
    uint16_t hdr, len, off, sum, want;
    uint8_t iftype;

    // Which header this is, from byte 0, because the two layouts are not
    // interchangeable and a raw cast to the wrong one reads length out of the
    // middle of something else.
    if (rxbuf[0] == V2_MAGIC) {
        hdr    = HDR_V2;
        iftype = (uint8_t)(rxbuf[4] & 0x3f);
        off    = le16(rxbuf + 8);
        len    = le16(rxbuf + 10);
        sum    = le16(rxbuf + 12);
        want   = 12;
    } else {
        hdr    = HDR_V1;
        iftype = (uint8_t)(rxbuf[0] & 0x0f);
        len    = le16(rxbuf + 2);
        off    = le16(rxbuf + 4);
        sum    = le16(rxbuf + 6);
        want   = 6;
    }

    // The dummy is recognised BEFORE the header is checked, and that order is
    // not cosmetic. A dummy says only "nothing this time" -- interface type
    // ESP_MAX_IF and length zero -- and nothing promises its offset field says
    // twelve. Checking the offset first counted every one of them as a broken
    // header, which is how the very first exchange of the link came to be
    // reported as a fault.
    if (iftype == IF_MAX || len == 0) { stats.dummies++; return; }

    if (off != hdr || (uint32_t)len + hdr > EH_BUF) {
        stats.bad_header++;
        if (!stats.bad_seen) {
            for (int i = 0; i < 12; i++) stats.first_bad[i] = rxbuf[i];
            stats.bad_seen = 1;
        }
        return;
    }

    if (sum != frame_checksum(rxbuf, (uint16_t)(hdr + len), want)) {
        stats.bad_checksum++;
        return;
    }

    stats.frames++;
    if (iftype < 9) stats.by_if[iftype]++;

    // The one frame kept whole, because it is the one that says the link came
    // up: the co-processor announces itself on the private interface with its
    // capabilities, and that announcement is the whole point of this step.
    if (iftype == IF_PRIV) {
        uint16_t n = len > sizeof(stats.last_priv) ? (uint16_t)sizeof(stats.last_priv) : len;
        for (uint16_t i = 0; i < n; i++) stats.last_priv[i] = rxbuf[hdr + i];
        stats.last_priv_len = n;
    }
}

static void eh_thread(void)
{
    build_dummy();

    for (;;) {
        // myrtos_sleep, NOT K->sleep_ms, and the sleep is first with no path
        // around it. This wedged the board twice in one afternoon and both
        // times for a reason modules/wifilib already had written down.
        //
        // The first time the sleep was at the bottom and a `continue` skipped
        // it, so that a burst could be taken as a burst. With handshake and
        // data ready both stuck high that is back-to-back 1.6 ms transactions
        // at priority 21, and the shell at 16 never runs again.
        //
        // The second time the sleep was unconditional and it still wedged,
        // because K->sleep_ms is the SDK's, which BUSY-WAITS. A kernel thread
        // that spins never reaches the scheduler: it is only preempted where
        // it makes a system call. wifilib.c:98 says so in as many words --
        // "seconds of spinning is exactly what froze the machine when
        // sleep_ms was used instead of myrtos_sleep" -- and I had read that
        // file the same morning, for the pin numbers.
        //
        // One transaction per tick is 1600 bytes a millisecond, which is 1.6
        // megabytes a second and more than this link will ever carry. There is
        // nothing to buy by going faster and a working machine to lose.
        myrtos_sleep(1);

        // The co-processor decides when. Clocking a slave that has not armed
        // itself reads nothing and, worse, leaves it out of step with the host
        // for every transaction after.
        if (!K->gpio_get(pin_hs)) continue;
        if (!K->gpio_get(pin_dr) && !tx_pending) continue;

        transact();
        if (tx_pending) { stats.sent++; tx_pending = false; build_dummy(); }
        take_frame();
    }
}

// --- THE DEVICE -------------------------------------------------------------

static int32_t eh_configure(const void *config, uint32_t size)
{
    if (size < sizeof(myrtos_ehspi_config_t)) return -1;
    const myrtos_ehspi_config_t *c = (const myrtos_ehspi_config_t*)config;

    pin_sck  = c->sck_pin;  pin_mosi = c->mosi_pin;
    pin_miso = c->miso_pin; pin_cs   = c->cs_pin;
    pin_hs   = c->handshake_pin;
    pin_dr   = c->data_ready_pin;

    K->spi_init(K->spi, c->baud_rate);

    // Mode 3, which the co-processor's own log states and does not negotiate:
    // clock idles high and data is sampled on the second edge. spi_init leaves
    // mode 0, and the two differ by exactly the transaction that reads as
    // plausible rubbish rather than failing.
    spi_hw_t *hw = spi_get_hw((spi_inst_t*)K->spi);
    hw->cr1 &= ~SPI_SSPCR1_SSE_BITS;
    hw->cr0 |= SPI_SSPCR0_SPO_BITS | SPI_SSPCR0_SPH_BITS;
    hw->cr1 |= SPI_SSPCR1_SSE_BITS;

    K->gpio_set_function(pin_sck,  MYRTOS_GPIO_FUNC_SPI);
    K->gpio_set_function(pin_mosi, MYRTOS_GPIO_FUNC_SPI);
    K->gpio_set_function(pin_miso, MYRTOS_GPIO_FUNC_SPI);

    // Chip select by hand, as the NINA driver did on these same pins: the
    // hardware's own select drops between bytes and this slave wants one long
    // assertion around the whole 1600.
    K->gpio_init(pin_cs);
    K->gpio_set_dir(pin_cs, true);
    K->gpio_put(pin_cs, 1);

    K->gpio_init(pin_hs);
    K->gpio_set_dir(pin_hs, false);
    K->gpio_init(pin_dr);
    K->gpio_set_dir(pin_dr, false);

    if (running) return 0;
    if (K->kernel_thread(eh_thread, 2048, MYRTOS_PRIO_EH) < 0) {
        K->print("eh: could not start the transport thread\n");
        return -1;
    }
    running = true;

    K->print("  eh driver: SPI mode 3, ");
    K->print_u32(c->baud_rate / 1000000u);
    K->print(" MHz, handshake GP");
    K->print_u32(pin_hs);
    K->print(", data ready GP");
    K->print_u32(pin_dr);
    K->print("\n");
    return 0;
}

static int32_t eh_open(void)  { return running ? 0 : -1; }
static int32_t eh_close(void) { return 0; }

// Nothing streams through this device yet. A frame has an interface number and
// a length, and neither survives being poured into a byte stream -- so the
// bytes will move as frames when there is something to move them for, and the
// device answers questions in the meantime.
static int32_t eh_write(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; return -1; }
static int32_t eh_read(uint8_t *buf, uint32_t len) { (void)buf; (void)len; return 0; }

static int32_t eh_getstat(uint32_t code, void *data, uint32_t len)
{
    if (code != MYRTOS_SS_EH_STATS || len < sizeof(myrtos_eh_stats_t)) return -1;
    // Byte by byte, and not a struct assignment. The compiler turns that into
    // a call to memcpy, and a module links no C library -- the failure is a
    // dangerous relocation at link time, which at least says so early.
    const uint8_t *from = (const uint8_t*)&stats;
    uint8_t *to = (uint8_t*)data;
    for (uint32_t i = 0; i < sizeof(stats); i++) to[i] = from[i];
    return 0;
}

static bool eh_init_module(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = eh_init_module,
    .ops = {
        .module_name = "ehspi",
        .configure = eh_configure,
        .open = eh_open, .write = eh_write, .read = eh_read,
        .close = eh_close,
        .getstat = eh_getstat,
    },
};

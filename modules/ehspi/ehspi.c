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
#include "hardware/dma.h"
#include "hardware/structs/dma.h"

static const myrtos_kernel_api_t *K;

#define MYRTOS_PRIO_EH 21          // just under the filesystem, just over a shell

// Not a choice: it is the size the co-processor arms its slave for.
#define EH_BUF 1600u

// The V1 header, which is what the link speaks until somebody negotiates V2.
// Twelve bytes, little-endian, and byte 0 carries two fields.
#define HDR_V1 12u
#define HDR_V2 20u
#define V2_MAGIC 0xe9u

#define IF_STA      1u             // network frames, plain Ethernet
#define IF_SERIAL   3u             // the RPC control plane
#define IF_PRIV     5u
#define IF_MAX      8u             // the dummy's interface type

// --- THE HANDSHAKE ----------------------------------------------------------
//
// The co-processor announces itself and then WAITS. Espressif's own note is
// blunt about it -- "getting this right is the whole game; if it does not
// complete, feature debugging is premature" -- and it is exactly the mistake
// that was made here first: an RPC request went out before the host had
// answered the announcement, the chip took the frame and said nothing, and
// there was nothing wrong with the frame at all.
//
// The answer is a private event of the same type, carrying type/length/value
// triplets. Only four are sent, and what is LEFT OUT matters more:
//
//   0x1A, the RPC version, is a strict match on the far side and a mismatch
//   calls abort() -- the chip reboots. Not sending it leaves the version the
//   chip already advertised, which is the one we want.
//
//   0x23, the RPC version ack, reads anything that is not V3 as V1 and would
//   talk us down a protocol.
//
// Neither is needed: the chip configures its RPC endpoints when it has had any
// init event at all from the host.
#define PRIV_EVENT_INIT   0x22u
#define PRIV_PKT_EVENT    0x33u    // header byte 11; 0x01 would mean a command

#define TLV_HOST_CAPS     0x44u
#define TLV_RCVD_CHIP_ID  0x45u
#define TLV_THROTTLE_HIGH 0x47u
#define TLV_THROTTLE_LOW  0x48u

static uint32_t pin_sck, pin_mosi, pin_miso, pin_cs, pin_hs, pin_dr;

// --- WHY THESE ARE NOT STATICS ----------------------------------------------
//
// A driver module's own memory is in the module pool, which is PSRAM, and DMA
// to PSRAM is not reliably visible to the CPU on this machine -- the SD driver
// bounces through SRAM for exactly this reason. K->driver_alloc gives SRAM
// that belongs to the driver rather than to whichever process happened to
// start it, and K->dma_safe is asked rather than assumed, because the library
// that does the DMA is not the one that knows the memory map.
static uint8_t *txbuf;
static uint8_t *rxbuf;
static volatile bool tx_pending;   // txbuf holds something worth sending

// --- WHY DMA -----------------------------------------------------------------
//
// The exchange is always exactly 1600 bytes. There is no escaping and no
// framing on this wire -- the length lives in the header, not in the stream --
// so the transfer size is known before it starts, which is what makes a DMA
// possible at all. That is the difference between this and the SLIP the ROM
// loader speaks over UART, where a byte can become two and nobody can say how
// many are coming.
//
// It matters because the alternative is measured: spi_write_read polls a byte
// at a time and held 534 microseconds of CPU at priority 21, ABOVE THE SHELL,
// for every frame. The thread now starts the transfer and sleeps, and the tick
// that wakes it is the one it was going to wait for anyway.
// A frame on its way out, built by a write and picked up by the thread. It is
// staged rather than written into txbuf directly because txbuf may be under a
// DMA at the moment somebody writes, and a trap does not wait for anything.
static uint8_t *stage;
static volatile uint32_t stage_len;

// The link's own reply to the announcement, owed as soon as one arrives.
static volatile bool want_hello;
static uint8_t chip_id;

// Control-plane frames in, four deep.
//
// It was one deep, on the reasoning that a request gets one answer. That was
// wrong, and wrong in a way that looked like a timeout: the chip pushes EVENTS
// on the same interface -- the radio started, a station connected -- and one
// arriving between a response and the reader taking it overwrote the response.
// WifiStart appeared to go unanswered for ten seconds while its reply had been
// sitting there and been replaced by the event it caused.
#define INBOX_SLOTS 4u

static uint8_t *inbox;                          // INBOX_SLOTS frames, EH_BUF each
static volatile uint16_t inbox_used[INBOX_SLOTS];
static volatile uint32_t inbox_head, inbox_tail;
static volatile uint32_t inbox_lost;

// --- THE DATA PLANE ---------------------------------------------------------
//
// Station frames are ordinary Ethernet and go to lwIP, which lives in the USB
// task and may be touched from nowhere else. So they are queued here and the
// kernel takes them from there on its own terms -- see kernel/lwipnet.c. Eight
// deep, because these arrive in bursts of whatever the air was carrying and a
// tick is a long time on a network.
#define NET_SLOTS 8u

static uint8_t *netbox;                         // NET_SLOTS frames, EH_BUF each
static volatile uint16_t netbox_used[NET_SLOTS];
static volatile uint32_t netbox_head, netbox_tail;
static volatile uint32_t netbox_lost;

// One frame out, the same one-at-a-time arrangement the control plane has.
static uint8_t *netstage;
static volatile uint32_t netstage_len;
static volatile bool nettx_pending;

// The station's own address, which this driver does not ask for and only
// keeps: it takes a control-plane RPC to fetch, and that lives in ehrpc.
static uint8_t sta_mac[6];
static volatile bool sta_mac_known;

static int32_t dma_tx = -1, dma_rx = -1;
static bool in_flight;
static uint32_t started_us;
static uint32_t cpu_us;   // what transact_start spent, added to in finish

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
// The host's half of the handshake, built where the transfer buffer is safe to
// touch -- which is the thread and nowhere else.
static void build_hello(void)
{
    uint8_t *p = txbuf + HDR_V1;
    uint32_t n = 0;

    p[n++] = PRIV_EVENT_INIT;
    p[n++] = 0;                                  // length, filled in below

    p[n++] = TLV_HOST_CAPS;     p[n++] = 1; p[n++] = 0;
    p[n++] = TLV_RCVD_CHIP_ID;  p[n++] = 1; p[n++] = chip_id;
    // Flow control off. The chip only throttles when the host asks it to, and
    // a host that has not measured its own buffers has no business naming a
    // percentage.
    p[n++] = TLV_THROTTLE_HIGH; p[n++] = 1; p[n++] = 0;
    p[n++] = TLV_THROTTLE_LOW;  p[n++] = 1; p[n++] = 0;

    p[1] = (uint8_t)(n - 2);

    for (uint32_t i = 0; i < HDR_V1; i++) txbuf[i] = 0;
    txbuf[0] = IF_PRIV;
    txbuf[11] = PRIV_PKT_EVENT;                  // not a command: an event
    put16(txbuf + 2, (uint16_t)n);
    put16(txbuf + 4, HDR_V1);
    put16(txbuf + 6, frame_checksum(txbuf, (uint16_t)(HDR_V1 + n), 6));
}

static void build_dummy(void)
{
    for (uint32_t i = 0; i < HDR_V1; i++) txbuf[i] = 0;
    txbuf[0] = (uint8_t)(IF_MAX | (0xf << 4));    // if_type, and if_num 0xF
    put16(txbuf + 2, 0);                          // no payload
    put16(txbuf + 4, HDR_V1);                     // where a payload would start
    put16(txbuf + 6, frame_checksum(txbuf, HDR_V1, 6));
}

// Select, arm both channels, and go. Returns at once: the wire takes 400
// microseconds at 32 MHz and the CPU spends none of them here.
static void transact_start(void)
{
    spi_inst_t *spi = (spi_inst_t*)K->spi;
    volatile void *dr = &spi_get_hw(spi)->dr;

    dma_channel_config tc = dma_channel_get_default_config((uint)dma_tx);
    channel_config_set_transfer_data_size(&tc, DMA_SIZE_8);
    channel_config_set_dreq(&tc, spi_get_dreq(spi, true));
    channel_config_set_read_increment(&tc, true);
    channel_config_set_write_increment(&tc, false);
    dma_channel_configure((uint)dma_tx, &tc, dr, txbuf, EH_BUF, false);

    dma_channel_config rc = dma_channel_get_default_config((uint)dma_rx);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_8);
    channel_config_set_dreq(&rc, spi_get_dreq(spi, false));
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    dma_channel_configure((uint)dma_rx, &rc, rxbuf, dr, EH_BUF, false);

    uint32_t t0 = (uint32_t)K->time_us();
    started_us = t0;
    K->gpio_put(pin_cs, 0);
    // Both in one write, so the receive side is armed before a byte can
    // arrive. Started separately, the first bytes clocked in have nowhere to
    // go and the whole frame is one short for ever after.
    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    in_flight = true;
    cpu_us = (uint32_t)K->time_us() - t0;
}

// True when the wire is finished with, either way.
static bool transact_finish(void)
{
    uint32_t t0 = (uint32_t)K->time_us();
    bool busy = dma_channel_is_busy((uint)dma_tx) || dma_channel_is_busy((uint)dma_rx);
    uint32_t took = t0 - started_us;

    if (busy) {
        // Ten milliseconds is twenty times what this can honestly take. A
        // transfer still running then is not slow, it is stuck, and leaving
        // chip select low for ever would take the link with it.
        if (took < 10000u) return false;
        hw_clear_bits(&dma_hw->ch[dma_tx].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        hw_clear_bits(&dma_hw->ch[dma_rx].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        stats.dma_timeouts++;
    }

    K->gpio_put(pin_cs, 1);
    in_flight = false;

    stats.transactions++;
    stats.wall_us = took;
    if (took > stats.worst_wall_us) stats.worst_wall_us = took;

    // The two stretches the processor was actually in this code: arming the
    // channels, and noticing they were done. Everything between was the DMA's.
    uint32_t cpu = cpu_us + ((uint32_t)K->time_us() - t0);
    stats.cpu_us = cpu;
    if (cpu > stats.worst_cpu_us) stats.worst_cpu_us = cpu;
    return true;
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

    if (iftype == IF_STA) {
        uint32_t next = (netbox_head + 1u) % NET_SLOTS;
        if (next == netbox_tail) { netbox_lost++; return; }
        uint16_t n = len > EH_BUF ? EH_BUF : len;
        uint8_t *slot = netbox + netbox_head * EH_BUF;
        for (uint16_t i = 0; i < n; i++) slot[i] = rxbuf[hdr + i];
        netbox_used[netbox_head] = n;
        netbox_head = next;
        return;
    }

    if (iftype == IF_SERIAL) {
        uint32_t next = (inbox_head + 1u) % INBOX_SLOTS;
        if (next == inbox_tail) { inbox_lost++; return; }   // full: keep the old
        uint16_t n = len > EH_BUF ? EH_BUF : len;
        uint8_t *slot = inbox + inbox_head * EH_BUF;
        for (uint16_t i = 0; i < n; i++) slot[i] = rxbuf[hdr + i];
        inbox_used[inbox_head] = n;
        inbox_head = next;
    }

    // The announcement is answered, and the answer is what opens the link.
    if (iftype == IF_PRIV && len >= 2 && rxbuf[hdr] == PRIV_EVENT_INIT) {
        // The chip id it just told us, handed straight back: the far side
        // compares it with its own and complains if they differ, which is a
        // cheap check that the two ends are talking about the same chip.
        chip_id = 0;
        for (uint16_t i = 2; i + 2u <= len; ) {
            if (rxbuf[hdr + i] == 0x12u) { chip_id = rxbuf[hdr + i + 2]; break; }
            i += 2u + rxbuf[hdr + i + 1];
        }
        want_hello = true;
    }

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

        // A transfer already going is finished before another is thought
        // about. It costs a tick to notice, which is the same tick this loop
        // was going to spend asleep, so the exchange is two ticks and no CPU
        // rather than one tick and half a millisecond of it.
        if (in_flight) {
            if (!transact_finish()) continue;
            if (want_hello)         { stats.sent++; want_hello = false; build_dummy(); }
            else if (tx_pending)    { stats.sent++; tx_pending = false; build_dummy(); }
            else if (nettx_pending) { stats.sent++; nettx_pending = false; build_dummy(); }
            take_frame();
            continue;
        }

        // The co-processor decides when. Clocking a slave that has not armed
        // itself reads nothing and, worse, leaves it out of step with the host
        // for every transaction after.
        if (!K->gpio_get(pin_hs)) continue;
        if (!K->gpio_get(pin_dr) && !tx_pending && !want_hello && !nettx_pending) continue;

        // What goes out with it, and the handshake goes first: nothing the
        // host has to say is heard until the announcement has been answered.
        // Either way txbuf is complete before the DMA can look at it, because
        // all three of these happen here and nowhere else.
        // The handshake first, then control, then data. A network frame is the
        // one of the three that can wait: nothing else moves until the link is
        // configured, and a late packet is a slow network rather than a broken
        // one.
        if (want_hello)          build_hello();
        else if (tx_pending)     { for (uint32_t i = 0; i < stage_len; i++) txbuf[i] = stage[i]; }
        else if (nettx_pending)  { for (uint32_t i = 0; i < netstage_len; i++) txbuf[i] = netstage[i]; }
        transact_start();
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

    // The SSP has to be told to raise its DMA requests; nothing else does it.
    hw->dmacr = SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS;

    if (running) return 0;

    txbuf = (uint8_t*)K->driver_alloc(EH_BUF);
    rxbuf = (uint8_t*)K->driver_alloc(EH_BUF);
    stage = (uint8_t*)K->driver_alloc(EH_BUF);
    inbox = (uint8_t*)K->driver_alloc(EH_BUF * INBOX_SLOTS);
    netbox = (uint8_t*)K->driver_alloc(EH_BUF * NET_SLOTS);
    netstage = (uint8_t*)K->driver_alloc(EH_BUF);
    if (!txbuf || !rxbuf || !stage || !inbox || !netbox || !netstage) {
        K->print("eh: no SRAM for the transfer buffers\n");
        return -1;
    }
    if (!K->dma_safe(txbuf) || !K->dma_safe(rxbuf)) {
        // Asked rather than assumed. driver_alloc gives SRAM today; the day it
        // gives something else, this says so instead of transferring into
        // memory the CPU cannot see afterwards.
        K->print("eh: the transfer buffers are not memory DMA may touch\n");
        return -1;
    }

    dma_tx = K->dma_claim_channel();
    dma_rx = K->dma_claim_channel();
    if (dma_tx < 0 || dma_rx < 0) {
        K->print("eh: no DMA channel to be had\n");
        return -1;
    }

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

// A write is one RPC message, and the driver puts the frame round it.
//
// The interface number is not in the bytes and does not need to be: this
// device carries the control plane and nothing else. Network frames will not
// come through here at all -- they belong to lwIP, which lives in the USB task
// and may not be touched from a process.
//
// One frame at a time. A second write while the first is still on its way is
// refused rather than queued, because the caller is a process that can wait
// and a queue here would be a queue nobody asked for.
static int32_t eh_write(const uint8_t *buf, uint32_t len)
{
    if (!running || tx_pending) return -1;
    if (len + HDR_V1 > EH_BUF) return -1;

    for (uint32_t i = 0; i < HDR_V1; i++) stage[i] = 0;
    stage[0] = IF_SERIAL;                     // if_num 0, which is all there is
    put16(stage + 2, (uint16_t)len);
    put16(stage + 4, HDR_V1);
    for (uint32_t i = 0; i < len; i++) stage[HDR_V1 + i] = buf[i];
    put16(stage + 6, frame_checksum(stage, (uint16_t)(HDR_V1 + len), 6));

    stage_len = HDR_V1 + len;
    tx_pending = true;
    return (int32_t)len;
}

// One frame per read, because a frame is the unit here and two of them run
// together are not a longer frame.
static int32_t eh_read(uint8_t *buf, uint32_t len)
{
    if (inbox_tail == inbox_head) return 0;
    const uint8_t *slot = inbox + inbox_tail * EH_BUF;
    uint32_t have = inbox_used[inbox_tail];
    uint32_t n = have > len ? len : have;
    for (uint32_t i = 0; i < n; i++) buf[i] = slot[i];
    inbox_tail = (inbox_tail + 1u) % INBOX_SLOTS;   // last, and only here
    return (int32_t)n;
}

static int32_t eh_readable(void) { return inbox_tail != inbox_head ? 1 : 0; }

static int32_t eh_getstat(uint32_t code, void *data, uint32_t len)
{
    if (code == MYRTOS_SS_EH_RX) {
        if (netbox_tail == netbox_head) return 0;          // nothing waiting
        const uint8_t *slot = netbox + netbox_tail * EH_BUF;
        uint32_t have = netbox_used[netbox_tail];
        if (have > len) have = len;
        uint8_t *out = (uint8_t*)data;
        for (uint32_t i = 0; i < have; i++) out[i] = slot[i];
        netbox_tail = (netbox_tail + 1u) % NET_SLOTS;      // last, and only here
        return (int32_t)have;
    }

    if (code == MYRTOS_SS_EH_MAC) {
        if (len < 6 || !sta_mac_known) return -1;
        uint8_t *out = (uint8_t*)data;
        for (int i = 0; i < 6; i++) out[i] = sta_mac[i];
        return 0;
    }

    if (code != MYRTOS_SS_EH_STATS || len < sizeof(myrtos_eh_stats_t)) return -1;
    // Byte by byte, and not a struct assignment. The compiler turns that into
    // a call to memcpy, and a module links no C library -- the failure is a
    // dangerous relocation at link time, which at least says so early.
    const uint8_t *from = (const uint8_t*)&stats;
    uint8_t *to = (uint8_t*)data;
    for (uint32_t i = 0; i < sizeof(stats); i++) to[i] = from[i];
    return 0;
}

static int32_t eh_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (code == MYRTOS_SS_EH_MAC) {
        if (len < 6) return -1;
        const uint8_t *in = (const uint8_t*)data;
        for (int i = 0; i < 6; i++) sta_mac[i] = in[i];
        sta_mac_known = true;
        return 0;
    }

    if (code == MYRTOS_SS_EH_TX) {
        if (nettx_pending) return -1;                      // one at a time
        if (len + HDR_V1 > EH_BUF) return -1;
        const uint8_t *in = (const uint8_t*)data;

        for (uint32_t i = 0; i < HDR_V1; i++) netstage[i] = 0;
        netstage[0] = IF_STA;
        put16(netstage + 2, (uint16_t)len);
        put16(netstage + 4, HDR_V1);
        for (uint32_t i = 0; i < len; i++) netstage[HDR_V1 + i] = in[i];
        put16(netstage + 6, frame_checksum(netstage, (uint16_t)(HDR_V1 + len), 6));

        netstage_len = HDR_V1 + len;
        nettx_pending = true;
        return (int32_t)len;
    }
    return -1;
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
        .readable = eh_readable,
        .close = eh_close,
        .getstat = eh_getstat, .setstat = eh_setstat,
    },
};

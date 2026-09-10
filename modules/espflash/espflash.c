#include "../../common/myrtos_abi.h"

// espflash -- talk to the ESP32-C6's ROM loader.
//
//   espflash sync    reset the chip into its serial bootloader and prove the
//                    wire, by listening to what the ROM says and then asking
//                    it to SYNC. Reads only; nothing is written to the chip.
//
// Why this exists at all, rather than a passthrough to esptool on a host: a
// flash image is full of 0x03, and kernel/usbdev.c peeks at every byte
// arriving on the USB console and takes 0x03 as Ctrl-C. Bridging the console
// through to the C6 would have the transfer killed by its own data. Making the
// console raw for the occasion would put a hole in the one mechanism that ends
// a runaway command, so myrtos speaks the ROM's protocol itself instead.
//
// Everything this does is undone by a power cycle or a reset: the strap is only
// read while the chip comes out of reset, and NINA is still in the flash.

#define ESP_DEV "/dev/esp"

// SLIP, RFC 1055. The ROM loader frames every packet this way in both
// directions.
#define SLIP_END     0xc0u
#define SLIP_ESC     0xdbu
#define SLIP_ESC_END 0xdcu
#define SLIP_ESC_ESC 0xddu

#define CMD_SYNC 0x08u

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

static void say_u32(uint32_t v) {
    char b[12];
    int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) { char c = b[--n]; myrtos_write(MYRTOS_STDOUT, &c, 1); }
}

static void say_hex(uint8_t v) {
    const char *d = "0123456789abcdef";
    char b[2] = { d[v >> 4], d[v & 15] };
    myrtos_write(MYRTOS_STDOUT, b, 2);
}

// One pin, one call, and the waiting out here. The driver cannot wait: a
// setstat runs in the trap handler with interrupts off.
static void pin(int32_t dev, uint32_t code, uint32_t level) {
    myrtos_setstat(dev, code, &level, sizeof(level));
}

// The sequence the ROM wants. IO9 low BEFORE the chip leaves reset, because
// that is the only moment it is read, and released afterwards, because the pin
// belongs to the audio DAC as well and there is no reason to sit on it.
//
// The delays are the chip's, not ours: EN wants a few milliseconds low to be a
// reset rather than a glitch, and the ROM takes tens of milliseconds to reach
// the point where it listens.
// Whatever the UART was holding, before it can be mistaken for an answer.
//
// This is not tidiness. The first run of `espflash sync` printed 32 bytes of
// "W (318) spi_flash: Detected size" under the heading "what the ROM says" --
// which is NINA's own log in ESP-IDF's format, something a ROM never prints,
// and exactly 32 bytes because that is how deep the RX FIFO is. It had been
// sitting there since the previous boot. The SYNC that followed was real, but
// the line above it was a lie, and a proof that prints stale bytes as evidence
// is worse than no proof.
static void drain(int32_t dev) {
    uint8_t b[64];
    while (myrtos_readable(dev) > 0) {
        if (myrtos_read(dev, b, sizeof(b)) <= 0) break;
    }
}

static void into_bootloader(int32_t dev) {
    drain(dev);
    pin(dev, MYRTOS_SS_ESP_STRAP, 0);
    pin(dev, MYRTOS_SS_ESP_RESET, 0);
    myrtos_sleep(20);
    pin(dev, MYRTOS_SS_ESP_RESET, 1);
    // Held a while longer than the reset edge itself. IO9 is latched as the
    // chip comes out of reset and twenty milliseconds is far more than that
    // needs -- but it is also twenty milliseconds in which nobody is reading,
    // and the RX FIFO is 32 bytes deep. So the banner arrives truncated:
    // "ESP-ROM:esp32c6-20220919 Build" and no more. That is enough to say
    // which chip answered, which is all this is for. Reading the whole banner
    // would mean listening through the strap hold, and there is no reason to
    // shorten a delay that costs nothing for a string nobody needs.
    myrtos_sleep(20);
    pin(dev, MYRTOS_SS_ESP_STRAP, 1);
}

static void into_application(int32_t dev) {
    pin(dev, MYRTOS_SS_ESP_STRAP, 1);  // released first: it must be high at reset
    pin(dev, MYRTOS_SS_ESP_RESET, 0);
    myrtos_sleep(20);
    pin(dev, MYRTOS_SS_ESP_RESET, 1);
}

// Read for a while and show it, printable or not. The C6's ROM prints a banner
// at 115200 on this same UART when it enters download mode -- "ESP-ROM:" and a
// build date and "waiting for download" -- so this alone says whether the
// wire, the strap and the reset all work.
static uint32_t listen(int32_t dev, uint32_t ms) {
    uint8_t buf[64];
    uint32_t total = 0;
    for (uint32_t waited = 0; waited < ms; waited += 5) {
        // Asked before it is read, because a read of a device with nothing in
        // it waits, and a chip that says nothing is exactly the case this is
        // here to report.
        if (myrtos_readable(dev) <= 0) { myrtos_sleep(5); continue; }
        int32_t n = myrtos_read(dev, buf, sizeof(buf));
        if (n <= 0) { myrtos_sleep(5); continue; }
        total += (uint32_t)n;
        for (int32_t i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n') say("\r\n");
            else if (c >= ' ' && c < 0x7f) myrtos_write(MYRTOS_STDOUT, &c, 1);
            else if (c != '\r') { say("<"); say_hex(c); say(">"); }
        }
    }
    return total;
}

static void put_escaped(int32_t dev, uint8_t b) {
    uint8_t two[2];
    if (b == SLIP_END)      { two[0] = SLIP_ESC; two[1] = SLIP_ESC_END; myrtos_write(dev, two, 2); }
    else if (b == SLIP_ESC) { two[0] = SLIP_ESC; two[1] = SLIP_ESC_ESC; myrtos_write(dev, two, 2); }
    else                      myrtos_write(dev, &b, 1);
}

// A command packet: direction, command, payload length, checksum, payload. The
// checksum is only meaningful for the data commands, and SYNC sends zero.
static void send_command(int32_t dev, uint8_t cmd, const uint8_t *data, uint16_t len) {
    uint8_t end = SLIP_END;
    myrtos_write(dev, &end, 1);

    uint8_t head[8] = { 0x00, cmd, (uint8_t)(len & 0xff), (uint8_t)(len >> 8),
                        0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) put_escaped(dev, head[i]);
    for (uint16_t i = 0; i < len; i++) put_escaped(dev, data[i]);

    myrtos_write(dev, &end, 1);
}

// One SLIP frame back, unescaped. Returns its length, or -1 if nothing arrived
// before the deadline.
static int32_t read_frame(int32_t dev, uint8_t *out, uint32_t cap, uint32_t ms) {
    bool started = false, escaped = false;
    uint32_t n = 0;

    for (uint32_t waited = 0; waited < ms; ) {
        uint8_t c;
        if (myrtos_readable(dev) <= 0) { myrtos_sleep(2); waited += 2; continue; }
        if (myrtos_read(dev, &c, 1) <= 0) { myrtos_sleep(2); waited += 2; continue; }

        if (c == SLIP_END) {
            if (!started) { started = true; n = 0; continue; }
            if (n) return (int32_t)n;
            continue;                        // an empty frame: keep waiting
        }
        if (!started) continue;              // noise before the first delimiter

        if (escaped) {
            c = (c == SLIP_ESC_END) ? SLIP_END : (c == SLIP_ESC_ESC) ? SLIP_ESC : c;
            escaped = false;
        } else if (c == SLIP_ESC) {
            escaped = true;
            continue;
        }
        if (n < cap) out[n++] = c;
    }
    return -1;
}

static void do_sync(int32_t dev) {
    say("resetting the C6 into its serial bootloader\r\n");
    say("(this drops the WiFi link, and the audio DAC shares the reset pin)\r\n\r\n");

    into_bootloader(dev);

    say("--- what the ROM says ---------------------------------------\r\n");
    uint32_t heard = listen(dev, 400);
    say("\r\n-------------------------------------------------------------\r\n");
    if (!heard) {
        say("nothing at all. The chip is not talking on GP9, or it never "
            "left reset.\r\n");
    } else {
        say_u32(heard);
        say(" bytes\r\n");
    }

    // The SYNC payload is fixed: two bytes the ROM looks for, a little-endian
    // 0x20120707 written backwards as the protocol has it, and thirty-two
    // 0x55s. It is not data -- it is a pattern chosen to be unmistakable in a
    // stream that may still hold the tail of a boot message.
    uint8_t payload[36];
    payload[0] = 0x07; payload[1] = 0x07; payload[2] = 0x12; payload[3] = 0x20;
    for (int i = 4; i < 36; i++) payload[i] = 0x55;

    say("\r\nSYNC");
    // The ROM answers a single SYNC several times over; one good frame is the
    // answer and the rest is noise to be left alone. Sent more than once
    // because the first often lands while the ROM is still settling.
    for (int attempt = 0; attempt < 6; attempt++) {
        say(".");
        send_command(dev, CMD_SYNC, payload, sizeof(payload));

        uint8_t frame[64];
        int32_t n = read_frame(dev, frame, sizeof(frame), 200);
        if (n < 8) continue;
        if (frame[0] != 0x01 || frame[1] != CMD_SYNC) continue;

        say("\r\nsynchronised: ");
        say_u32((uint32_t)n);
        say(" bytes back,");
        for (int32_t i = 0; i < n && i < 12; i++) { say(" "); say_hex(frame[i]); }
        say("\r\n\r\nThe ROM loader is listening. The wire, the strap and the "
            "reset all work.\r\n");

        say("putting the chip back the way it was\r\n");
        into_application(dev);
        say("done -- 'wifi connect' will find NINA again\r\n");
        return;
    }

    say("\r\nno answer. The ROM did not synchronise.\r\n");
    into_application(dev);
    say("the chip has been reset back into its application\r\n");
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: espflash sync\n\nTalks to the ESP32-C6's ROM loader over "
            "/dev/esp.\n\n  sync    reset the chip into its serial bootloader "
            "and prove the wire.\n          Reads only. The WiFi link goes down "
            "and comes back with\n          the chip; the audio DAC is reset "
            "too, since it shares the pin.\n")) return;

    if (argc < 2 || !is(argv[1], "sync")) {
        say("usage: espflash sync\r\n");
        return;
    }

    int32_t dev = myrtos_open(ESP_DEV);
    if (dev < 0) {
        say("espflash: no " ESP_DEV ". Is the esplink driver loaded?\r\n");
        return;
    }

    do_sync(dev);
    myrtos_close(dev);
}

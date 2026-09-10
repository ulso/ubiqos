#include "../../common/myrtos_abi.h"

// A flash block is a kilobyte, and it is read, escaped and sent from here.
MYRTOS_MEM_SIZE(8192);

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

#define CMD_FLASH_BEGIN 0x02u
#define CMD_FLASH_DATA  0x03u
#define CMD_FLASH_END   0x04u
#define CMD_SYNC        0x08u
#define CMD_SPI_PARAMS  0x0bu
#define CMD_SPI_ATTACH  0x0du

// What the ROM writes in one go. The stub loader raises this to 0x4000; there
// is no stub here, and the ROM's own limit is a kilobyte.
#define FLASH_BLOCK 1024u

// This board's ESP32-C6-MINI-1. The ROM has to be told, because it is what
// decides how much gets erased, and nothing on the wire can ask the chip.
#define FLASH_SIZE (4u * 1024u * 1024u)

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
static uint32_t listen_or_discard(int32_t dev, uint32_t ms, bool quiet) {
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
        if (quiet) continue;
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

// A command packet: direction, command, payload length, checksum, payload.
//
// The checksum covers only the DATA of a flash block, not the sixteen header
// bytes in front of it, and it is a running exclusive-or seeded with 0xEF
// rather than anything that would catch two errors. Every other command sends
// zero and the ROM does not look.
static void send_command(int32_t dev, uint8_t cmd, const uint8_t *head, uint16_t hlen,
                         const uint8_t *body, uint32_t blen, uint32_t checksum) {
    uint8_t end = SLIP_END;
    myrtos_write(dev, &end, 1);

    uint32_t len = (uint32_t)hlen + blen;
    uint8_t h[8] = { 0x00, cmd, (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff),
                     (uint8_t)(checksum & 0xff), (uint8_t)((checksum >> 8) & 0xff),
                     (uint8_t)((checksum >> 16) & 0xff), (uint8_t)((checksum >> 24) & 0xff) };
    for (int i = 0; i < 8; i++) put_escaped(dev, h[i]);
    for (uint16_t i = 0; i < hlen; i++) put_escaped(dev, head[i]);
    for (uint32_t i = 0; i < blen; i++) put_escaped(dev, body[i]);

    myrtos_write(dev, &end, 1);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
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

// Reset the chip into its ROM loader and get it talking, which is the first
// half of every job here. Loud when a person asked for it and quiet when it is
// only the step before a write.
static bool sync_rom(int32_t dev, bool quiet) {
    into_bootloader(dev);

    // The banner is read either way. Left in the FIFO it would be noise in
    // front of the first reply, and read_frame would have to walk past it.
    if (!quiet) {
        say("--- what the ROM says ---------------------------------------\r\n");
    }
    uint32_t heard = listen_or_discard(dev, 400, quiet);
    if (!quiet) {
        say("\r\n-------------------------------------------------------------\r\n");
        if (!heard) {
            say("nothing at all. The chip is not talking on GP9, or it never "
                "left reset.\r\n");
        } else {
            say_u32(heard);
            say(" bytes\r\n");
        }
    }

    // The SYNC payload is fixed: two bytes the ROM looks for, 0x20120707 in
    // the order the protocol writes it, and thirty-two 0x55s. It is not data --
    // it is a pattern chosen to be unmistakable in a stream that may still hold
    // the tail of a boot message.
    uint8_t payload[36];
    payload[0] = 0x07; payload[1] = 0x07; payload[2] = 0x12; payload[3] = 0x20;
    for (int i = 4; i < 36; i++) payload[i] = 0x55;

    if (!quiet) say("\r\nSYNC");
    // The ROM answers one SYNC several times over; one good frame is the answer
    // and the rest is noise to leave alone. Sent more than once because the
    // first often lands while the ROM is still settling.
    for (int attempt = 0; attempt < 6; attempt++) {
        if (!quiet) say(".");
        send_command(dev, CMD_SYNC, payload, sizeof(payload), 0, 0, 0);

        uint8_t frame[64];
        int32_t n = read_frame(dev, frame, sizeof(frame), 200);
        if (n < 8) continue;
        if (frame[0] != 0x01 || frame[1] != CMD_SYNC) continue;

        if (!quiet) {
            say("\r\nsynchronised: ");
            say_u32((uint32_t)n);
            say(" bytes back,");
            for (int32_t i = 0; i < n && i < 12; i++) { say(" "); say_hex(frame[i]); }
            say("\r\n");
        }
        // Whatever else the ROM sends after a SYNC, before the next command
        // goes out and its reply is looked for.
        myrtos_sleep(50);
        drain(dev);
        return true;
    }

    if (!quiet) say("\r\n");
    say("espflash: the ROM did not synchronise\r\n");
    into_application(dev);
    return false;
}

// Send one command and wait for the answer to it. The ROM echoes the command
// byte, and anything else on the wire -- a late SYNC reply, the tail of a boot
// message -- is skipped rather than mistaken for a failure.
//
// The payload of a reply IS its status: the last two bytes are the code and
// the reason, and zero is success. Taking them from the end is what makes this
// work on chips that answer with two and chips that answer with four.
// Why the last command was refused, for the caller to say out loud. The status
// pair is code then reason, and the reason is only meaningful when the code is
// not zero.
static uint8_t last_error;

static bool command(int32_t dev, uint8_t cmd, const uint8_t *head, uint16_t hlen,
                    const uint8_t *body, uint32_t blen, uint32_t checksum,
                    uint32_t ms) {
    send_command(dev, cmd, head, hlen, body, blen, checksum);

    for (int tries = 0; tries < 8; tries++) {
        uint8_t frame[64];
        int32_t n = read_frame(dev, frame, sizeof(frame), ms);
        if (n < 10) return false;                   // nothing, or too short to be one
        if (frame[0] != 0x01 || frame[1] != cmd) continue;
        last_error = frame[n - 1];
        return frame[n - 2] == 0;
    }
    return false;
}

// One byte of trouble, spelled out. The ROM's own numbering.
static const char *why(uint8_t code) {
    switch (code) {
    case 0x05: return "the ROM says the message was bad";
    case 0x06: return "the ROM does not know that command";
    case 0x07: return "the checksum did not match";
    case 0x08: return "the packet was the wrong size";
    case 0x09: return "the ROM could not keep up";
    default:   return "the ROM refused it";
    }
}

// Everything before the first block: which pins the flash is on, how big it is,
// and how much to erase. The ROM needs all three, and none of them can be asked
// of the chip from here -- FLASH_SIZE is this board's module, written down.
static bool prepare(int32_t dev, uint32_t size) {
    uint8_t attach[8] = { 0 };                      // 0 = the default SPI pins
    if (!command(dev, CMD_SPI_ATTACH, attach, sizeof(attach), 0, 0, 0, 500)) {
        say("espflash: the ROM would not attach to its flash\r\n");
        return false;
    }

    uint8_t params[24];
    put_le32(params +  0, 0);                       // flash id, which it works out
    put_le32(params +  4, FLASH_SIZE);
    put_le32(params +  8, 64 * 1024);               // block
    put_le32(params + 12, 4 * 1024);                // sector
    put_le32(params + 16, 256);                     // page
    put_le32(params + 20, 0xffff);                  // status mask
    if (!command(dev, CMD_SPI_PARAMS, params, sizeof(params), 0, 0, 0, 500)) {
        say("espflash: the ROM would not take the flash parameters\r\n");
        return false;
    }

    uint32_t blocks = (size + FLASH_BLOCK - 1) / FLASH_BLOCK;

    // Twenty bytes, not sixteen. The ESP32-S2 and everything after it -- this
    // chip included -- expect a fifth word saying the write is not encrypted,
    // but ONLY when there is no stub loader in the way. Sixteen bytes here gets
    // "the packet was the wrong size" and nothing else to go on.
    uint8_t begin[20];
    put_le32(begin +  0, size);                     // erase this much
    put_le32(begin +  4, blocks);
    put_le32(begin +  8, FLASH_BLOCK);
    put_le32(begin + 12, 0);                        // at offset zero
    put_le32(begin + 16, 0);                        // not encrypted

    // The erase happens inside this one command and it is the long wait of the
    // whole business: a megabyte of flash does not go quickly.
    say("erasing ");
    say_u32(size / 1024);
    say(" kB");
    if (!command(dev, CMD_FLASH_BEGIN, begin, sizeof(begin), 0, 0, 0, 60000)) {
        say("\r\nespflash: the ROM would not begin\r\n");
        return false;
    }
    say(" -- done\r\n");
    return true;
}

static void do_write(int32_t dev, const char *path) {
    int32_t f = myrtos_open_flags(path, MYRTOS_O_RDONLY);
    if (f < 0) {
        say("espflash: cannot read ");
        say(path);
        say("\r\n");
        return;
    }

    uint32_t size = 0;
    if (myrtos_fs_stat(path, &size) < 0 || !size) {
        say("espflash: that file has no length\r\n");
        myrtos_close(f);
        return;
    }

    say("writing ");
    say(path);
    say(", ");
    say_u32(size);
    say(" bytes, to the C6 at offset 0\r\n");
    say("This replaces NINA. 'wifi' stops working until it is put back.\r\n\r\n");

    if (!sync_rom(dev, true)) {
        myrtos_close(f);
        return;
    }
    if (!prepare(dev, size)) {
        myrtos_close(f);
        into_application(dev);
        return;
    }

    uint8_t block[FLASH_BLOCK];
    uint8_t head[16];
    uint32_t blocks = (size + FLASH_BLOCK - 1) / FLASH_BLOCK;
    uint32_t done = 0;

    for (uint32_t seq = 0; seq < blocks; seq++) {
        int32_t got = myrtos_read(f, block, FLASH_BLOCK);
        if (got <= 0) {
            say("\r\nespflash: the file ended early\r\n");
            break;
        }
        // Every block is a full block. The ROM was told how many kilobytes to
        // expect and will not take a short one, so the last is padded with the
        // erased value rather than with zeroes.
        for (int32_t i = got; i < (int32_t)FLASH_BLOCK; i++) block[i] = 0xff;

        uint32_t sum = 0xef;
        for (uint32_t i = 0; i < FLASH_BLOCK; i++) sum ^= block[i];

        put_le32(head +  0, FLASH_BLOCK);
        put_le32(head +  4, seq);
        put_le32(head +  8, 0);
        put_le32(head + 12, 0);

        if (!command(dev, CMD_FLASH_DATA, head, sizeof(head),
                     block, FLASH_BLOCK, sum, 3000)) {
            say("\r\nespflash: block ");
            say_u32(seq);
            say(" was not taken -- ");
            say(why(last_error));
            say("\r\nThe chip now holds neither firmware. Adafruit's "
                "SerialESPPassthrough UF2 is the way back.\r\n");
            myrtos_close(f);
            return;
        }

        done += (uint32_t)got;
        if ((seq % 64) == 0) {
            say("\r  ");
            say_u32(done / 1024);
            say(" of ");
            say_u32(size / 1024);
            say(" kB");
        }
    }
    myrtos_close(f);

    say("\r  ");
    say_u32(size / 1024);
    say(" of ");
    say_u32(size / 1024);
    say(" kB\r\n");

    // Told not to reboot itself, so that the reset below is the one that
    // decides -- and so that a chip which fails to start is a chip we reset on
    // purpose rather than one that vanished.
    uint8_t fin[4];
    put_le32(fin, 1);
    command(dev, CMD_FLASH_END, fin, sizeof(fin), 0, 0, 0, 2000);

    say("\r\nwritten. Starting it.\r\n\r\n");
    into_application(dev);
    myrtos_sleep(200);

    say("--- what it says ---------------------------------------------\r\n");
    uint32_t heard = listen_or_discard(dev, 1500, false);
    say("\r\n--------------------------------------------------------------\r\n");
    if (!heard) say("nothing. It did not start, or it does not talk on this pin.\r\n");
}

static void do_sync(int32_t dev) {
    say("resetting the C6 into its serial bootloader\r\n");
    say("(this drops the WiFi link, and the audio DAC shares the reset pin)\r\n\r\n");

    if (!sync_rom(dev, false)) {
        say("the chip has been reset back into its application\r\n");
        return;
    }

    say("\r\nThe ROM loader is listening. The wire, the strap and the reset "
        "all work.\r\n");
    say("putting the chip back the way it was\r\n");
    into_application(dev);
    say("done -- 'wifi connect' will find NINA again\r\n");
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: espflash sync | espflash write FILE\n\n"
            "Talks to the ESP32-C6's ROM loader over /dev/esp.\n\n"
            "  sync         reset the chip into its serial bootloader and prove\n"
            "               the wire. Reads only; the chip comes back as it was.\n"
            "  write FILE   write FILE to the chip's flash from offset 0. This\n"
            "               REPLACES what is in it -- NINA, and with it every\n"
            "               'wifi' command -- and takes minutes at 115200 baud.\n\n"
            "Either way the WiFi link goes down, and the audio DAC is reset too,\n"
            "since it shares the reset pin on this board.\n")) return;

    bool sync  = argc == 2 && is(argv[1], "sync");
    bool write = argc == 3 && is(argv[1], "write");
    if (!sync && !write) {
        say("usage: espflash sync | espflash write FILE\r\n");
        return;
    }

    int32_t dev = myrtos_open(ESP_DEV);
    if (dev < 0) {
        say("espflash: no " ESP_DEV ". Is the esplink driver loaded?\r\n");
        return;
    }

    if (sync) do_sync(dev);
    else      do_write(dev, argv[2]);
    myrtos_close(dev);
}

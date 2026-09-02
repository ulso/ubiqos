#include "sdcard.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define SD_SPI       spi0
#define SD_SCK_PIN   34
#define SD_MOSI_PIN  35
#define SD_MISO_PIN  36
#define SD_CS_PIN    39
// GP33 is card detect according to Adafruit's own board header
// (ADAFRUIT_FRUIT_JAM_SD_CARD_DETECT_PIN), and on this board nothing drives it.
// Measured 2 Sep 2026 with a card in the slot that mounted over four-bit SDIO
// in the next breath: with a pull-up GP33 reads 1, with a pull-down it reads 0.
// It follows the pull, which is what an unconnected pin does.
//
// There was a myrtos_sd_present() here that read it and returned "empty". It is
// gone rather than left for someone to trust: gating the mount on it stopped
// the machine reading a card that was plainly there, and a function that
// answers wrongly is worse than no function.
//
// So a removed card has to be noticed by the card no longer answering, not by
// asking the slot. That is the more robust test anyway -- a card can stop
// answering without being pulled.
#define SD_DETECT_PIN 33      // defined for the record; nothing reads it

#define CMD0_GO_IDLE          0
#define CMD8_SEND_IF_COND     8
#define CMD17_READ_SINGLE    17
#define CMD24_WRITE_SINGLE   24
#define CMD55_APP            55
#define CMD58_READ_OCR       58
#define ACMD41_SEND_OP_COND  41

#define R1_IDLE 0x01

// Cards over 2 GB are addressed in blocks, smaller ones in bytes. CMD58 says which.
static bool sd_block_addressed;

static void cs_low(void)  { gpio_put(SD_CS_PIN, 0); }
static void cs_high(void) { gpio_put(SD_CS_PIN, 1); }

static uint8_t sd_xfer(uint8_t out) {
    uint8_t in = 0xff;
    spi_write_read_blocking(SD_SPI, &out, &in, 1);
    return in;
}

// The card does not answer at once; it holds MISO high until it is ready.
static uint8_t sd_wait_response(void) {
    for (int i = 0; i < 8192; i++) {
        uint8_t r = sd_xfer(0xff);
        if (!(r & 0x80)) return r;
    }
    return 0xff;
}

static uint8_t sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_xfer(0xff);
    sd_xfer(0x40 | cmd);
    sd_xfer((uint8_t)(arg >> 24));
    sd_xfer((uint8_t)(arg >> 16));
    sd_xfer((uint8_t)(arg >> 8));
    sd_xfer((uint8_t)arg);
    sd_xfer(crc);          // CRC is only required for CMD0 and CMD8
    return sd_wait_response();
}

static bool spi_init_card(void) {
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    cs_high();
    gpio_init(SD_DETECT_PIN);
    gpio_set_dir(SD_DETECT_PIN, GPIO_IN);
    gpio_pull_up(SD_DETECT_PIN);

    // Initialisation has to be slow: the standard allows at most 400 kHz
    // before the card has said what it can take.
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);

    // At least 74 clock pulses with CS high before the card listens.
    cs_high();
    for (int i = 0; i < 10; i++) sd_xfer(0xff);

    cs_low();
    uint8_t r = 0xff;
    for (int i = 0; i < 16 && r != R1_IDLE; i++) {
        r = sd_command(CMD0_GO_IDLE, 0, 0x95);
    }
    if (r != R1_IDLE) { cs_high(); myrtos_print("SD: no response to CMD0\n"); return false; }

    // CMD8 separates modern cards (v2) from old ones. 0x1AA = 2.7-3.6 V, pattern AA.
    r = sd_command(CMD8_SEND_IF_COND, 0x1aa, 0x87);
    if (r & ~R1_IDLE) { cs_high(); myrtos_print("SD: card too old (no CMD8)\n"); return false; }
    for (int i = 0; i < 4; i++) sd_xfer(0xff);   // resten av R7

    // ACMD41 with the HCS bit: ask the card to leave idle, and say that we can
    // handle high-capacity cards.
    absolute_time_t deadline = make_timeout_time_ms(1000);
    do {
        sd_command(CMD55_APP, 0, 0xff);
        r = sd_command(ACMD41_SEND_OP_COND, 0x40000000, 0xff);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            cs_high(); myrtos_print("SD: timed out leaving idle\n"); return false;
        }
    } while (r != 0);

    r = sd_command(CMD58_READ_OCR, 0, 0xff);
    if (r) { cs_high(); myrtos_print("SD: CMD58 failed\n"); return false; }
    uint8_t ocr0 = sd_xfer(0xff);
    for (int i = 0; i < 3; i++) sd_xfer(0xff);
    sd_block_addressed = (ocr0 & 0x40) != 0;   // CCS

    cs_high();
    sd_xfer(0xff);

    // Now bring the speed up.
    spi_set_baudrate(SD_SPI, 12 * 1000 * 1000);

    myrtos_print("SD: card ready, ");
    myrtos_print(sd_block_addressed ? "block addressed (SDHC/SDXC)\n" : "byte addressed (SDSC)\n");
    return true;
}

static bool spi_read_block(uint32_t lba, uint8_t *buf) {
    uint32_t addr = sd_block_addressed ? lba : lba * 512u;

    cs_low();
    if (sd_command(CMD17_READ_SINGLE, addr, 0xff) != 0) {
        cs_high();
        return false;
    }
    // The card sends 0xFE when the data block begins.
    uint8_t token = 0xff;
    for (int i = 0; i < 20000 && token == 0xff; i++) token = sd_xfer(0xff);
    if (token != 0xfe) { cs_high(); return false; }

    for (int i = 0; i < 512; i++) buf[i] = sd_xfer(0xff);
    sd_xfer(0xff);      // CRC, which we do not check in SPI mode
    sd_xfer(0xff);

    cs_high();
    sd_xfer(0xff);
    return true;
}

static bool spi_write_block(uint32_t lba, const uint8_t *buf) {
    uint32_t addr = sd_block_addressed ? lba : lba * 512u;

    cs_low();
    if (sd_command(CMD24_WRITE_SINGLE, addr, 0xff) != 0) {
        cs_high();
        return false;
    }

    sd_xfer(0xff);          // one idle byte before the token, as the spec asks
    sd_xfer(0xfe);          // start of a single block
    for (int i = 0; i < 512; i++) sd_xfer(buf[i]);
    sd_xfer(0xff);          // CRC, ignored in SPI mode but still clocked out
    sd_xfer(0xff);

    // The card answers with a data response token. Only the low five bits mean
    // anything, and 0b00101 is the one that says it took the block.
    uint8_t resp = 0xff;
    for (int i = 0; i < 20000 && (resp & 0x11) != 0x01; i++) resp = sd_xfer(0xff);
    if ((resp & 0x1f) != 0x05) { cs_high(); return false; }

    // Then it holds MISO low for as long as the write takes. Returning before it
    // lets go would put the next command into a card that is still busy.
    for (int i = 0; i < 500000; i++) {
        if (sd_xfer(0xff) != 0x00) {
            cs_high();
            sd_xfer(0xff);
            return true;
        }
    }
    cs_high();
    return false;                       // still busy: treat as a failed write
}


// --- SDIO ------------------------------------------------------------------
// The card has four data lines on this board -- GP36 to GP39, clock on 34 and
// command on 35 -- and the board header names them, so the vendored driver's own
// defaults are right. Four bits at a time instead of one.
//
// But it is NOT tried at startup, and that is deliberate. The first attempt hung
// before the console had drawn anything and before the USB task had run, so the
// board went dark and off the bus at once: nothing to look at, nothing to talk
// to, and only the BOOTSEL button left. A driver marked "prototyping level" does
// not belong in front of the two things that make a failure observable.
//
// So the board boots on SPI, which is known to work, and SDIO is asked for by
// the `mount` command. A hang there costs the filesystem server, which is a
// process like any other -- the shell, the screen and the keyboard carry on, and
// the machine can be looked at instead of just power-cycled.

#include "pico/sd_card.h"
#include "hardware/pio.h"

static bool use_sdio;

// A card that has stopped answering stays stopped until it is mounted again.
//
// Without this, one wedged transfer took the machine down. Every later read
// found the data state machine still stuck, gave up after its million spins and
// printed a line saying so -- and the filesystem walks a FAT chain a sector at a
// time, so one `ls` was hundreds of those. The screen filled, and the shell
// never ran again because the filesystem server sits at priority 22 above it.
// From outside the board looked bricked; it was answering diligently, hundreds
// of times, that it could not.
//
// One failure, one message, and every call after it returns false immediately.
// `mount` clears it, because a remount is exactly the moment to try again.
static bool sd_failed;

static bool sd_fail(const char *why) {
    if (!sd_failed) {
        sd_failed = true;
        myrtos_print("SD: ");
        myrtos_print(why);
        myrtos_print(" -- the card is offline until it is mounted again\n");
        // Once, and with it the state that says where it stopped. A failure
        // that only says "it failed" costs a power cycle to learn anything
        // from; this one is meant to be read afterwards in /var/dmesg. The
        // state machine and the channel numbers belong to the driver, so it
        // prints them.
        sd_dump_state();
    }
    return false;
}

// Writing over four-bit SDIO works, verified 1 Sep 2026: a 100000-byte copy
// read back byte for byte across all 196 sectors. Kept as a variable rather
// than an #if so it can be turned off in one line if a card ever misbehaves.
const bool myrtos_sd_sdio_writes_allowed = true;

// Once SPI has been spoken to the card, SDIO is not worth asking for again --
// see below. This says so, so that the mount command's retry does not hang.
static bool sdio_refused;

bool myrtos_sd_init(void) {
    use_sdio = false;
    sdio_refused = false;
    sd_failed = false;

    // SDIO first, and the order is the whole point. A card latches into SPI
    // mode the moment it is addressed that way and stays there until the power
    // is cut -- so asking for SDIO after spi_init_card has run is asking a card
    // that does not speak it any more.
    //
    // That is what was wrong, and it was measured rather than guessed. Halted on
    // the debugger while mount hung: PIO1's dbg_padout showed the clock
    // toggling, dbg_padoe showed CMD released to an input, every RX FIFO was
    // empty, and the command DMA sat with two words remaining and never moved.
    // The card was being clocked and asked, correctly, and said nothing at all
    // -- which is what a card in SPI mode does when spoken to in SDIO.
    // ...and it cannot be done here. This runs before the scheduler starts, so
    // before the USB process has ever run, and the driver is full of loops that
    // upstream itself marks "todo not forever" -- an unbounded wait on a FIFO at
    // 323 and 327, and the ACMD41 busy-wait at 645, which spins for ever if the
    // card does not answer. Trying it here cost a boot: no console, no USB, and
    // the BOOTSEL button the only way back.
    //
    // Removing __breakpoint() was not enough and could not be: the driver gives
    // up on the DMA and then hangs on the next loop instead. The place for this
    // is a process, where a hang costs one process. That is the next change.
    // The flag goes up only if SPI actually took. A failed attempt -- no card in
    // the socket at boot, say -- leaves the card untouched and still able to
    // speak SDIO when one is put in and mount is run.
    bool ok = spi_init_card();
    if (ok) sdio_refused = true;
    return ok;
}

// Try to move the card to four-bit SDIO. Returns false and leaves SPI in place
// if the card will not have it.
bool myrtos_sd_try_sdio(void) {
    if (use_sdio) return true;
    if (sdio_refused) return false;     // the card is in SPI mode now
    sd_failed = false;

    // The card is on GP34 to GP39, and on RP2350 one PIO reaches either GPIO
    // 0-31 or 16-47, never both. The default window is the low one, so without
    // this the state machines are configured for pins they cannot see: nothing
    // is driven, the card never answers, and the driver waits for ever. The
    // driver comes from the RP2040 world, where the question does not arise.
    pio_set_gpio_base(pio1, 16);

    if (sd_init_4pins() != SD_OK) return false;
    if (sd_set_wide_bus(true) != SD_OK) return false;
    use_sdio = true;
    return true;
}

bool myrtos_sd_is_sdio(void) { return use_sdio; }

// The driver takes words, so a caller's buffer has to be aligned. Everything
// that reaches here is a static 512-byte buffer in the kernel, declared aligned.
bool myrtos_sd_read_block(uint32_t lba, uint8_t *buf) {
    if (!use_sdio) return spi_read_block(lba, buf);
    if (sd_failed) return false;
    if ((uintptr_t)buf & 3u) return false;
    if (sd_readblocks_sync((uint32_t*)(void*)buf, lba, 1) != SD_OK)
        return sd_fail("a read did not complete");
    return true;
}

// Put the bus back to four bits after a write.
//
// sd_writeblocks_async calls sd_set_wide_bus(false) -- "use 1 bit writes for
// now", says the comment upstream -- and never puts it back. The write itself may well land, but the
// card and the driver are left one bit wide while use_sdio still says four, so
// the next READ asks four lines for data arriving on one, the data state
// machine never returns to waiting_for_cmd, and the bounded wait gives up.
// Every read after the first write, for ever. That is the flood that filled the
// screen, and the write was only where it started.
//
// So writes are one bit wide and slow, which is upstream's choice and fine, but
// the bus goes back to four the moment one finishes.
static void restore_wide_bus(void) {
    if (use_sdio) sd_set_wide_bus(true);
}

// Writing over four-bit SDIO, which took three wrong answers to get right.
//
// The first write ever attempted wedged the card, and each of these looked like
// the cause and was not: the bus left one bit wide after a write (real, fixed),
// the driver's debug output in the polling loop (real, fixed), and the data
// state machine not parked where the write path asserts it is -- an assert that
// -DNDEBUG deletes, so the code proceeded on an assumption nothing checked.
//
// What it actually was: after the data lands, the card spends time programming
// and answers nothing until it is done. Restoring the bus width during that
// window sends ACMD6 to a card that will not take a command, and it never came
// back. The board said so plainly once there was a diagnostic to say it with --
// both DMA channels idle, transfer finished, card still busy after half a
// second. Wait for the card first, reconfigure afterwards.
//
// Upstream guessed at this from the other end and left the note in
// sd_writeblocks_async: "probably need a delay between sectors". It is not a
// delay, it is the card's own answer to CMD13.
bool myrtos_sd_write_block(uint32_t lba, const uint8_t *buf) {
    if (!use_sdio) return spi_write_block(lba, buf);
    if ((uintptr_t)buf & 3u) return false;
    if (!myrtos_sd_sdio_writes_allowed) return false;
    if (sd_failed) return false;

    if (sd_writeblocks_async((const uint32_t*)(const void*)buf, lba, 1) != SD_OK) {
        restore_wide_bus();
        return sd_fail("a write was refused before it started");
    }

    // Bounded, for the reason above. The driver's own waits give up and return,
    // and this one has to as well -- otherwise it simply calls them again.
    int status = SD_OK;
    uint32_t spins = 0;
    while (!sd_write_complete(&status)) {
        if (++spins > 1000000u) {
            restore_wide_bus();
            return sd_fail("a write did not complete, and a block may be torn");
        }
    }
    if (status != SD_OK) { restore_wide_bus(); return false; }

    // Wait for the card FIRST, and only then touch the bus width. The other
    // order was mine and it was wrong: restore_wide_bus sends ACMD6, and a card
    // that is still programming will not take a command. So the sequence was
    // write, reconfigure a busy card, then ask why it never came back -- and it
    // never did, which is exactly what the board reported: both DMA channels
    // idle, the transfer finished, the card still busy after half a second.
    //
    // Upstream's own note two functions down guessed at this from the other
    // end: "probably need a delay between sectors". The delay is not a guess
    // here, it is the card's own answer to CMD13.
    if (sd_wait_not_busy(500) != SD_OK) {
        restore_wide_bus();
        return sd_fail("the card stayed busy for half a second after a write");
    }
    restore_wide_bus();
    return true;
}

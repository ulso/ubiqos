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
// DMA out of a ring, so playing something costs the processor a copy rather
// than its whole attention. The ring is SRAM asked for from the kernel and not
// a buffer of this module's own: a module lives in PSRAM, which DMA cannot
// reach -- the same wall sdlib hit, and the same way round it.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "audio_i2s.pio.h"

#define I2S_DIN    24
#define I2S_BCLK   26        // and WS on 27, which must be BCLK + 1
#define I2S_WS     27
#define I2S_PIO    pio2
#define I2S_SM     1         // 0 is the neopixel's

// 48 kHz, and the number is not arbitrary: sys is 120 MHz here, and
// 120e6 * 4 / 48000 is exactly 10000, so the 8.8 divider comes out at 39.0625
// with no rounding at all. 44100 does not divide as kindly.
// 48000, and the fractional PIO divider that comes with it is a known and
// measured non-problem.
//
// The divider is 8.8 fixed point and a frame is 64 PIO cycles, so 48 kHz at
// 120 MHz needs 39.0625. The average rate is exact; the individual bit-clock
// edges are quantised to the system clock, which is jitter, and jitter on the
// sample instants is a phase error proportional to signal frequency -- the
// same shape as the distortion this device actually has.
//
// So it was tried: 46875 Hz gives a divider of exactly 40, and the DAC's clock
// chain follows it with nothing changed (BCLK 1.5 MHz, PLL J=64 to 96 MHz,
// NDAC=8 MDAC=2 DOSR=128 dividing to exactly 46875). Measured at the
// headphone jack with an integer divider, the distortion was unchanged to
// within the measurement: the sideband on a 10 kHz tone stayed at -15 dB.
//
// Left at 48000 because that is the rate most files already have, and at that
// rate they are played rather than resampled. The jitter is real and is not
// what anybody is hearing.
//
// Changing the rate did settle something else, though. The distortion this
// device has is an amplitude modulation by a disturbance at a FIXED 68176 Hz
// which the DAC's sampling folds down to |68176 - fs|, and the sidebands land
// there plus and minus the tone. Measured at three rates:
//
//     fs 48000 -> sidebands about 20172      68172
//     fs 46875 -> sidebands about 21305      68180
//     fs 44100 -> sidebands about 24078      68178
//
// Constant to four hertz across nearly four kilohertz of fs, and the third was
// a prediction before it was a measurement: 24076 was calculated and 24078
// came back. That is why the fault gets worse with pitch and not why anyone
// would guess -- at 1 kHz the sidebands sit at 19 and 21 kHz where nobody
// hears them, and at 10 kHz one of them lands at 14 kHz. The source of the
// 68176 Hz is not known. See the README.
#define I2S_HZ     48000

static const myrtos_kernel_api_t *K;
static bool ready;

// --- THE RING -------------------------------------------------------------
// The DMA reads it and wraps for ever; this side writes ahead of where the DMA
// has got to. There is no interrupt and no end: the transfer count is set to
// the largest there is, which at 48000 words a second runs for a day, and the
// hardware's own address wrapping keeps it inside the buffer.
//
// The wrap is why the size is a power of two AND why the buffer has to be
// aligned to its own size -- the DMA masks the low bits of the read address
// rather than comparing against a limit. mem_alloc makes no promises about
// alignment, so twice the ring is asked for and the aligned part used.
//
// 2048 bytes is 512 stereo frames, which is 10.7 ms at 48 kHz. Enough that a
// writer scheduled a few milliseconds late does not run dry, and small enough
// that four kilobytes of SRAM is a fair price for a sound device.
//
// Silence is written AHEAD of the data, which is what stops the ring becoming
// a loop pedal. The DMA never ends -- there is no interrupt and no stopping it
// -- so whatever it finds, it plays; and what it found, until this was fixed,
// was the last ten milliseconds of the tone, for ever. "A buzz is a better
// symptom than silence" is what the comment here used to say, and it was wrong
// in the way that only becomes obvious with headphones on: a device that never
// goes quiet is not a device.
//
// So every write also clears the space in front of it. The cost is writing the
// ring twice, which at 48 kHz stereo is 384 kB/s of memory traffic and beneath
// notice, and it means a writer that stops -- or falls behind, or is killed --
// runs into silence rather than into its own past.
// 4096, which is 1024 stereo frames and so 21 ms of audio at 48 kHz. It was
// 2048, and 10 ms turned out to be less slack than a player needs: a read off
// the SD card is a message to the file server and a FAT walk behind it, and
// when one of those took longer than the ring had left, the ring ran dry and
// it was audible as a stutter. The cost is SRAM and it is paid twice -- the
// DMA's read-address wrapping needs the buffer aligned to its own size, and
// the only way to get that out of a general allocator is to ask for double and
// align up inside it.
#define RING_BYTES 4096u
#define RING_WORDS (RING_BYTES / 4u)
#define RING_BITS  12u          // 2^12 = RING_BYTES, which the DMA masks with

static volatile uint32_t *ring;
static int32_t dma_ch = -1;
static uint32_t write_at;       // in words, where this side has got to

// Where the DMA is reading, as a word index into the ring. Read from the
// channel's live address rather than counted, because counting would have to
// be kept in step with hardware that never stops.
static uint32_t dma_at(void)
{
    uintptr_t addr = (uintptr_t)dma_channel_hw_addr((uint)dma_ch)->read_addr;
    return (uint32_t)((addr - (uintptr_t)ring) / 4u) & (RING_WORDS - 1u);
}

// Words this side may still write without overtaking the reader. One is kept
// back so that full and empty are not the same arrangement.
static uint32_t ring_free(void)
{
    return (dma_at() - write_at - 1u) & (RING_WORDS - 1u);
}

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
    // -35 dB, in half-decibel steps of a signed byte, and NOT 0 dB. Nought is
    // full scale, which is what this was, and the first tone anybody heard
    // through headphones was too loud. A device somebody puts on their head
    // should not come up at maximum, and this is the level Ulf picked by ear
    // out of three. `volume` moves these two; keep it and vol_now agreeing.
    { 0, 0x41, 0xba,  0 },
    { 0, 0x42, 0xba,  0 },

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

// --- STATUS ---------------------------------------------------------------
// The volume lives here now rather than in a command that wrote the codec's
// registers over /dev/i2c. That the chip is a TLV320 at 0x18, and that its
// volume is two registers in half-decibel steps, is this file's business and
// nobody else's -- which is the whole reason getstat and setstat exist.

#define DAC_VOL_L  0x41
#define DAC_VOL_R  0x42

// 0-100 onto the register's signed half-decibels: 100 is 0 dB, which is full
// scale, and 0 is -50 dB. Not silence -- muting is a different register and a
// different question, and a volume of nought that is merely very quiet is
// easier to recover from than one that looks like broken hardware.
static uint8_t vol_now = 100u - 70u;    // what dac_init writes: -35 dB

static bool dac_set(uint8_t page, uint8_t reg, uint8_t val)
{
    uint8_t sel[2] = { 0x00, page };
    if (K->i2c_write(K->i2c, DAC_ADDR, sel, 2, false) < 0) return false;
    uint8_t w[2] = { reg, val };
    return K->i2c_write(K->i2c, DAC_ADDR, w, 2, false) >= 0;
}

static int32_t audio_getstat(uint32_t code, void *data, uint32_t len)
{
    // The ring is the exception to the four-byte rule below: it is as big as
    // it is, and a caller that asks for it with the wrong size is refused
    // rather than given part of it.
    if (code == MYRTOS_SS_RINGDUMP) {
        if (!ready || !data || len != RING_BYTES) return -1;
        uint32_t at = dma_at();
        uint32_t *out32 = (uint32_t *)data;
        for (uint32_t i = 0; i < RING_WORDS; i++)
            out32[i] = ring[(at + i) & (RING_WORDS - 1u)];
        return 0;
    }
    if (!data || len != 4) return -1;
    switch (code) {
    case MYRTOS_SS_VOLUME: *(uint32_t *)data = vol_now; return 0;
    // Fixed, and not by choice: the PIO divider is an integer at this system
    // clock and 48000 is the rate that comes out exact. Worth answering all
    // the same -- a player has to know whether the file it holds can be
    // played at all, and the honest answer to that is a number.
    case MYRTOS_SS_RATE:   *(uint32_t *)data = I2S_HZ; return 0;
    default:               return -1;
    }
}

static int32_t audio_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (!data || len != 4) return -1;
    if (code != MYRTOS_SS_VOLUME) return -1;
    uint32_t v = *(const uint32_t *)data;
    if (v > 100u) return -1;
    uint8_t reg = (uint8_t)(int8_t)((int32_t)v - 100);
    if (!dac_set(0, DAC_VOL_L, reg) || !dac_set(0, DAC_VOL_R, reg)) return -1;
    vol_now = (uint8_t)v;
    return 0;
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

    // The ring, and the DMA that empties it into the state machine's FIFO.
    // Twice the size is asked for because mem_alloc promises nothing about
    // alignment and the hardware's wrapping demands it.
    // Two failures, two messages. "no SRAM or no DMA channel" was one message
    // for two causes, and it sent the reader to weigh both when the machine
    // knew perfectly well which.
    // driver_alloc and not mem_alloc: this runs at boot in kernel context,
    // where mem_alloc refuses on purpose because it hands out memory OWNED by
    // a process. A driver's ring belongs to the driver and lives as long as
    // the machine. See the note beside driver_alloc in the ABI.
    uint8_t *raw = (uint8_t *)K->driver_alloc(RING_BYTES * 2u);
    if (!raw) {
        K->print("  audio driver: no SRAM for the ring\n");
        return -1;
    }
    dma_ch = K->dma_claim_channel();
    if (dma_ch < 0) {
        K->print("  audio driver: every DMA channel is taken\n");
        return -1;
    }
    ring = (volatile uint32_t *)(((uintptr_t)raw + (RING_BYTES - 1u)) & ~(uintptr_t)(RING_BYTES - 1u));
    for (uint32_t i = 0; i < RING_WORDS; i++) ring[i] = 0;

    dma_channel_config_t dc = dma_channel_get_default_config((uint)dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    // The read address wraps inside the ring; the write address is one FIFO
    // register and never moves.
    channel_config_set_ring(&dc, false, RING_BITS);
    // Paced by the state machine: a word leaves only when the FIFO has room,
    // which is what makes this play at 48 kHz rather than as fast as memory.
    channel_config_set_dreq(&dc, pio_get_dreq(I2S_PIO, I2S_SM, true));

    dma_channel_configure((uint)dma_ch, &dc,
                          &I2S_PIO->txf[I2S_SM],   // to the FIFO
                          (const void *)ring,      // from the ring
                          0xffffffffu,             // effectively never ending
                          true);                   // and start now

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

// Closing means silence, and it has to be said rather than assumed. The DMA
// never stops -- there is no interrupt and nothing to stop it with -- so it
// keeps reading the ring for ever. Clearing ahead of the writer is not enough
// on its own: once the reader has gone all the way round through that silence
// it arrives back at the data it played before, and the result is the tone
// coming and going, which sounds like interference rather than like a loop.
//
// So the whole ring goes to zero here. It costs up to the last ten
// milliseconds of whatever was playing, which nobody can hear, and it means
// the machine is quiet when nothing is using it -- which turns out to matter
// more than a tidy tail when the thing is on somebody's head.
static int32_t audio_close(void)
{
    if (ready) for (uint32_t i = 0; i < RING_WORDS; i++) ring[i] = 0;
    write_at = 0;
    return 0;
}

// One 32-bit word is one stereo frame: left in the high half, right in the
// low, because the shift register goes out most significant bit first and I2S
// sends the left channel first.
//
// As much as fits and no more. This does not wait: a short answer is the
// contract for a device that can fill up, and the I/O manager parks the writer
// on WAIT_WRITE until writable() says there is room again. Waiting here would
// be waiting inside a system call, which on this machine is how a driver
// starves everything else -- see the note in myrtos_long_syscalls.
static int32_t audio_write(const uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;

    uint32_t want = len / 4u;
    uint32_t room = ring_free();
    uint32_t n = want < room ? want : room;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = (uint32_t)buf[i * 4 + 0] | ((uint32_t)buf[i * 4 + 1] << 8);
        uint32_t r = (uint32_t)buf[i * 4 + 2] | ((uint32_t)buf[i * 4 + 3] << 8);
        ring[write_at] = (l << 16) | (r & 0xffffu);
        write_at = (write_at + 1u) & (RING_WORDS - 1u);
    }

    // And clear what lies in front, as far as there is room. Only ever space
    // the reader has already passed, so nothing waiting to be played is lost.
    uint32_t ahead = ring_free();
    for (uint32_t i = 0, at = write_at; i < ahead; i++, at = (at + 1u) & (RING_WORDS - 1u))
        ring[at] = 0;

    return (int32_t)(n * 4u);
}

// Room in the ring, in bytes. A caller that asks before writing never blocks;
// one that does not, blocks in the I/O manager rather than in here.
static int32_t audio_writable(void)
{
    return ready ? (int32_t)(ring_free() * 4u) : 0;
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
        .getstat = audio_getstat, .setstat = audio_setstat,
    },
};

// --- WHAT THIS IS NOT -----------------------------------------------------
// The rate cannot be set, only asked. Changing it means a new PIO divider and
// a new DAC clock chain -- NDAC, MDAC and DOSR all follow from it -- and only
// the rates that come out exact at this system clock could be offered. That is
// real work rather than another status code, and nothing wants it yet: a WAV
// player can ask what the rate is and resample or refuse.

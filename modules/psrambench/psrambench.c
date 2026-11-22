// psrambench -- how fast DMA can read PSRAM, three ways.
//
//   psrambench          1 MB each way, a DMA channel the caller names
//   psrambench KB CH    KB kilobytes, DMA channel CH
//
// The question it answers is whether a 640x480 picture can be kept in PSRAM
// and copied a band at a time into SRAM scanlines: that wants about 19 MB/s
// sustained during the active lines, with the rest of the system running.
//
//   cached     DMA reads the ordinary address, through the XIP cache
//   uncached   DMA reads the no-cache, no-allocate alias
//   stream     the XIP streaming FIFO reads ahead, DMA empties it
//
// Each copies into a single SRAM word (no write increment), so the number is
// the read side alone. The data is written through the uncached alias first,
// so the stream and uncached reads are checked against what is really in the
// chip rather than what the cache holds.
//
// THE KB MODE IS ONE LONG UNPACED BURST, and run over and over on 21 Nov 2026
// it upset the WiFi link: 51188 bad frame headers on the SPI to the C6, WiFi
// lost until a reboot. It also showed a few dozen wrong words in its first
// runs and none in the sixteen after, which is not explained. `lines` is the
// realistic one -- paced bands -- and 5.7 million lines of it showed no wrong
// word, no link error, no lost ping and no display underrun.
//
// A program has no way to claim a DMA channel, so it is given one: check that
// nothing uses it (its registers all zero) before running this. On the Fruit
// Jam on 21 Nov 2026 channels 12 to 15 had never been touched.
#include "../../common/ubiqos_abi.h"

#define REG(a)          (*(volatile uint32_t *)(uintptr_t)(a))
#define TIMERAWL        REG(0x400b0028u)
#define DMA_CH(c, off)  REG(0x50000000u + (c) * 0x40u + (off))
#define XIP_STREAM_ADDR REG(0x400c8000u + 0x14u)
#define XIP_STREAM_CTR  REG(0x400c8000u + 0x18u)
#define XIP_AUX_STREAM  0x50500000u

#define CTRL_EN         (1u << 0)
#define CTRL_HIGH       (1u << 1)
#define CTRL_WORD       (2u << 2)
#define CTRL_INCR_READ  (1u << 4)
#define CTRL_CHAIN(c)   ((uint32_t)(c) << 13)
#define CTRL_TREQ(t)    ((uint32_t)(t) << 17)
#define CTRL_BUSY       (1u << 26)
#define TREQ_FORCE      0x3fu
#define DREQ_XIP_STREAM 49u

#define NOCACHE_OFFSET  0x04000000u   // 0x11000000 -> 0x15000000

static void say_rate(const char *what, uint32_t bytes, uint32_t us, uint32_t bad) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, what);
    ubiqos_line_u32(&l, us);
    ubiqos_line_str(&l, " us, ");
    // MB/s to one decimal: bytes / us is MB/s.
    // 32 bits: four megabytes times ten fits, and a program has no libgcc
    // for a 64-bit division.
    const uint32_t tenths = (bytes * 10u) / (us ? us : 1u);
    ubiqos_line_u32(&l, tenths / 10u);
    ubiqos_line_str(&l, ".");
    ubiqos_line_u32(&l, tenths % 10u);
    ubiqos_line_str(&l, " MB/s");
    if (bad != 0xffffffffu) {
        ubiqos_line_str(&l, ", wrong words ");
        ubiqos_line_u32(&l, bad);
    }
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

static uint32_t num(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return v;
}

// One DMA run, words from src into *dst, paced by treq. Returns microseconds.
static uint32_t run(uint32_t ch, uint32_t src, bool incr, uint32_t treq,
                    volatile uint32_t *dst, uint32_t words) {
    DMA_CH(ch, 0x0C) = 0;                                   // stop, unconfigure
    DMA_CH(ch, 0x00) = src;
    DMA_CH(ch, 0x04) = (uint32_t)(uintptr_t)dst;
    DMA_CH(ch, 0x08) = words;
    const uint32_t t0 = TIMERAWL;
    DMA_CH(ch, 0x0C) = CTRL_EN | CTRL_WORD | (incr ? CTRL_INCR_READ : 0u) |
                       CTRL_CHAIN(ch) | CTRL_TREQ(treq);
    while (DMA_CH(ch, 0x0C) & CTRL_BUSY) { }
    return TIMERAWL - t0;
}

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void invalidate(uint32_t cached, uint32_t bytes) {
    for (uint32_t off = cached - 0x10000000u; off < cached - 0x10000000u + bytes; off += 8u)
        *(volatile uint8_t *)(uintptr_t)(0x18000000u + off + 2u) = 0;   // invalidate by address
#ifdef __riscv
    __asm volatile ("fence rw, rw; fence.i" ::: "memory");
#else
    __asm volatile ("dsb; isb" ::: "memory");
#endif
}

// What a picture pump would do: a 640x480 frame in PSRAM, copied a band of
// sixteen lines at a time through the XIP stream into a band in SRAM, two
// bands a millisecond -- about the 30000 lines a second 640x480 is played
// at -- and every copied line checked. Small, paced copies rather than one
// long burst, because the long burst is what upset the WiFi link.
static void lines_test(uint32_t seconds, uint32_t ch) {
    const uint32_t W = 640u, H = 480u, BAND = 16u;
    const uint32_t frame = W * H, band_words = W * BAND / 4u;
    uint8_t *fb = (uint8_t *)ubiqos_alloc_bulk(frame);
    uint32_t *band = (uint32_t *)ubiqos_alloc(W * BAND);
    if (!fb || !band || ((uint32_t)(uintptr_t)fb >> 24) != 0x11u ||
        ((uint32_t)(uintptr_t)band >> 28) != 0x2u) {
        ubiqos_write_str(UBIQOS_STDOUT, "psrambench: no PSRAM frame or SRAM band\n");
        if (fb) ubiqos_free(fb);
        if (band) ubiqos_free(band);
        return;
    }
    const uint32_t cached = (uint32_t)(uintptr_t)fb;
    invalidate(cached, frame);
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(cached + NOCACHE_OFFSET);
    for (uint32_t i = 0; i < frame / 4u; i++) w[i] = i * 2654435761u;

    uint32_t bands = 0, bad = 0, dma_us = 0, worst = 0, shown = 0;
    const uint32_t start = TIMERAWL;
    uint32_t y = 0;
    while (TIMERAWL - start < seconds * 1000000u) {
        for (uint32_t k = 0; k < 2u; k++) {
            const uint32_t first = y * W / 4u;               // word index of the band
            XIP_STREAM_CTR = 0;
            XIP_STREAM_ADDR = cached + y * W;
            XIP_STREAM_CTR = band_words;
            DMA_CH(ch, 0x0C) = 0;
            DMA_CH(ch, 0x00) = XIP_AUX_STREAM;
            DMA_CH(ch, 0x04) = (uint32_t)(uintptr_t)band;
            DMA_CH(ch, 0x08) = band_words;
            const uint32_t t = TIMERAWL;
            DMA_CH(ch, 0x0C) = CTRL_EN | CTRL_WORD | (1u << 6) /* INCR_WRITE */ |
                               CTRL_CHAIN(ch) | CTRL_TREQ(DREQ_XIP_STREAM);
            while (DMA_CH(ch, 0x0C) & CTRL_BUSY) { }
            const uint32_t dt = TIMERAWL - t;
            dma_us += dt;
            if (dt > worst) worst = dt;
            for (uint32_t i = 0; i < band_words; i++) {
                const uint32_t want = (first + i) * 2654435761u;
                if (band[i] == want) continue;
                if (shown < 6) {
                    ubiqos_line_t l;
                    ubiqos_line_reset(&l);
                    ubiqos_line_str(&l, "  word ");
                    ubiqos_line_u32(&l, first + i);
                    ubiqos_line_str(&l, " want ");
                    ubiqos_line_hex(&l, want);
                    ubiqos_line_str(&l, " stream ");
                    ubiqos_line_hex(&l, band[i]);
                    ubiqos_line_str(&l, " cpu ");
                    ubiqos_line_hex(&l, w[first + i]);
                    ubiqos_line_str(&l, "\n");
                    ubiqos_line_flush(UBIQOS_STDOUT, &l);
                    shown++;
                }
                bad++;
            }
            bands++;
            y = (y + BAND) % H;
        }
        ubiqos_sleep(1);
    }
    DMA_CH(ch, 0x0C) = 0;
    const uint32_t elapsed = TIMERAWL - start;

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_u32(&l, bands);
    ubiqos_line_str(&l, " bands (");
    ubiqos_line_u32(&l, bands * BAND);
    ubiqos_line_str(&l, " lines) in ");
    ubiqos_line_u32(&l, elapsed / 1000u);
    ubiqos_line_str(&l, " ms, ");
    ubiqos_line_u32(&l, bands * BAND / (elapsed / 1000u ? elapsed / 1000u : 1u));
    ubiqos_line_str(&l, " lines/ms\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);           // a line holds only so much
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "DMA busy ");
    ubiqos_line_u32(&l, dma_us / 1000u);
    ubiqos_line_str(&l, " ms, worst band ");
    ubiqos_line_u32(&l, worst);
    ubiqos_line_str(&l, " us; wrong words ");
    ubiqos_line_u32(&l, bad);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
    ubiqos_free(band);
    ubiqos_free(fb);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: psrambench KB CH | lines SECONDS CH\n\n"
            "Reads KB kilobytes of PSRAM with DMA channel CH three ways -- through\n"
            "the cache, past it, and through the XIP stream -- and says how fast.\n"
            "'lines' instead copies a 640x480 frame from PSRAM a 16-line band at a\n"
            "time, two bands a millisecond, for SECONDS, checking every word.\n"
            "CH must be a channel nothing else uses.\n")) return;
    if (argc == 4 && is(argv[1], "lines")) {
        const uint32_t secs = num(argv[2]), c = num(argv[3]);
        if (!secs || secs > 3600 || c > 15) {
            ubiqos_write_str(UBIQOS_STDOUT, "psrambench: lines 1..3600 s, CH 0..15\n");
            return;
        }
        lines_test(secs, c);
        return;
    }
    if (argc != 3) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: psrambench KB CH\n");
        return;
    }
    const uint32_t kb = num(argv[1]), ch = num(argv[2]);
    if (!kb || kb > 4096 || ch > 15) {
        ubiqos_write_str(UBIQOS_STDOUT, "psrambench: KB 1..4096, CH 0..15\n");
        return;
    }
    const uint32_t bytes = kb * 1024u, words = bytes / 4u;

    uint8_t *buf = (uint8_t *)ubiqos_alloc_bulk(bytes);
    if (!buf) { ubiqos_write_str(UBIQOS_STDOUT, "psrambench: no PSRAM\n"); return; }
    const uint32_t cached = (uint32_t)(uintptr_t)buf;
    if ((cached >> 24) != 0x11u) {
        ubiqos_write_str(UBIQOS_STDOUT, "psrambench: that block is not in PSRAM\n");
        ubiqos_free(buf);
        return;
    }
    const uint32_t nocache = cached + NOCACHE_OFFSET;

    // Anything the cache still holds for this range goes first, unwritten.
    // The block was somebody else's a moment ago, and a dirty line of theirs
    // evicted later would land on top of the pattern below. (The first runs
    // found a few dozen wrong words; whether this was why is not known.)
    invalidate(cached, bytes);

    // A pattern straight into the chip, past the cache.
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)nocache;
    uint32_t t0 = TIMERAWL;
    for (uint32_t i = 0; i < words; i++) w[i] = i * 2654435761u;
    say_rate("CPU write, uncached:  ", bytes, TIMERAWL - t0, 0xffffffffu);

    // Where DMA writes must be SRAM: a program's stack is in PSRAM here, and
    // a sink there would make every word read a word written as well.
    uint32_t *line = (uint32_t *)ubiqos_alloc(640);
    if (!line || ((uint32_t)(uintptr_t)line >> 28) != 0x2u) {
        ubiqos_write_str(UBIQOS_STDOUT, "psrambench: no SRAM for the destination\n");
        if (line) ubiqos_free(line);
        ubiqos_free(buf);
        return;
    }
    volatile uint32_t *sink = line;
    say_rate("DMA read, cached:     ", bytes, run(ch, cached, true, TREQ_FORCE, sink, words), 0xffffffffu);
    say_rate("DMA read, uncached:   ", bytes, run(ch, nocache, true, TREQ_FORCE, sink, words), 0xffffffffu);

    // The stream: the FIFO is two words deep, so DMA paced by its DREQ.
    XIP_STREAM_CTR = 0;
    XIP_STREAM_ADDR = cached;
    XIP_STREAM_CTR = words;
    say_rate("DMA read, stream:     ", bytes,
             run(ch, XIP_AUX_STREAM, false, DREQ_XIP_STREAM, sink, words), 0xffffffffu);

    // The stream again, this time into a line in SRAM and checked: what came
    // out must be what was written. 640 bytes, as a scanline would be.
    {
        uint32_t bad = 0, us = 0;
        const uint32_t lines = bytes / 640u;
        for (uint32_t n = 0; n < lines; n++) {
            XIP_STREAM_CTR = 0;
            XIP_STREAM_ADDR = cached + n * 640u;
            XIP_STREAM_CTR = 160u;
            DMA_CH(ch, 0x0C) = 0;
            DMA_CH(ch, 0x00) = XIP_AUX_STREAM;
            DMA_CH(ch, 0x04) = (uint32_t)(uintptr_t)line;
            DMA_CH(ch, 0x08) = 160u;
            const uint32_t s = TIMERAWL;
            DMA_CH(ch, 0x0C) = CTRL_EN | CTRL_WORD | (1u << 6) /* INCR_WRITE */ |
                               CTRL_CHAIN(ch) | CTRL_TREQ(DREQ_XIP_STREAM);
            while (DMA_CH(ch, 0x0C) & CTRL_BUSY) { }
            us += TIMERAWL - s;
            for (uint32_t i = 0; i < 160u; i++) {
                const uint32_t want = (n * 160u + i) * 2654435761u;
                if (line[i] == want) continue;
                // Read the same word again, by the CPU and past the cache: if
                // it is right now, the stream read it wrong; if it is wrong
                // again, the chip holds it wrong.
                if (bad < 6) {
                    const uint32_t again = w[n * 160u + i];
                    ubiqos_line_t l;
                    ubiqos_line_reset(&l);
                    ubiqos_line_str(&l, "  word ");
                    ubiqos_line_u32(&l, n * 160u + i);
                    ubiqos_line_str(&l, " want ");
                    ubiqos_line_hex(&l, want);
                    ubiqos_line_str(&l, " stream ");
                    ubiqos_line_hex(&l, line[i]);
                    ubiqos_line_str(&l, " cpu ");
                    ubiqos_line_hex(&l, again);
                    ubiqos_line_str(&l, "\n");
                    ubiqos_line_flush(UBIQOS_STDOUT, &l);
                }
                bad++;
            }
        }
        say_rate("stream, 640-byte lines:", lines * 640u, us, bad);
    }
    ubiqos_free(line);
    DMA_CH(ch, 0x0C) = 0;
    ubiqos_free(buf);
}

#include "../../common/myrtos_abi.h"

// play -- a WAV file out of /dev/audio.
//
//   play FILE
//   play -i FILE     say what the file is and stop
//
// The device runs at one rate and only one: 48000, because that is what comes
// out exact from a 120 MHz system clock and an integer PIO divider. Most of
// the free WAV files on the internet are 44100. So the player resamples rather
// than refusing, and it asks the device what rate to resample TO -- see
// MYRTOS_SS_RATE -- instead of having 48000 written in a second time. Point it
// at a device that runs at 32000 and it plays there.
//
// Linear interpolation, which is the honest description of what this does. It
// is not a good resampler: the error it makes lands as quiet high-frequency
// junk near the top of the band, worst on material with a lot of treble. It is
// two multiplies a sample and needs no tables, and going 44100 to 48000 is a
// ratio near one, where linear interpolation is at its least bad. A proper
// polyphase filter is the upgrade, and it wants somewhere to keep its
// coefficients.

MYRTOS_MEM_SIZE(24576);

#define IN_FRAMES   512u          // read this many source frames at a time
#define OUT_FRAMES  704u          // more than IN_FRAMES: upsampling makes more

// --- READING NUMBERS OUT OF THE FILE ---------------------------------------
// A byte at a time, because a WAV's fields are little-endian and packed with no
// regard for alignment -- a 32-bit load off a chunk header is not guaranteed to
// be on a four-byte boundary, and on this machine that is a trap rather than a
// slow read.

static uint32_t u16le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool tag_is(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1]
        && p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

// One sample, whatever the file keeps it as, as the int16 the device wants.
//
// The 32-bit float case is decoded with integer arithmetic on purpose. A module
// is linked without libgcc, so on a machine with no hardware FPU -- which is
// the RISC-V half of this system -- touching a float at all is a call to a
// soft-float routine that is not there, and the failure is at link time in a
// build nobody may have run. Taking the exponent and mantissa apart by hand
// costs a few shifts and works the same on both.
static inline int16_t sample_of(const uint8_t *p, uint32_t bits, bool is_float)
{
    if (bits == 8u) return (int16_t)(((int32_t)p[0] - 128) << 8);   // unsigned, 128 is silence
    if (bits == 16u) return (int16_t)(uint16_t)u16le(p);
    if (bits == 24u) return (int16_t)(uint16_t)u16le(p + 1);        // the top two bytes
    if (!is_float) return (int16_t)(uint16_t)u16le(p + 2);          // 32-bit int, likewise

    uint32_t u = u32le(p);
    int32_t  exp = (int32_t)((u >> 23) & 0xffu);
    if (!exp) return 0;                          // zero and denormals: silence
    int32_t  m = (int32_t)((u & 0x7fffffu) | 0x800000u);
    // The value is m * 2^(exp-127-23), and the sample wanted is that times
    // 32768. Nan and infinity fall out as the clamp below, which is the right
    // answer for a file that should not have contained them.
    int32_t  sh = exp - 127 - 23 + 15;
    int32_t  v = (sh >= 0) ? ((sh < 24) ? (m << sh) : 0x7fffffff)
                           : ((-sh < 31) ? (m >> -sh) : 0);
    if (v > 32767) v = 32767;
    return (int16_t)((u & 0x80000000u) ? -v : v);
}

typedef struct {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    bool     is_float;
    uint32_t data_bytes;
} wav_t;

static void say(const char *a, const char *b) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, a);
    if (b) myrtos_line_str(&l, b);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDERR, &l);
}

// Walks the chunks rather than trusting the layout. A WAV written by anything
// other than the simplest encoder has LIST, INFO or fact chunks between fmt
// and data, and a player that assumed data began at byte 44 would play those
// as audio -- which is a loud noise, not a quiet bug.
static bool wav_open(int32_t fd, wav_t *w) {
    uint8_t hdr[16];
    if (myrtos_read(fd, hdr, 12) != 12) { say("play: too short to be a WAV", 0); return false; }
    if (!tag_is(hdr, "RIFF") || !tag_is(hdr + 8, "WAVE")) {
        say("play: not a RIFF/WAVE file", 0);
        return false;
    }

    bool have_fmt = false;
    for (;;) {
        if (myrtos_read(fd, hdr, 8) != 8) { say("play: no data chunk in the file", 0); return false; }
        uint32_t size = u32le(hdr + 4);

        if (tag_is(hdr, "fmt ")) {
            // 26 rather than 16: WAVE_FORMAT_EXTENSIBLE keeps the format it
            // really is in the first two bytes of a GUID at offset 24, and
            // that is the only thing that says whether 32-bit samples are
            // integers or floats.
            uint8_t f[26];
            uint32_t want = size < 26u ? size : 26u;
            if (myrtos_read(fd, f, want) != (int32_t)want) return false;
            uint32_t format = u16le(f);
            // 0xfffe is EXTENSIBLE, which nearly every recent encoder writes
            // even for plain PCM.
            if (format == 0xfffeu && want >= 26u) format = u16le(f + 24);
            if (format != 1u && format != 3u) {
                say("play: not PCM (compressed WAVs are not supported)", 0);
                return false;
            }
            w->channels = u16le(f + 2);
            w->rate     = u32le(f + 4);
            w->bits     = u16le(f + 14);
            w->is_float = (format == 3u);
            have_fmt = true;
            // Whatever is left of a longer fmt chunk, plus its pad byte.
            uint32_t rest = size - want + (size & 1u);
            if (rest && myrtos_seek(fd, (int32_t)rest, MYRTOS_SEEK_CUR) < 0) return false;
            continue;
        }

        if (tag_is(hdr, "data")) {
            if (!have_fmt) { say("play: data before fmt, which is not a WAV", 0); return false; }
            w->data_bytes = size;
            return true;
        }

        // Something else -- skip it whole. Seeking rather than reading,
        // because a LIST chunk of album art is megabytes and there is no
        // buffer here that wants to see it.
        if (myrtos_seek(fd, (int32_t)(size + (size & 1u)), MYRTOS_SEEK_CUR) < 0) {
            say("play: cannot skip a chunk in this file", 0);
            return false;
        }
    }
}

// --- PLAYING ----------------------------------------------------------------

// Thread-local, not static. A module here may be shared between processes and
// so may have no writable static data at all -- check_module.py refuses a .data
// or .bss section outright -- and __thread puts these in the process's own area
// instead. Locals would work too; three and a half kilobytes of them would not,
// on a stack that also has to hold everything play calls.
static __thread uint8_t  raw[IN_FRAMES * 8u];   // 32-bit stereo is the widest frame
static __thread int16_t  src[IN_FRAMES * 2u];   // decoded to stereo, always
static __thread int16_t  out[OUT_FRAMES * 2u];

// Everything the device took, however many calls that is. A short write is the
// normal answer from /dev/audio -- the ring is as full as the DMA has left it --
// and a caller that treated one as an error would drop audio at every ring
// boundary.
static bool push(int32_t fd, const int16_t *frames, uint32_t n) {
    uint32_t done = 0, bytes = n * 4u;
    while (done < bytes) {
        int32_t w = myrtos_write(fd, (const uint8_t *)frames + done, bytes - done);
        if (w < 0) return false;
        // Ring full: let the DMA drain some. One millisecond rather than two,
        // because the wait is a whole scheduler tick either way and the ring
        // holds only twenty of them -- sleeping longer than necessary here is
        // sleeping through the time the reader needed to get ahead again.
        if (w == 0) { myrtos_sleep(1); continue; }
        done += (uint32_t)w;
    }
    return true;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: play [-i] FILE\n\nPlays a PCM WAV file. 8- and 16-bit, mono or stereo, any rate --\n"
            "it is resampled to whatever /dev/audio runs at.\n-i says what the file is without playing it.\n"))
        return;

    bool info_only = false;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i' && !argv[1][2]) { info_only = true; argv++; argc--; }
    if (argc < 2) { say("usage: play [-i] FILE", 0); return; }

    int32_t f = myrtos_open_flags(argv[1], MYRTOS_O_RDONLY);
    if (f < 0) { say("play: cannot open ", argv[1]); return; }

    wav_t w;
    if (!wav_open(f, &w)) { myrtos_close(f); return; }

    if (w.channels < 1u || w.channels > 2u) {
        say("play: only mono and stereo", 0);
        myrtos_close(f); return;
    }
    if (w.bits != 8u && w.bits != 16u && w.bits != 24u && w.bits != 32u) {
        say("play: only 8, 16, 24 and 32 bits a sample", 0);
        myrtos_close(f); return;
    }

    int32_t dev = myrtos_open("/dev/audio");
    if (dev < 0) { say("play: no /dev/audio", 0); myrtos_close(f); return; }

    // What to resample to, asked rather than assumed. This is the whole reason
    // the device can be asked anything at all.
    uint32_t dst_rate = 0;
    if (myrtos_getstat(dev, MYRTOS_SS_RATE, &dst_rate, sizeof dst_rate) < 0 || !dst_rate) {
        say("play: the device will not say what rate it runs at", 0);
        myrtos_close(dev); myrtos_close(f); return;
    }

    uint32_t frame_bytes = w.channels * (w.bits / 8u);
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, argv[1]);
    myrtos_line_str(&l, ": "); myrtos_line_u32(&l, w.rate);
    myrtos_line_str(&l, " Hz, "); myrtos_line_u32(&l, w.bits);
    myrtos_line_str(&l, "-bit, "); myrtos_line_str(&l, w.channels == 1u ? "mono" : "stereo");
    myrtos_line_str(&l, ", "); myrtos_line_u32(&l, w.data_bytes / (frame_bytes ? frame_bytes : 1u) / (w.rate ? w.rate : 1u));
    myrtos_line_str(&l, " s");
    if (w.rate != dst_rate) {
        myrtos_line_str(&l, " (resampled to "); myrtos_line_u32(&l, dst_rate);
        myrtos_line_str(&l, ")");
    }
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    if (info_only) { myrtos_close(dev); myrtos_close(f); return; }

    // How far the source position moves per output frame, in 16.16. Written in
    // two parts on purpose: (rate << 16) overflows 32 bits above 65535 Hz, and
    // the alternative -- a 64-bit divide -- is a call to __udivdi3, which is in
    // libgcc and libgcc is not linked into a module. Quotient and remainder
    // separately keeps every intermediate inside 32 bits, exactly.
    uint32_t step = ((w.rate / dst_rate) << 16)
                  + (((w.rate % dst_rate) << 16) / dst_rate);

    // Two ways of doing it, and which one depends on which direction the rate
    // is going.
    //
    // Going UP -- 44100 to 48000, the common case -- each output frame lands
    // between two input frames and is interpolated between them.
    //
    // Going DOWN, interpolation is the wrong tool: picking one value out of
    // every two and a bit is decimation, and everything in the source above
    // half the new rate folds back into the audible band as tones that were
    // never there. A 96000 Hz file with content at 30 kHz would come out with
    // a whistle at 18. So each output frame is instead the AVERAGE of the
    // input frames it spans -- a box filter, one add a sample and one divide
    // an output frame.
    //
    // Measured rather than assumed, and the measurement is worth writing down
    // because it is worse than it sounds: a box nulls at multiples of the
    // OUTPUT rate and does very little just above output Nyquist, which is
    // exactly where the first aliases land. 96000 to 48000 with a 30 kHz tone
    // attenuates the fold-down by 5 dB; at 24 kHz it would be 3. It helps most
    // where the ratio is large and the offending content is well up the band,
    // and it is genuinely weak at 2:1.
    //
    // The real fix is a polyphase FIR, which is also the only thing that would
    // make the interpolating side better than it is. Not done: it wants
    // coefficients, and coefficients want either a table or a way to compute
    // them, and neither belongs in the first version of this. What is here
    // does not pretend otherwise.
    bool decimating = step > 0x10000u;

    int16_t prev_l = 0, prev_r = 0;   // last frame of the previous buffer
    uint32_t pos = 0;                 // 16.16, from the start of this buffer
    int32_t acc_l = 0, acc_r = 0;     // the box filter, carried across buffers
    uint32_t acc_n = 0, frac = 0;
    uint32_t left = w.data_bytes;
    bool ok = true;

    while (left && ok) {
        uint32_t want = IN_FRAMES * frame_bytes;
        if (want > left) want = left;
        int32_t got = myrtos_read(f, raw, want);
        if (got <= 0) break;
        uint32_t n = (uint32_t)got / frame_bytes;
        if (!n) break;
        left -= n * frame_bytes;

        // Everything becomes 16-bit stereo here, so the resampler below has one
        // case instead of ten.
        // The common shape -- 16-bit stereo -- gets its own loop rather than
        // going through sample_of, and it is worth the duplication. Everything
        // else pays a call, two branches on the bit depth and a branch on the
        // channel count PER SAMPLE, which measured at 1.24 seconds of work for
        // a second of 32-bit audio: over real time, so the ring emptied and it
        // stuttered. The decision does not change within a file, so it does
        // not belong inside the loop.
        uint32_t sample_bytes = w.bits / 8u;
        if (w.bits == 16u && w.channels == 2u) {
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *p = raw + i * 4u;
                src[i * 2]     = (int16_t)(uint16_t)u16le(p);
                src[i * 2 + 1] = (int16_t)(uint16_t)u16le(p + 2);
            }
        } else if (w.channels == 2u) {
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *p = raw + i * frame_bytes;
                src[i * 2]     = sample_of(p, w.bits, w.is_float);
                src[i * 2 + 1] = sample_of(p + sample_bytes, w.bits, w.is_float);
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                int16_t a = sample_of(raw + i * frame_bytes, w.bits, w.is_float);
                src[i * 2] = a;
                src[i * 2 + 1] = a;
            }
        }

        uint32_t nout = 0;
        if (decimating) {
            // Every input frame goes into the accumulator; an output frame
            // comes out each time enough of them have. The accumulator and the
            // fraction live outside this loop, so a span that straddles a
            // buffer boundary is averaged across it rather than cut in two.
            for (uint32_t i = 0; i < n && ok; i++) {
                acc_l += src[i * 2];
                acc_r += src[i * 2 + 1];
                acc_n++;
                frac += 0x10000u;
                if (frac >= step) {
                    frac -= step;
                    out[nout * 2]     = (int16_t)(acc_l / (int32_t)acc_n);
                    out[nout * 2 + 1] = (int16_t)(acc_r / (int32_t)acc_n);
                    acc_l = acc_r = 0;
                    acc_n = 0;
                    if (++nout == OUT_FRAMES) { ok = push(dev, out, nout); nout = 0; }
                }
            }
        } else {
            // Linear interpolation between the sample before the position and
            // the one after. Index 0 reaches back into the previous buffer,
            // which is what prev_l/prev_r are for: without them every buffer
            // boundary would interpolate from silence and put a click there,
            // 187 times a second.
            while ((pos >> 16) < n) {
                uint32_t i = pos >> 16;
                // Half the fraction's bits. (b - a) can be 65535 and the
                // fraction 65535, and their product does not fit in 32 bits;
                // eight bits of interpolation is inaudible here and always
                // fits.
                int32_t fr = (int32_t)((pos & 0xffffu) >> 8);
                int16_t al = (i == 0) ? prev_l : src[(i - 1) * 2];
                int16_t ar = (i == 0) ? prev_r : src[(i - 1) * 2 + 1];
                int16_t bl = src[i * 2], br = src[i * 2 + 1];
                out[nout * 2]     = (int16_t)(al + (((int32_t)bl - al) * fr >> 8));
                out[nout * 2 + 1] = (int16_t)(ar + (((int32_t)br - ar) * fr >> 8));
                pos += step;
                if (++nout == OUT_FRAMES) {
                    if (!(ok = push(dev, out, nout))) break;
                    nout = 0;
                }
            }
            pos -= n << 16;                 // carry the fraction into the next buffer
            prev_l = src[(n - 1) * 2];
            prev_r = src[(n - 1) * 2 + 1];
        }
        if (ok && nout) ok = push(dev, out, nout);
    }

    if (!ok) say("play: the device stopped taking audio", 0);
    myrtos_close(dev);
    myrtos_close(f);
}

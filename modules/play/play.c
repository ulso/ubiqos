#include "../../common/myrtos_abi.h"
#include "sinc_table.h"

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
// The resampler is a polyphase FIR: a Kaiser-windowed sinc, ten zero crossings
// each side, sampled 64 times between each pair with the fraction interpolated
// between those. tools/make_sinc.py generates the coefficients, because a
// module has no floating point to compute them with -- the RISC-V half of this
// system has no FPU and no libgcc to fake one -- and no libm to ask for a sine.
//
// The filter stretches with the ratio. Upward it is a fixed 20 taps; downward
// the impulse response is scaled by the ratio, which moves the cutoff down to
// the OUTPUT Nyquist, and that is the whole trick -- it is what stops content
// above the new Nyquist folding back into the band, and what the linear
// interpolation and box average that came before could not do.

MYRTOS_MEM_SIZE(24576);

#define IN_FRAMES   512u          // read this many source frames at a time
#define OUT_FRAMES  704u          // more than IN_FRAMES: upsampling makes more

// How far the filter reaches, in source frames, on each side of an output.
// That is SINC_NZERO / time_scale, so it grows as the rate falls; 8 is the
// steepest decimation the filter is stretched for, and beyond it the cutoff
// stops following the rate down. 8:1 is 192000 into a 24000 Hz device, which
// no file here is going to ask for.
#define MAX_DECIM   8u
#define GUARD       (SINC_NZERO * MAX_DECIM)

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

// A 32-bit IEEE float as the int16 the device wants, decoded with integer
// arithmetic on purpose.
//
// UNEXPLAINED, and measured with `play -v` on the board's own clock: this loop
// costs about 400 cycles a sample, where the 32-bit integer loop beside it --
// same reads, same stores, same count, four instructions instead of sixteen --
// costs a handful. A second of 32-bit float audio takes 1360 ms of work and so
// stutters; the same file with its format tag saying integer takes 1080. Five
// explanations were tried and measured and none of them was it: byte-wise
// loads (made them word loads, no change), the branches (made it branchless,
// no change), the layout on the card (played a fresh copy, no change), the
// hot path branching across a literal pool (marked the clamps unlikely, no
// change), and my own timing (an SD card left slow by usbdisk, which did
// explain one contradictory run and nothing else). What is left is instruction
// fetch, since a module is executed through a cache, but that is a hypothesis
// and not a measurement, and it is written here as one.
//
// Every other format plays at or under real time. 32-bit float is a production
// format and rare in delivered audio, which is why this is recorded rather
// than worked around. A module is linked without libgcc, so on a machine
// with no hardware FPU -- which is the RISC-V half of this system -- touching a
// float at all is a call to a soft-float routine that is not there. Taking the
// exponent and mantissa apart by hand costs a few shifts and works on both.
static inline int16_t float_sample(uint32_t u)
{
    int32_t m  = (int32_t)((u & 0x7fffffu) | 0x800000u);
    // 135 is 127 for the exponent bias, 23 for the mantissa and -15 for the
    // scaling to a 16-bit sample, so this is the right shift that turns the
    // one into the other. Every case that is not an ordinary number falls out
    // of clamping it rather than being tested for: a shift of nought or less
    // means the value was at or above 1.0 and clips, 31 or more means it
    // underflows to silence, and a zero exponent -- zero itself, and
    // denormals -- lands in the second of those. NaN and infinity clip, which
    // is the right answer for a file that should not have contained them.
    int32_t sh = 135 - (int32_t)((u >> 23) & 0xffu);
    if (sh < 0) sh = 0;
    if (sh > 31) sh = 31;
    int32_t v = m >> sh;
    if (v > 32767) v = 32767;
    return (int16_t)((u & 0x80000000u) ? -v : v);
}

// Which of the five the file keeps its samples as, worked out once.
enum { K_U8, K_S16, K_S24, K_S32, K_F32 };

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
// Aligned so the 32-bit formats can be read a word at a time. A WAV's fields
// are packed with no regard for alignment and are read a byte at a time for
// that reason, but the SAMPLES are not fields: at 4 bytes each they sit on
// word boundaries from the start of the buffer, and this is the only thing
// that makes them do so.
static __thread __attribute__((aligned(4))) uint8_t raw[IN_FRAMES * 8u];
// GUARD frames of history in front and GUARD of lookahead behind: the filter
// reaches both ways from every output, and a buffer that held only the new
// frames would have to invent silence at each end of it.
static __thread int16_t  src[(IN_FRAMES + 2u * GUARD) * 2u];
static __thread int16_t  out[OUT_FRAMES * 2u];

// One coefficient, with the fraction between two table entries interpolated.
// 64 phases would be audible as a phase-quantisation whine on its own; the
// interpolation is what makes a table this small enough.
static inline int32_t sinc_at(uint32_t tp) {
    uint32_t i = tp >> 16;
    int32_t a = sinc_h[i], b = sinc_h[i + 1];
    return a + (((b - a) * (int32_t)((tp >> 8) & 0xffu)) >> 8);
}

static inline int16_t clamp16(int64_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

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

// The ring as hex, four frames a line. Slow and enormous and meant to
// be read by a program at the other end of the serial line, not by a person.
static __thread uint8_t ringbuf[4096];

static void dump_ring(int32_t dev)
{
    if (myrtos_getstat(dev, MYRTOS_SS_RINGDUMP, ringbuf, sizeof ringbuf) < 0) {
        say("play: the device will not show its ring", 0);
        return;
    }
    myrtos_write_str(MYRTOS_STDOUT, "RING\n");
    // Sixteen bytes a line, not more. myrtos_line_t holds 96 characters and
    // hex_byte writes four of them, so 24 bytes is the most that fits -- and a
    // line that does not fit is silently cut, which came out as a dump that
    // was almost right and therefore worse than one that failed.
    myrtos_line_t l;
    for (uint32_t i = 0; i < sizeof ringbuf; i += 16) {
        myrtos_line_reset(&l);
        for (uint32_t j = 0; j < 16; j++) myrtos_line_hex_byte(&l, ringbuf[i + j]);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }
    myrtos_write_str(MYRTOS_STDOUT, "ENDRING\n");
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: play [-i] [-v] FILE\n\nPlays a PCM WAV file. 8- and 16-bit, mono or stereo, any rate --\n"
            "it is resampled to whatever /dev/audio runs at.\n-i says what the file is without playing it.\n"))
        return;

    bool info_only = false, verbose = false, dump = false;
    while (argc > 1 && argv[1][0] == '-' && argv[1][1] && !argv[1][2]) {
        if (argv[1][1] == 'i') info_only = true;
        else if (argv[1][1] == 'v') verbose = true;
        else if (argv[1][1] == 'd') dump = true;
        else break;
        argv++; argc--;
    }
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

    // The filter's time scale: how much of its own length it covers per source
    // frame. Upward it is one -- the response is used as designed. Downward it
    // is 1/step, which stretches the impulse response in time and so pulls its
    // cutoff down in frequency, from the input Nyquist to the output one. That
    // is the entire reason a polyphase FIR handles decimation and the box
    // average it replaced did not.
    //
    // 0xffffffff rather than 1 << 32: the value wanted is 2^32/step and that
    // numerator does not fit in the word doing the dividing. One part in four
    // billion low is not a rate error anybody can measure.
    uint32_t ts_q16 = (step <= 0x10000u) ? 0x10000u : (0xffffffffu / step);
    if (ts_q16 < 0x10000u / MAX_DECIM) ts_q16 = 0x10000u / MAX_DECIM;

    // How far along the table one source frame moves, in Q16 table entries.
    uint32_t tstep = ts_q16 * SINC_NPHASE;
    uint32_t tend  = (uint32_t)(SINC_NZERO * SINC_NPHASE) << 16;

    // Silence in front of the file, so the first output has a left wing to
    // read. The file's own first frame lands at src[GUARD], which is where pos
    // starts.
    for (uint32_t i = 0; i < GUARD * 2u; i++) src[i] = 0;
    uint32_t have = GUARD;
    uint32_t pos = GUARD << 16;

    uint32_t t0 = myrtos_ticks_now(), frames_in = 0, frames_out = 0;
    uint32_t left = w.data_bytes;
    int kind = w.is_float ? K_F32
             : w.bits == 8u  ? K_U8
             : w.bits == 16u ? K_S16
             : w.bits == 24u ? K_S24 : K_S32;
    bool ok = true, drained = false;

    while (ok && !drained) {
        // Fill up behind what is already there.
        uint32_t room = IN_FRAMES + 2u * GUARD - have;
        uint32_t n = 0;
        if (left && room) {
            uint32_t want = (room < IN_FRAMES ? room : IN_FRAMES) * frame_bytes;
            if (want > left) want = left;
            int32_t got = myrtos_read(f, raw, want);
            if (got > 0) {
                n = (uint32_t)got / frame_bytes;
                left -= n * frame_bytes;
            } else {
                left = 0;
            }
        }

        if (!n && !left) {
            // End of the file. GUARD frames of silence let the filter run off
            // the end of the audio instead of stopping in the middle of its
            // own impulse response, which would be a click.
            n = GUARD;
            if (n > room) n = room;
            for (uint32_t i = 0; i < n * 2u; i++) src[(have + i / 2u) * 2u + (i & 1u)] = 0;
            drained = true;
        }

        // One loop per format, chosen once per buffer, and the format decision
        // never inside it. This was a single loop calling sample_of per
        // sample, and on this machine that cost 40% of real time on 32-bit
        // files -- enough that the ring emptied and it stuttered -- while the
        // one shape that did have its own loop, 16-bit stereo, ran at 1.02.
        // Same arithmetic, same file, same resampler: the difference was
        // entirely the call and the branches around it.
        //
        // Stereo decodes straight into place, since a WAV's samples are
        // already interleaved the way src wants them. Mono decodes flat and is
        // then spread outwards, backwards so it does not overwrite itself.
        if (!drained) {
            int16_t *d = src + have * 2u;
            uint32_t nsamp = n * w.channels;
            switch (kind) {
            case K_U8:
                for (uint32_t i = 0; i < nsamp; i++)
                    d[i] = (int16_t)(((int32_t)raw[i] - 128) << 8);
                break;
            case K_S16:
                for (uint32_t i = 0; i < nsamp; i++)
                    d[i] = (int16_t)(uint16_t)u16le(raw + i * 2u);
                break;
            case K_S24:
                for (uint32_t i = 0; i < nsamp; i++)
                    d[i] = (int16_t)(uint16_t)u16le(raw + i * 3u + 1u);
                break;
            case K_S32: {
                const uint32_t *q = (const uint32_t *)(const void *)raw;
                for (uint32_t i = 0; i < nsamp; i++) d[i] = (int16_t)(uint16_t)(q[i] >> 16);
                break;
            }
            default: {
                const uint32_t *q = (const uint32_t *)(const void *)raw;
                for (uint32_t i = 0; i < nsamp; i++) d[i] = float_sample(q[i]);
                break;
            }
            }
            if (w.channels == 1u)
                for (uint32_t i = n; i-- > 0; ) { d[i * 2] = d[i * 2 + 1] = d[i]; }
        }
        have += n;
        frames_in += n;

        // An output can be made while the filter's right wing still has real
        // frames to read. GUARD is its longest reach.
        uint32_t nout = 0;
        while (ok && (pos >> 16) + GUARD < have) {
            uint32_t i = pos >> 16;
            uint32_t fr = pos & 0xffffu;
            int32_t accl = 0, accr = 0;

            // Left wing: src[i], src[i-1], ... at distances fr, fr+1, fr+2 in
            // source frames, which is fr*tstep, +tstep, +tstep along the table.
            uint32_t tp = (uint32_t)(((uint64_t)fr * tstep) >> 16);
            for (uint32_t j = i; tp < tend; tp += tstep) {
                int32_t h = sinc_at(tp);
                accl += (int32_t)src[j * 2] * h;
                accr += (int32_t)src[j * 2 + 1] * h;
                if (!j--) break;
            }
            // Right wing: src[i+1], src[i+2], ... at distances 1-fr, 2-fr, ...
            tp = (uint32_t)(((uint64_t)(0x10000u - fr) * tstep) >> 16);
            for (uint32_t j = i + 1; tp < tend && j < have; j++, tp += tstep) {
                int32_t h = sinc_at(tp);
                accl += (int32_t)src[j * 2] * h;
                accr += (int32_t)src[j * 2 + 1] * h;
            }

            // Q15 out of the coefficients, then times the time scale, because
            // a stretched filter has proportionally more taps under it and
            // would otherwise come out that much louder.
            out[nout * 2]     = clamp16(((int64_t)(accl >> 15) * (int32_t)ts_q16) >> 16);
            out[nout * 2 + 1] = clamp16(((int64_t)(accr >> 15) * (int32_t)ts_q16) >> 16);
            pos += step;
            frames_out++;
            // Halfway through, hand back what the DMA is actually reading.
            // This is the only view of the samples the hardware is being fed
            // that does not need a probe on pins too small to reach.
            if (dump && frames_out == 24000u) dump_ring(dev);
            if (++nout == OUT_FRAMES) { ok = push(dev, out, nout); nout = 0; }
        }
        if (ok && nout) ok = push(dev, out, nout);

        // Keep GUARD frames of history in front of where the next output sits,
        // and move them to the start so the read above has room behind them.
        uint32_t shift = (pos >> 16) - GUARD;
        if (shift) {
            uint32_t keep = have - shift;
            for (uint32_t i = 0; i < keep * 2u; i++) src[i] = src[shift * 2u + i];
            have = keep;
            pos -= shift << 16;
        }
    }

    if (verbose) {
        // The board's own clock, because timing this from the far end of a
        // serial line measures the serial line. Expected is what the output
        // frames are worth at the device's rate; anything much over it is the
        // ring running dry, which is heard as a stutter.
        uint32_t ms = myrtos_ticks_now() - t0;
        myrtos_line_t v;
        myrtos_line_reset(&v);
        myrtos_line_str(&v, "in "); myrtos_line_u32(&v, frames_in);
        myrtos_line_str(&v, ", out "); myrtos_line_u32(&v, frames_out);
        myrtos_line_str(&v, ", "); myrtos_line_u32(&v, ms);
        myrtos_line_str(&v, " ms for "); myrtos_line_u32(&v, frames_out / (dst_rate / 1000u));
        myrtos_line_str(&v, " ms of audio\n");
        myrtos_line_flush(MYRTOS_STDOUT, &v);
    }
    if (!ok) say("play: the device stopped taking audio", 0);
    myrtos_close(dev);
    myrtos_close(f);
}

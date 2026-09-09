#include "../../common/myrtos_abi.h"

// vidstat -- whether the character generator is keeping ahead of the beam.
//
// The display plays a small ring of scanline buffers and an interrupt above the
// kernel's priority threshold refills them. underruns counts the times it did
// not get there first. Everything else here is context for that one number.

static void row(const char *label, uint32_t v, const char *unit) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, label);
    myrtos_line_u32(&l, v);
    myrtos_line_str(&l, unit);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

static void peek(uint32_t line) {
    uint8_t b[64];
    myrtos_video_peek(line, b);
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "line ");
    myrtos_line_u32(&l, line);
    myrtos_line_str(&l, ": ");
    for (int i = 0; i < 32; i++) {
        myrtos_line_hex_byte(&l, b[i]);
        myrtos_line_str(&l, " ");
    }
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

// One line per record. The body is rendered compactly -- <E> for escape, <r>
// and <n> for the line ending -- and cut at sixty characters, because what is
// being looked for is where one writer's bytes land inside another's, and that
// shows in the first few.
static void trace_dump(void)
{
    uint8_t b[64];
    uint32_t off = 0, left = 0, shown = 0;
    int state = 0;                       // 0 seeking, 1 pid, 2 length, 3 body
    myrtos_line_t l;
    myrtos_line_reset(&l);

    for (;;) {
        int32_t got = myrtos_console_trace(off, b);
        if (got <= 0) break;

        for (int32_t i = 0; i < got; i++) {
            uint8_t c = b[i];
            switch (state) {
            case 0:
                if (c == 0xfe) state = 1;
                break;
            case 1:
                if (l.len) { myrtos_line_str(&l, "\n"); myrtos_line_flush(MYRTOS_STDOUT, &l); }
                myrtos_line_reset(&l);
                myrtos_line_str(&l, "pid ");
                myrtos_line_u32(&l, c);
                myrtos_line_str(&l, "  ");
                state = 2;
                break;
            case 2:
                left = c; shown = 0;
                state = left ? 3 : 0;
                break;
            default: {
                if (shown < 60) {
                    const char *rep = 0;
                    char one = (char)c;
                    if (c == 0x1b)      rep = "<E>";
                    else if (c == '\r') rep = "<r>";
                    else if (c == '\n') rep = "<n>";
                    else if (c < 32 || c > 126) rep = ".";
                    if (rep) myrtos_line_str(&l, rep);
                    else     myrtos_line_chars(&l, &one, 1);
                    shown++;
                } else if (shown == 60) {
                    myrtos_line_str(&l, "...");
                    shown++;
                }
                if (--left == 0) state = 0;
                break;
            }
            }
        }
        off += (uint32_t)got;
    }
    if (l.len) { myrtos_line_str(&l, "\n"); myrtos_line_flush(MYRTOS_STDOUT, &l); }
}

void module_main(int argc, char **argv) {
    uint32_t s[16];

    // -s dumps the console's cells as text. It is not printed by default
    // because thirty lines of screen every time hides the four numbers that
    // are the point -- but it is what told us, on 9 Sep 2026, that a glyph
    // appearing in the wrong column was not the console putting it there.
    bool show_screen = (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 's');
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't') { trace_dump(); return; }

    if (myrtos_video_stats(s) < 0) {
        myrtos_write_str(MYRTOS_STDOUT,
            "This build draws into a framebuffer; there is nothing to keep up with.\n"
            "Configure with -DMYRTOS_VIDEO=chargen to use the character generator.\n");
        return;
    }

    // Eight cells of four scanlines: the top of a glyph row, its middle where
    // ink should be, and two rows further down.
    // Every row as text, with a | at each end so a character sitting in the
    // last column cannot be mistaken for trailing space.
    for (uint32_t r = 0; show_screen && r < 30; r++) {
        uint8_t b[80];
        myrtos_console_peek_row(r, b);
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_u32(&l, r);
        myrtos_line_str(&l, r < 10 ? "  |" : " |");
        for (int i = 0; i < 80; i++) {
            char ch = (char)b[i];
            myrtos_line_chars(&l, (ch < 32 || ch > 126) ? "." : &ch, 1);
        }
        myrtos_line_str(&l, "|\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    row("Scanline buffers:  ", s[3], "");
    row("Scrolled back:     ", s[6], " rows");
    row("History to go back:", s[7], " rows");
    row("  of which in PSRAM:", s[8], " rows");
    row("Beam is at line:   ", s[4], "");
    row("Built up to line:  ", s[5], "");
    row("Pumps:             ", s[1], "");
    row("Scanlines built:   ", s[2], "");
    // What it costs. PUMP_US is 500, so pumps*500 is the elapsed time the
    // display has been asked about, and the share of it spent inside the
    // interrupt is the number that decides whether anything else can run.
    //
    // BOTH ARE CUMULATIVE SINCE BOOT, so this share is an average over the
    // whole run and moves very slowly once the machine has been up a while.
    // It is honest for "what does this build cost" and useless for comparing
    // two things in one session -- it made a change look like it saved a third
    // when it saved a fifth. For that, read these two numbers twice and divide
    // the differences.
    row("Time in the pump:  ", s[10] / 1000, " ms");
    row("  pump period:     ", s[15], " us");
    row("  worst one call:  ", s[11], " us");
    if (s[1] && s[15]) row("  share of the CPU:", (s[10] / 8) * 100 / (s[1] * s[15] / 8), " %");
    row("Glyph rows built:  ", s[13], "");
    row("Single lines built:", s[14], "");
    row("Underruns:         ", s[0], "");
    if (s[12]) row("Segments refused:  ", s[12], " (raise MYRTOS_VEC_ACTIVE)");
    if (s[0]) row("  first at pump:   ", s[9], "");

    // Three questions, in the order that makes the next one worth asking.
    if (s[1] == 0) {
        myrtos_write_str(MYRTOS_STDOUT,
            "\nThe pump has never run. The alarm is not reaching the handler,\n"
            "so nothing has been built since the buffers were filled at boot.\n");
        return;
    }

    // A count of lines that says nothing about margin on its own: what matters
    // is that it tracks the beam. 480 lines a frame at about 57 frames a second
    // is some 27000 a second, so this should climb by that and no faster.
    if (s[0] == 0)
        myrtos_write_str(MYRTOS_STDOUT,
            "\nThe generator has been ahead of the display every line so far.\n");
    else
        myrtos_write_str(MYRTOS_STDOUT,
            "\nThe display has reached lines that were not built yet. Either the\n"
            "pump is being delayed past the ring's depth, or a line costs more\n"
            "than the budget allows.\n");
}

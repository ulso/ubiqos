#include "../../common/ubiqos_abi.h"

// vidstat -- whether the character generator is keeping ahead of the beam.
//
// The display plays a small ring of scanline buffers and an interrupt above the
// kernel's priority threshold refills them. underruns counts the times it did
// not get there first. Everything else here is context for that one number.

static void row(const char *label, uint32_t v, const char *unit) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, label);
    ubiqos_line_u32(&l, v);
    ubiqos_line_str(&l, unit);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

static void peek(uint32_t line) {
    uint8_t b[64];
    ubiqos_video_peek(line, b);
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "line ");
    ubiqos_line_u32(&l, line);
    ubiqos_line_str(&l, ": ");
    for (int i = 0; i < 32; i++) {
        ubiqos_line_hex_byte(&l, b[i]);
        ubiqos_line_str(&l, " ");
    }
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
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
    ubiqos_line_t l;
    ubiqos_line_reset(&l);

    for (;;) {
        int32_t got = ubiqos_console_trace(off, b);
        if (got <= 0) break;

        for (int32_t i = 0; i < got; i++) {
            uint8_t c = b[i];
            switch (state) {
            case 0:
                if (c == 0xfe) state = 1;
                break;
            case 1:
                if (l.len) { ubiqos_line_str(&l, "\n"); ubiqos_line_flush(UBIQOS_STDOUT, &l); }
                ubiqos_line_reset(&l);
                ubiqos_line_str(&l, "pid ");
                ubiqos_line_u32(&l, c);
                ubiqos_line_str(&l, "  ");
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
                    if (rep) ubiqos_line_str(&l, rep);
                    else     ubiqos_line_chars(&l, &one, 1);
                    shown++;
                } else if (shown == 60) {
                    ubiqos_line_str(&l, "...");
                    shown++;
                }
                if (--left == 0) state = 0;
                break;
            }
            }
        }
        off += (uint32_t)got;
    }
    if (l.len) { ubiqos_line_str(&l, "\n"); ubiqos_line_flush(UBIQOS_STDOUT, &l); }
}

void module_main(int argc, char **argv) {
    uint32_t s[16];

    // -s dumps the console's cells as text. It is not printed by default
    // because thirty lines of screen every time hides the four numbers that
    // are the point -- but it is what told us, on 9 Sep 2026, that a glyph
    // appearing in the wrong column was not the console putting it there.
    bool show_screen = (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 's');
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't') { trace_dump(); return; }

    if (ubiqos_video_stats(s) < 0) {
        ubiqos_write_str(UBIQOS_STDOUT,
            "This build draws into a framebuffer; there is nothing to keep up with.\n"
            "Configure with -DUBIQOS_VIDEO=chargen to use the character generator.\n");
        return;
    }

    // Eight cells of four scanlines: the top of a glyph row, its middle where
    // ink should be, and two rows further down.
    // Every row as text, with a | at each end so a character sitting in the
    // last column cannot be mistaken for trailing space.
    for (uint32_t r = 0; show_screen && r < 30; r++) {
        uint8_t b[80];
        ubiqos_console_peek_row(r, b);
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_u32(&l, r);
        ubiqos_line_str(&l, r < 10 ? "  |" : " |");
        for (int i = 0; i < 80; i++) {
            char ch = (char)b[i];
            ubiqos_line_chars(&l, (ch < 32 || ch > 126) ? "." : &ch, 1);
        }
        ubiqos_line_str(&l, "|\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }

    row("Scanline buffers:  ", s[3], "");
    row("Scrolled back:     ", s[6], " rows");
    row("History to go back:", s[7], " rows");
    row("  of which in PSRAM:", s[8], " rows");
    // A panel that reports its costliest band (slot 12) uses slots 2, 4, 5, 13
    // and 14 for that band's breakdown, and has no beam line or glyph rows.
    if (!s[12]) {
        row("Beam is at line:   ", s[4], "");
        row("Built up to line:  ", s[5], "");
    }
    row("Pumps:             ", s[1], "");
    if (!s[12]) row("Scanlines built:   ", s[2], "");
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
    // The RGB panel's pump draws bands of a scene, and the costliest band since
    // the last time this was asked says WHERE a scene is expensive -- the worst
    // call since boot only says that something once was. Zero from a display
    // that does not keep it.
    if (s[12]) {
        row("  costliest band:  ", s[12] >> 8, " us");
        row("    on row:        ", (s[12] & 0xffu) - 1u, "");
        row("    rectangles:    ", s[2], " us");
        row("    text:          ", s[5], " us");
        row("    masks:         ", s[4], " us");
        row("    curve fill:    ", s[13], " us");
        row("    curve line:    ", s[14], " us");
    }
    if (s[1] && s[15]) row("  share of the CPU:", (s[10] / 8) * 100 / (s[1] * s[15] / 8), " %");
    if (!s[12]) {
        row("Glyph rows built:  ", s[13], "");
        row("Single lines built:", s[14], "");
    }
    row("Underruns:         ", s[0], "");
    if (s[0]) row("  first at pump:   ", s[9], "");

    // Three questions, in the order that makes the next one worth asking.
    if (s[1] == 0) {
        ubiqos_write_str(UBIQOS_STDOUT,
            "\nThe pump has never run. The alarm is not reaching the handler,\n"
            "so nothing has been built since the buffers were filled at boot.\n");
        return;
    }

    // A count of lines that says nothing about margin on its own: what matters
    // is that it tracks the beam. 480 lines a frame at about 57 frames a second
    // is some 27000 a second, so this should climb by that and no faster.
    if (s[0] == 0)
        ubiqos_write_str(UBIQOS_STDOUT,
            "\nThe generator has been ahead of the display every line so far.\n");
    else
        ubiqos_write_str(UBIQOS_STDOUT,
            "\nThe display has reached lines that were not built yet. Either the\n"
            "pump is being delayed past the ring's depth, or a line costs more\n"
            "than the budget allows.\n");
}

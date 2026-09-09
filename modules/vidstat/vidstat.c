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

void module_main(void) {
    uint32_t s[8];

    if (myrtos_video_stats(s) < 0) {
        myrtos_write_str(MYRTOS_STDOUT,
            "This build draws into a framebuffer; there is nothing to keep up with.\n"
            "Configure with -DMYRTOS_VIDEO=chargen to use the character generator.\n");
        return;
    }

    // Eight cells of four scanlines: the top of a glyph row, its middle where
    // ink should be, and two rows further down.
    peek(0); peek(4); peek(8); peek(20);

    row("Scanline buffers:  ", s[3], "");
    row("Alarm / IRQ:       ", s[6], "");
    row("                   ", s[7], " (irq)");
    row("Beam is at line:   ", s[4], "");
    row("Built up to line:  ", s[5], "");
    row("Pumps:             ", s[1], "");
    row("Scanlines built:   ", s[2], "");
    row("Underruns:         ", s[0], "");

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

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

void module_main(void) {
    uint32_t s[4];

    if (myrtos_video_stats(s) < 0) {
        myrtos_write_str(MYRTOS_STDOUT,
            "This build draws into a framebuffer; there is nothing to keep up with.\n"
            "Configure with -DMYRTOS_VIDEO=chargen to use the character generator.\n");
        return;
    }

    row("Scanline buffers:  ", s[3], "");
    row("Pumps:             ", s[1], "");
    row("Scanlines built:   ", s[2], "");
    row("Underruns:         ", s[0], "");

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

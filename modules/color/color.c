#include "../../common/myrtos_abi.h"

// color -- what the screen writes in from here on.
//
//   color               the sixteen, and a reminder of the names
//   color yellow        text only
//   color yellow blue   text and background
//   color reset
//
// The colour is not this process's. It belongs to the terminal, so it outlasts
// the command that set it and the next thing anything prints comes out in it --
// which is the whole point, and also why "reset" has to exist.

// A table of pointers, which is what this wants to be. Every entry is an
// absolute address the linker wrote in and the loader fixes up -- sixteen
// relocations, four bytes each, carried on the card and not in RAM. It was an
// array of arrays for as long as the loader could not do that, and every name
// cost fourteen bytes whether it needed them or not.
static const char *const names[16] = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
    "grey", "brightred", "brightgreen", "brightyellow",
    "brightblue", "brightmagenta", "brightcyan", "brightwhite",
};

static bool same(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return !*a && !*b;
}

static uint32_t by_name(const char *s) {
    for (uint32_t i = 0; i < 16; i++) if (same(s, names[i])) return i;
    return MYRTOS_KEEP;
}

// Every colour as a band of its own name, on the background it would give. A
// chart rather than a list: the only useful question about a colour is whether
// it is legible, and that cannot be answered in words.
static void chart(int32_t c) {
    myrtos_line_t l;
    for (uint32_t i = 0; i < 16; i++) {
        myrtos_line_reset(&l);
        // The name, in the colour, on black -- then the same colour as a
        // background with black on it. One write, so nothing gets in between
        // the escape and the text it is meant to colour.
        myrtos_line_colour(&l, i, MYRTOS_BLACK);
        myrtos_line_str(&l, "  ");
        myrtos_line_str(&l, names[i]);
        uint32_t n = 0;
        while (names[i][n]) n++;
        while (n++ < 15) myrtos_line_str(&l, " ");   // so the blocks line up
        myrtos_line_colour(&l, MYRTOS_BLACK, i);
        myrtos_line_str(&l, "      ");
        myrtos_line_plain(&l);
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(c, &l);
    }
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: color <text> [<background>], or color reset\n")) return;

    const int32_t c = MYRTOS_STDOUT;

    if (argc < 2) {
        chart(c);
        myrtos_write_str(c, "usage: color <text> [<background>], or color reset\r\n");
        return;
    }

    if (same(argv[1], "reset")) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_plain(&l);
        myrtos_line_flush(c, &l);
        return;
    }

    uint32_t fg = by_name(argv[1]);
    uint32_t bg = (argc > 2) ? by_name(argv[2]) : MYRTOS_KEEP;

    if (fg == MYRTOS_KEEP || (argc > 2 && bg == MYRTOS_KEEP)) {
        myrtos_write_str(c, "color: no such colour\r\n");
        return;
    }
    myrtos_colour(c, fg, bg);
}

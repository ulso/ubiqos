#include "../../common/ubiqos_abi.h"

// font -- which typeface the screen draws in.
//
//   font          what is current, and what else there is
//   font 6x12     switch, and say what grid that gives
//
// A font is named by its cell and by nothing else: "6x12" is the two numbers,
// so there is no table of names to keep in step with the table of glyphs.
//
// Switching clears the screen. The grid changes under the text and there is no
// sensible way to reflow eighty columns into a hundred and six, so what comes
// back is the shell's next prompt.

static void describe(ubiqos_line_t *l, const ubiqos_confont_t *f) {
    ubiqos_line_u32(l, f->cell_w);
    ubiqos_line_str(l, "x");
    ubiqos_line_u32(l, f->cell_h);
    ubiqos_line_str(l, ", ");
    ubiqos_line_u32(l, f->cols);
    ubiqos_line_str(l, " by ");
    ubiqos_line_u32(l, f->rows);
}

// "6x12" into its two numbers. Anything else is not a font name.
static bool parse_cell(const char *s, uint32_t *w, uint32_t *h) {
    uint32_t v = 0, n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
    if (!n || (*s != 'x' && *s != 'X')) return false;
    *w = v; s++;
    v = n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
    if (!n || *s) return false;
    *h = v;
    return true;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: font [WxH]\n\nShows the console fonts, or selects one.\n")) return;

    ubiqos_line_t line;
    ubiqos_confont_t cur;

    if (ubiqos_console_font_info(-1, &cur) < 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "font: no console\n");
        return;
    }

    if (argc < 2) {
        for (uint32_t i = 0; i < cur.count; i++) {
            ubiqos_confont_t f;
            if (ubiqos_console_font_info((int32_t)i, &f) < 0) continue;
            ubiqos_line_reset(&line);
            ubiqos_line_str(&line, i == cur.index ? "* " : "  ");
            describe(&line, &f);
            ubiqos_line_str(&line, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &line);
        }
        return;
    }

    uint32_t w, h;
    if (!parse_cell(argv[1], &w, &h)) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: font [WxH]\n");
        return;
    }

    for (uint32_t i = 0; i < cur.count; i++) {
        ubiqos_confont_t f;
        if (ubiqos_console_font_info((int32_t)i, &f) < 0) continue;
        if (f.cell_w != w || f.cell_h != h) continue;
        ubiqos_console_font((int32_t)i, &f);
        // Written for the serial shell, which is still there to read it. On the
        // screen the clear takes this with it, and the prompt says it took.
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "font: ");
        describe(&line, &f);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
        return;
    }
    ubiqos_write_str(UBIQOS_STDOUT, "font: no such font\n");
}

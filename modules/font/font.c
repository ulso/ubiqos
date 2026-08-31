#include "../../common/myrtos_abi.h"

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

static void describe(myrtos_line_t *l, const myrtos_confont_t *f) {
    myrtos_line_u32(l, f->cell_w);
    myrtos_line_str(l, "x");
    myrtos_line_u32(l, f->cell_h);
    myrtos_line_str(l, ", ");
    myrtos_line_u32(l, f->cols);
    myrtos_line_str(l, " by ");
    myrtos_line_u32(l, f->rows);
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
    myrtos_line_t line;
    myrtos_confont_t cur;

    if (myrtos_console_font_info(-1, &cur) < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "font: no console\n");
        return;
    }

    if (argc < 2) {
        for (uint32_t i = 0; i < cur.count; i++) {
            myrtos_confont_t f;
            if (myrtos_console_font_info((int32_t)i, &f) < 0) continue;
            myrtos_line_reset(&line);
            myrtos_line_str(&line, i == cur.index ? "* " : "  ");
            describe(&line, &f);
            myrtos_line_str(&line, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &line);
        }
        return;
    }

    uint32_t w, h;
    if (!parse_cell(argv[1], &w, &h)) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: font [WxH]\n");
        return;
    }

    for (uint32_t i = 0; i < cur.count; i++) {
        myrtos_confont_t f;
        if (myrtos_console_font_info((int32_t)i, &f) < 0) continue;
        if (f.cell_w != w || f.cell_h != h) continue;
        myrtos_console_font((int32_t)i, &f);
        // Written for the serial shell, which is still there to read it. On the
        // screen the clear takes this with it, and the prompt says it took.
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "font: ");
        describe(&line, &f);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
        return;
    }
    myrtos_write_str(MYRTOS_STDOUT, "font: no such font\n");
}

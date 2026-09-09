#include "../../common/myrtos_abi.h"

// vec -- draw line segments over the console.
//
//   vec clear
//   vec line x0 y0 x1 y1 [colour]
//   vec demo
//
// Colours are RGB332: 0xe0 red, 0x1c green, 0x03 blue, 0xff white.

static uint32_t num(const char *s, uint32_t dflt) {
    if (!s || !*s) return dflt;
    uint32_t v = 0, base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9')      d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    return v;
}

// Curve stitching: a family of chords whose envelope is a parabola. Every one
// is a straight line, which is the point -- the curve is drawn by segments that
// are not curved, and the scanline renderer never has to know.
static void demo(void) {
    // Six chords per corner, not twenty-four. The curve is still readable and
    // the active list stays inside what one scanline can afford -- see
    // MYRTOS_VEC_ACTIVE_MAX. The first version drew a hundred segments and the
    // display ate the processor the keyboard needed.
    const int32_t W = 640, H = 480, N = 6;
    myrtos_vec_clear();

    for (int32_t i = 0; i <= N; i++) {
        int32_t a = i * W / N;
        int32_t b = i * H / N;
        myrtos_vec_line(a, 0, 0, H - b, 0x1c);          // top-left, green
        myrtos_vec_line(W - a, 0, W, H - b, 0xe0);      // top-right, red
        myrtos_vec_line(a, H - 1, 0, b, 0x03);          // bottom-left, blue
        myrtos_vec_line(W - a, H - 1, W, b, 0xff);      // bottom-right, white
    }
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
        "vec -- line segments drawn over the console\n"
        "  vec clear\n"
        "  vec line x0 y0 x1 y1 [colour]\n"
        "  vec demo\n"))
        return;

    if (argc < 2) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "segments: ");
        myrtos_line_u32(&l, (uint32_t)myrtos_vec_count());
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        return;
    }

    const char *cmd = argv[1];

    if (cmd[0] == 'c') {
        if (myrtos_vec_clear() < 0)
            myrtos_write_str(MYRTOS_STDOUT, "vec: this build has no scanline renderer\n");
        return;
    }

    if (cmd[0] == 'd') { demo(); return; }

    if (cmd[0] == 'l') {
        if (argc < 6) { myrtos_write_str(MYRTOS_STDOUT, "vec line x0 y0 x1 y1 [colour]\n"); return; }
        int32_t r = myrtos_vec_line((int32_t)num(argv[2], 0), (int32_t)num(argv[3], 0),
                                    (int32_t)num(argv[4], 0), (int32_t)num(argv[5], 0),
                                    num(argc >= 7 ? argv[6] : 0, 0xff));
        if (r < 0)
            myrtos_write_str(MYRTOS_STDOUT, "vec: refused -- list full, or off the top or bottom\n");
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "vec: clear, line or demo\n");
}

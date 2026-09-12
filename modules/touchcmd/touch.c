#include "../../common/myrtos_abi.h"

// touch -- where the fingers are, as fast as the screen will say.
//
// Exists so the driver can be believed. A touch controller that is wired wrong
// answers a read perfectly and reports nothing, and nothing is also what an
// untouched screen reports -- so the only way to tell them apart is to put a
// finger on the glass and watch.
//
// Ctrl-C ends it.

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv, "touch -- print the touch points until Ctrl-C"))
        return;

    int32_t fd = myrtos_open("touch");
    if (fd < 0) { say("touch: no touch device\r\n"); return; }

    uint32_t was = 0xffffffffu;
    for (;;) {
        myrtos_touch_t t;
        int32_t n = myrtos_read(fd, (uint8_t *)&t, sizeof t);
        if (n == (int32_t)sizeof t) {
            // Printed on a change rather than every poll: a finger held still
            // would otherwise fill the screen and hide what moved.
            bool changed = (t.points != was);
            if (!changed && t.points) changed = true;   // a moving finger is news
            if (changed) {
                myrtos_line_t l;
                myrtos_line_reset(&l);
                if (!t.points) {
                    myrtos_line_str(&l, "up\r\n");
                } else {
                    for (uint32_t i = 0; i < t.points; i++) {
                        myrtos_line_str(&l, i ? "   " : "");
                        myrtos_line_u32(&l, t.p[i].x);
                        myrtos_line_str(&l, ",");
                        myrtos_line_u32(&l, t.p[i].y);
                    }
                    myrtos_line_str(&l, "\r\n");
                }
                myrtos_line_flush(MYRTOS_STDOUT, &l);
            }
            was = t.points;
        }
        myrtos_sleep(30);          // thirty a second is more than a finger needs
    }
}

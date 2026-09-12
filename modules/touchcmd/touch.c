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

    int32_t fd = myrtos_open("/dev/touch");
    if (fd < 0) { say("touch: no /dev/touch\r\n"); return; }

    // Said first, because the numbers below mean nothing without it. The chip
    // holds its own coordinate range and it need not be the panel's, so this is
    // what decides whether anything has to be scaled at all.
    myrtos_touch_range_t r;
    if (myrtos_getstat(fd, MYRTOS_SS_TOUCH_RANGE, &r, sizeof r) == 0) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "range ");
        myrtos_line_u32(&l, r.width);
        myrtos_line_str(&l, " by ");
        myrtos_line_u32(&l, r.height);
        myrtos_line_str(&l, ", ");
        myrtos_line_u32(&l, r.points);
        myrtos_line_str(&l, " points, firmware ");
        myrtos_line_u32(&l, r.firmware);
        myrtos_line_str(&l, ", config ");
        myrtos_line_u32(&l, r.config);
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    } else {
        say("range: the driver will not say\r\n");
    }

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

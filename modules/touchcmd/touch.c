#include "../../common/ubiqos_abi.h"

// touch -- where the fingers are, as fast as the screen will say.
//
// Exists so the driver can be believed. A touch controller that is wired wrong
// answers a read perfectly and reports nothing, and nothing is also what an
// untouched screen reports -- so the only way to tell them apart is to put a
// finger on the glass and watch.
//
// Ctrl-C ends it.

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv, "touch -- print the touch points until Ctrl-C"))
        return;

    int32_t fd = ubiqos_open("/dev/touch");
    if (fd < 0) { say("touch: no /dev/touch\r\n"); return; }

    // Said first, because the numbers below mean nothing without it. The chip
    // holds its own coordinate range and it need not be the panel's, so this is
    // what decides whether anything has to be scaled at all.
    ubiqos_touch_range_t r;
    if (ubiqos_getstat(fd, UBIQOS_SS_TOUCH_RANGE, &r, sizeof r) == 0) {
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "range ");
        ubiqos_line_u32(&l, r.width);
        ubiqos_line_str(&l, " by ");
        ubiqos_line_u32(&l, r.height);
        ubiqos_line_str(&l, ", ");
        ubiqos_line_u32(&l, r.points);
        ubiqos_line_str(&l, " points, firmware ");
        ubiqos_line_u32(&l, r.firmware);
        ubiqos_line_str(&l, ", config ");
        ubiqos_line_u32(&l, r.config);
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    } else {
        say("range: the driver will not say\r\n");
    }

    uint32_t was = 0xffffffffu;
    for (;;) {
        ubiqos_touch_t t;
        int32_t n = ubiqos_read(fd, (uint8_t *)&t, sizeof t);
        if (n == (int32_t)sizeof t) {
            // Printed on a change rather than every poll: a finger held still
            // would otherwise fill the screen and hide what moved.
            bool changed = (t.points != was);
            if (!changed && t.points) changed = true;   // a moving finger is news
            if (changed) {
                ubiqos_line_t l;
                ubiqos_line_reset(&l);
                if (!t.points) {
                    ubiqos_line_str(&l, "up\r\n");
                } else {
                    for (uint32_t i = 0; i < t.points; i++) {
                        ubiqos_line_str(&l, i ? "   " : "");
                        ubiqos_line_u32(&l, t.p[i].x);
                        ubiqos_line_str(&l, ",");
                        ubiqos_line_u32(&l, t.p[i].y);
                    }
                    ubiqos_line_str(&l, "\r\n");
                }
                ubiqos_line_flush(UBIQOS_STDOUT, &l);
            }
            was = t.points;
        }
        ubiqos_sleep(30);          // thirty a second is more than a finger needs
    }
}

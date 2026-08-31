#include "../../common/myrtos_abi.h"

// cu -- sit between a terminal and another device.
//
//   cu acm          talk to whatever is in the USB socket
//   cu acm AT+DUAL  send one line first, then talk
//
// Named after the Unix program that did this to a modem, and doing the same
// job: what you type goes out, what comes back is printed, and neither end
// knows the other is not a wire.
//
// There is no escape sequence to leave with. Ctrl-C ends it, the way it ends
// anything else running in front of the shell -- which is the point of having
// put that in the driver rather than in a key this program has to watch for.

#define CHUNK 64

static int32_t pump(int32_t from, int32_t to) {
    uint8_t buf[CHUNK];
    while (myrtos_readable(from) > 0) {
        int32_t n = myrtos_read(from, buf, sizeof(buf));
        if (n <= 0) return 0;
        for (int32_t off = 0; off < n; ) {
            int32_t w = myrtos_write(to, buf + off, (uint32_t)(n - off));
            if (w < 0) return -1;                  // the device is gone
            if (w == 0) { myrtos_sleep(1); continue; }
            off += w;
        }
    }
    return 0;
}

void module_main(int argc, char **argv) {
    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: cu <device> [line to send]\r\n");
        return;
    }

    int32_t dev = myrtos_open(argv[1]);
    if (dev < 0) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "cu: no device called ");
        myrtos_line_str(&l, argv[1]);
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "connected; ctrl-C to stop\r\n");

    // Anything after the device name is sent as one line, so that a command
    // that is always the same need not be typed every time.
    if (argc > 2) {
        for (int i = 2; i < argc; i++) {
            if (i > 2) myrtos_write(dev, " ", 1);
            myrtos_write_str(dev, argv[i]);
        }
        myrtos_write(dev, "\r\n", 2);
    }

    // Neither side may be waited on: a read blocks when there is nothing, and
    // blocking on the keyboard would mean the dongle's answer waits for a
    // keystroke that may never come. So both are asked before either is read.
    for (;;) {
        if (pump(MYRTOS_STDIN, dev) < 0) {
            myrtos_write_str(MYRTOS_STDOUT, "\r\ncu: nothing on that device\r\n");
            return;
        }
        pump(dev, MYRTOS_STDOUT);
        myrtos_sleep(2);
    }
}

#include "../../common/ubiqos_abi.h"

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
    while (ubiqos_readable(from) > 0) {
        int32_t n = ubiqos_read(from, buf, sizeof(buf));
        if (n <= 0) return 0;
        for (int32_t off = 0; off < n; ) {
            int32_t w = ubiqos_write(to, buf + off, (uint32_t)(n - off));
            if (w < 0) return -1;                  // the device is gone
            if (w == 0) { ubiqos_sleep(1); continue; }
            off += w;
        }
    }
    return 0;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: cu <device> [line to send]\n\nTalks to a serial device. Ctrl-C to stop.\n")) return;

    if (argc < 2) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: cu <device> [line to send]\r\n");
        return;
    }

    int32_t dev = ubiqos_open(argv[1]);
    if (dev < 0) {
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "cu: no device called ");
        ubiqos_line_str(&l, argv[1]);
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        return;
    }

    ubiqos_write_str(UBIQOS_STDOUT, "connected; ctrl-C to stop\r\n");

    // Anything after the device name is sent as one line, so that a command
    // that is always the same need not be typed every time.
    if (argc > 2) {
        for (int i = 2; i < argc; i++) {
            if (i > 2) ubiqos_write(dev, " ", 1);
            ubiqos_write_str(dev, argv[i]);
        }
        ubiqos_write(dev, "\r\n", 2);
    }

    // Neither side may be waited on: a read blocks when there is nothing, and
    // blocking on the keyboard would mean the dongle's answer waits for a
    // keystroke that may never come. So both are asked before either is read.
    for (;;) {
        if (pump(UBIQOS_STDIN, dev) < 0) {
            ubiqos_write_str(UBIQOS_STDOUT, "\r\ncu: nothing on that device\r\n");
            return;
        }
        pump(dev, UBIQOS_STDOUT);
        ubiqos_sleep(2);
    }
}

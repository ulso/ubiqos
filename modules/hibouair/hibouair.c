// hibouair -- use BleuIO dongle to scan for HibouAir sensor data.
//
// Ctrl-C ends scanning.

#include <stdbool.h>
#include "../../common/myrtos_stdio.h"

#define AT_ECHO_OFF    "ATE0\r"
#define AT_ECHO_ON     "ATE1\r"
#define AT_VERBOSE_OFF "ATV0\r"
#define AT_VERBOSE_ON  "ATV1\r"

#define CHUNK          128

// Every piece of per-process state the C library keeps: errno, the streams, and
// what strtok remembers. One line, and it has to be here rather than in the
// header -- see the note by the macro in myrtos_stdio.h.
MYRTOS_LIBC_DEFINE

__thread uint8_t buf[CHUNK];

/**
 * flush_input -- empty receive buffer.
 */
static int32_t flush_input(int32_t dev)
{
    while (myrtos_readable(dev) > 0) {
        int32_t n = myrtos_read(dev, buf, sizeof(buf));
        if (n < 0)
            return -1;
    }

    return 0;
}

/**
 * send_command --  send AT command to BLE dongle.
 */
static int32_t send_command(int32_t dev, const char *cmd)
{
    if (flush_input(dev) < 0)
        return -1;

    return write(dev, cmd, strlen(cmd));
}

/**
 * set_echo -- turn BLE dongle echo on or off.
 */
static int32_t set_echo(int32_t dev, bool on)
{
    return send_command(dev, on ? AT_ECHO_ON : AT_ECHO_OFF);
}

/**
 * set_verbose -- turn BLE dongle verbose result mode on or off.
 */
static int32_t set_verbose(int32_t dev, bool on)
{
    return send_command(dev, on ? AT_VERBOSE_ON : AT_VERBOSE_OFF);
}


static int32_t pump(int32_t from, int32_t to)
{
    uint8_t buf[CHUNK];
    while (myrtos_readable(from) > 0) {
        int32_t n = myrtos_read(from, buf, sizeof(buf));
        if (n <= 0)
            return 0;
        for (int32_t off = 0; off < n;) {
            int32_t w = myrtos_write(to, buf + off, (uint32_t)(n - off));
            if (w < 0)
                return -1;   // the device is gone
            if (w == 0) {
                myrtos_sleep(1);
                continue;
            }
            off += w;
        }
    }
    return 0;
}

void module_main(int argc, char **argv)
{
    int32_t dev = myrtos_open("/dev/acm");
    if (dev < 0) {
        printf("hibouair: can't open device\n");
        return;
    }

    printf("connected; ctrl-C to stop\n");

    if (set_echo(dev, false) < 0) {
        printf("Failed to turn echo off.\n");
        return;
    }
    
    if (set_verbose(dev, true) < 0) {
        printf("Failed to turn verbose mode on.\n");
        return;
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

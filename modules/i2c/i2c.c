#include "../../common/myrtos_abi.h"

// i2c -- look at the bus, and read from something on it.
//
//   i2c                  every address that answers
//   i2c ADDR             does this one answer
//   i2c ADDR REG [N]     N bytes from a register, one by default
//   i2c ADDR - N         N bytes with no register named at all
//
// Addresses and registers are hex, with or without 0x, because that is how
// every datasheet writes them.
//
// Writing the register number and reading the answer is ONE transaction with a
// repeated start in the middle, not two -- see the note in the driver. Two
// would let something else take the bus between naming the register and
// reading it, and on a bus with more than one master that is a real hazard
// rather than a theoretical one.

MYRTOS_MEM_SIZE(8192);

static bool from_hex(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0, n = 0;
    for (; *s; s++, n++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
        else return false;
        v = v * 16 + d;
    }
    *out = v;
    return n > 0;
}

static void put_hex2(myrtos_line_t *l, uint32_t v) { myrtos_line_hex_byte(l, v); }

// One exchange: the header, then what to send. Answers true if anybody
// acknowledged.
static bool xfer(int32_t fd, uint8_t addr, const uint8_t *w, uint8_t nw, uint8_t nr) {
    uint8_t buf[4 + 16];
    if (nw > 16) return false;
    buf[0] = addr; buf[1] = nw; buf[2] = nr; buf[3] = 0;
    for (uint8_t i = 0; i < nw; i++) buf[4 + i] = w[i];
    return myrtos_write(fd, buf, (uint32_t)(4 + nw)) >= 0;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: i2c [ADDR [REG [N]]]\n\n"
            "  (none)          every address that answers\n"
            "  ADDR            does this one answer\n"
            "  ADDR REG [N]    N bytes from a register, one by default\n"
            "  ADDR - N        N bytes with no register named -- for a device that\n"
            "                  has no registers, like an IMU speaking SHTP\n\n"
            "Addresses and registers are hex, 0x optional.\n"))
        return;

    int32_t fd = myrtos_open("/dev/i2c");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "i2c: no /dev/i2c\n"); return; }

    myrtos_line_t l;

    if (argc == 1) {
        // 0x08 to 0x77. Below and above are reserved by the specification and
        // addressing them means nothing -- a scan that walks them is a scan
        // that reports noise.
        uint32_t found = 0;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "scanning 0x08-0x77:");
        for (uint32_t a = 0x08; a <= 0x77; a++) {
            if (!xfer(fd, (uint8_t)a, 0, 0, 1)) continue;
            myrtos_line_str(&l, " ");
            put_hex2(&l, a);
            found++;
        }
        if (!found) myrtos_line_str(&l, " nothing answered");
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        myrtos_close(fd);
        return;
    }

    uint32_t addr;
    if (!from_hex(argv[1], &addr) || addr < 0x08 || addr > 0x77) {
        myrtos_write_str(MYRTOS_STDERR, "i2c: address must be hex, 08 to 77\n");
        myrtos_close(fd);
        return;
    }

    if (argc == 2) {
        myrtos_line_reset(&l);
        put_hex2(&l, addr);
        myrtos_line_str(&l, xfer(fd, (uint8_t)addr, 0, 0, 1) ? " answers\n" : " no answer\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        myrtos_close(fd);
        return;
    }

    // "-" for a register means there is no register. Plenty of devices have
    // none: the BNO08x on this bus speaks SHTP, where a read gets a four-byte
    // header saying how long the rest is, and naming a register first would
    // only confuse it.
    bool raw = argv[2][0] == '-' && !argv[2][1];

    uint32_t reg = 0, count = 1;
    if ((!raw && (!from_hex(argv[2], &reg) || reg > 0xff))
        || (argc > 3 && (!from_hex(argv[3], &count) || !count || count > MYRTOS_I2C_MAX_READ))) {
        myrtos_write_str(MYRTOS_STDERR, "i2c: register is hex 00-ff or -, count 1 to 64\n");
        myrtos_close(fd);
        return;
    }

    uint8_t r = (uint8_t)reg;
    if (!xfer(fd, (uint8_t)addr, raw ? 0 : &r, raw ? 0 : 1, (uint8_t)count)) {
        myrtos_write_str(MYRTOS_STDERR, "i2c: no answer\n");
        myrtos_close(fd);
        return;
    }

    uint8_t got[MYRTOS_I2C_MAX_READ];
    int32_t n = myrtos_read(fd, got, count);
    myrtos_line_reset(&l);
    for (int32_t i = 0; i < n; i++) { put_hex2(&l, got[i]); myrtos_line_str(&l, " "); }
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
    myrtos_close(fd);
}

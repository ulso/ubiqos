#include "../../common/myrtos_abi.h"

// ehstat -- what the ESP-Hosted link has been doing.
//
// The transport is a thread and a pair of pins, neither of which can be looked
// at from a shell. This is the window into it, and the first question it has to
// answer is the only one that matters at this stage: did the co-processor say
// anything at all, and was it a frame or noise.

static const char *if_name(int i) {
    switch (i) {
    case 0: return "invalid";
    case 1: return "station";
    case 2: return "soft AP";
    case 3: return "serial (RPC)";
    case 4: return "bluetooth";
    case 5: return "private";
    case 6: return "test";
    case 7: return "ethernet";
    default: return "dummy";
    }
}

// One line, written and flushed. myrtos_line_t holds 96 characters and stops
// there without saying so: the first version of this built six counters into
// one buffer and printed four and a half of them.
static void num(const char *label, uint32_t v) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, label);
    myrtos_line_u32(&l, v);
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

// What the co-processor announces itself with. A hexdump would be honest and
// useless: these are the terms of the link, and the terms are the point.
static const char *tlv_name(uint8_t t) {
    switch (t) {
    case 0x11: return "capabilities";
    case 0x12: return "chip";
    case 0x13: return "raw throughput test";
    case 0x14: return "its receive queue";
    case 0x15: return "its send queue";
    case 0x16: return "extended capabilities";
    case 0x17: return "firmware";
    case 0x19: return "feature capabilities";
    case 0x1a: return "RPC version";
    case 0x20: return "proposed header version";
    case 0x22: return "proposed RPC version";
    case 0x24: return "RPC request endpoint";
    case 0x25: return "RPC event endpoint";
    default:   return 0;
    }
}

static void caps_in_words(uint8_t caps) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "      ");
    if (caps & 0x20u) myrtos_line_str(&l, "WLAN over SPI  ");
    if (caps & 0x80u) myrtos_line_str(&l, "checksum on  ");
    if (caps & 0x01u) myrtos_line_str(&l, "WLAN over SDIO  ");
    if (caps & 0x08u) myrtos_line_str(&l, "BLE  ");
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

// The version is four bytes, and they read patch, minor, major -- which is the
// order that makes 3.0.7 out of 07 00 03 00 and would otherwise read as a very
// large number nobody would question.
static void version_in_words(const uint8_t *v) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "      ");
    myrtos_line_u32(&l, v[2]);
    myrtos_line_str(&l, ".");
    myrtos_line_u32(&l, v[1]);
    myrtos_line_str(&l, ".");
    myrtos_line_u32(&l, v[0]);
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

static void decode_priv(const uint8_t *p, uint32_t n) {
    // A private frame is an event byte, its length, and then a run of
    // type/length/value. Event 0x22 is the one that says the slave is up.
    if (n < 2) return;
    say(p[0] == 0x22 ? "\r\nthe co-processor announced itself\r\n"
                     : "\r\na private event\r\n");

    uint32_t at = 2, end = (uint32_t)p[1] + 2u;
    if (end > n) end = n;

    while (at + 2 <= end) {
        uint8_t type = p[at], len = p[at + 1];
        const uint8_t *val = p + at + 2;
        if (at + 2u + len > end) break;

        const char *name = tlv_name(type);
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "  ");
        if (name) myrtos_line_str(&l, name);
        else { myrtos_line_str(&l, "tag "); myrtos_line_hex_byte(&l, type); }
        myrtos_line_str(&l, "  ");
        for (uint8_t i = 0; i < len; i++) {
            myrtos_line_hex_byte(&l, val[i]);
            myrtos_line_str(&l, " ");
        }
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);

        if (type == 0x11 && len == 1) caps_in_words(val[0]);
        if (type == 0x17 && len == 4) version_in_words(val);

        at += 2u + len;
    }
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: ehstat\n\nWhat the ESP-Hosted SPI link has carried.\n")) return;

    int32_t dev = myrtos_open("/dev/eh");
    if (dev < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "ehstat: no /dev/eh\r\n");
        return;
    }

    myrtos_eh_stats_t s;
    if (myrtos_getstat(dev, MYRTOS_SS_EH_STATS, &s, sizeof(s)) < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "ehstat: the driver would not say\r\n");
        myrtos_close(dev);
        return;
    }
    myrtos_close(dev);

    num("transactions   ", s.transactions);
    num("frames in      ", s.frames);
    num("dummies in     ", s.dummies);
    num("frames out     ", s.sent);
    num("bad checksum   ", s.bad_checksum);
    num("bad header     ", s.bad_header);
    num("dma timeouts   ", s.dma_timeouts);
    num("control lost   ", s.inbox_lost);
    num("network lost   ", s.netbox_lost);
    num("tx refused     ", s.nettx_refused);

    say("\r\na frame waiting to go out, microseconds\r\n");
    num("  last         ", s.txwait_us);
    num("  worst        ", s.worst_txwait_us);
    num("  turns ready  ", s.turns_ready);
    num("  turns blocked", s.turns_blocked);
    say("\r\nthe co-processor offering a turn\r\n");
    num("  handshake high", s.hs_high);
    num("  handshake low ", s.hs_low);
    num("  data ready hi ", s.dr_high);
    num("  data ready lo ", s.dr_low);
    num("  us since last ", s.turn_gap_us);
    num("  worst gap     ", s.worst_turn_gap_us);
    say("\r\none exchange, microseconds\r\n");
    num("  on the wire  ", s.wall_us);
    num("  worst        ", s.worst_wall_us);
    num("  on the CPU   ", s.cpu_us);
    num("  worst        ", s.worst_cpu_us);

    say("\r\nby interface\r\n");
    for (int i = 0; i < 9; i++) {
        if (!s.by_if[i]) continue;
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "  ");
        myrtos_line_str(&l, if_name(i));
        myrtos_line_str(&l, "  ");
        myrtos_line_u32(&l, s.by_if[i]);
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    // The private frame is the co-processor announcing itself, and at this
    // stage its bytes are more useful than any reading of them: it is the
    // difference between a link that works and one that merely does not fail.
    if (s.bad_seen) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "\r\nthe first header it could not read\r\n ");
        for (int i = 0; i < 12; i++) {
            myrtos_line_str(&l, " ");
            myrtos_line_hex_byte(&l, s.first_bad[i]);
        }
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    if (!s.last_priv_len) {
        say("\r\nnothing on the private interface yet\r\n");
        return;
    }
    decode_priv(s.last_priv, s.last_priv_len);
}

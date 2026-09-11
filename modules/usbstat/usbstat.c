#include "../../common/myrtos_abi.h"

// usbstat -- what the PIO USB host believes, printed by the machine itself.
//
// The point is what it is NOT. All of this was read with J-Link on 3 Sep 2026,
// and that turned out to be the wrong instrument: memory access on Hazard3 goes
// through a halt, and a bus bit-banged in software loses transactions while the
// processor is stopped. The probe kept producing the failure it was brought in
// to observe, and hours went into chasing it. A running machine can say all of
// this without stopping, over the UART or the console, and that is this.
//
// Read it twice a second apart. A single frame proves very little: failed goes
// back to zero whenever a new transfer is queued, so a device that is simply
// gone still shows zeroes between the climbs.

static void kv(const char *label, uint32_t v) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, label);
    myrtos_line_u32(&l, v);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

void module_main(void) {
    kv("keys pushed:    ", myrtos_usbinfo(MYRTOS_USB_KEYSIN));
    kv("rearms:         ", myrtos_usbinfo(MYRTOS_USB_REARMS));
    kv("recoveries:     ", myrtos_usbinfo(MYRTOS_USB_RECOVERIES));
    kv("cdc re-arms:    ", myrtos_usbinfo(MYRTOS_USB_CDCREARMS));

    // What the HOST did to this device's bus. Zeroes after a night of the Mac
    // sleeping would say the board never saw it go -- which is an answer, and
    // a different one from having seen it and failed to come back.
    kv("host suspends:  ", myrtos_usbinfo(MYRTOS_USB_SUSPENDS));
    kv("host resumes:   ", myrtos_usbinfo(MYRTOS_USB_RESUMES));
    kv("host mounts:    ", myrtos_usbinfo(MYRTOS_USB_MOUNTS));
    kv("host unmounts:  ", myrtos_usbinfo(MYRTOS_USB_UNMOUNTS));
    kv("last one at ms: ", myrtos_usbinfo(MYRTOS_USB_LASTEVENT));
    if (myrtos_usbinfo(MYRTOS_USB_CDCGIVEUP))
        myrtos_write_str(MYRTOS_STDOUT, "cdc:            given up, not asking again\n");

    uint32_t rk = myrtos_usbinfo(MYRTOS_USB_REPEATKEY);
    myrtos_line_t l;
    myrtos_line_reset(&l);
    // The USB network device, which is a different thing from the host side
    // above: this is what the Mac sees when it enumerates this board.
    {
        uint32_t n[40];
        if (myrtos_netdev_stats(n) == 0) {
            // What was observed, not a state that cannot be asked for: NCM
            // gives no link callback, so a frame arriving is the evidence.
            myrtos_line_str(&l, "net: lwIP ");
            myrtos_line_str(&l, n[0] ? "up" : "not started");
            myrtos_line_str(&l, ", address ");
            if (n[2]) {
                for (int b = 3; b >= 0; b--) {
                    myrtos_line_u32(&l, (n[2] >> (b * 8)) & 0xffu);
                    if (b) myrtos_line_str(&l, ".");
                }
            } else {
                myrtos_line_str(&l, "none yet");
            }
            myrtos_line_str(&l, "\nnet frames:     in ");
            myrtos_line_u32(&l, n[1]);
            myrtos_line_str(&l, ", out ");
            myrtos_line_u32(&l, n[3]);
            myrtos_line_str(&l, ", dropped ");
            myrtos_line_u32(&l, n[4]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);

            // Two lines, because myrtos_line_t holds 96 bytes and one long
            // enough to say all of this is silently cut in half -- which read
            // as "lwip: link 0 drop" and looked like a finding.
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "lwip: arp in ");
            myrtos_line_u32(&l, n[9]);
            myrtos_line_str(&l, " out ");
            myrtos_line_u32(&l, n[10]);
            myrtos_line_str(&l, ", ip in ");
            myrtos_line_u32(&l, n[11]);
            myrtos_line_str(&l, " drop ");
            myrtos_line_u32(&l, n[12]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);

            myrtos_line_reset(&l);
            myrtos_line_str(&l, "      icmp in ");
            myrtos_line_u32(&l, n[13]);
            myrtos_line_str(&l, " out ");
            myrtos_line_u32(&l, n[14]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "      on the wire: arp ");
            myrtos_line_u32(&l, n[16]);
            myrtos_line_str(&l, ", ipv4 ");
            myrtos_line_u32(&l, n[17]);
            myrtos_line_str(&l, ", other ");
            myrtos_line_u32(&l, n[18]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "      addressed to us ");
            myrtos_line_u32(&l, n[19]);
            myrtos_line_str(&l, ", of which icmp ");
            myrtos_line_u32(&l, n[20]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "sock: served ");
            myrtos_line_u32(&l, n[21]);
            myrtos_line_str(&l, ", accepts queued ");
            myrtos_line_u32(&l, n[22]);
            myrtos_line_str(&l, " taken ");
            myrtos_line_u32(&l, n[23]);
            myrtos_line_str(&l, "\n      bytes in ");
            myrtos_line_u32(&l, n[24]);
            myrtos_line_str(&l, " out ");
            myrtos_line_u32(&l, n[25]);
            myrtos_line_str(&l, ", last refusal ");
            myrtos_line_u32(&l, n[26]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "      lwip gave us ");
            myrtos_line_u32(&l, n[29]);
            myrtos_line_str(&l, " pbufs, ");
            myrtos_line_u32(&l, n[30]);
            myrtos_line_str(&l, " bytes\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, " tcp: recv ");
            myrtos_line_u32(&l, n[31]);
            myrtos_line_str(&l, " drop ");
            myrtos_line_u32(&l, n[32]);
            myrtos_line_str(&l, " err ");
            myrtos_line_u32(&l, n[33]);
            myrtos_line_str(&l, "\n      chkerr ");
            myrtos_line_u32(&l, n[34]);
            myrtos_line_str(&l, " proterr ");
            myrtos_line_u32(&l, n[35]);
            myrtos_line_str(&l, " xmit ");
            myrtos_line_u32(&l, n[36]);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
            // What the server actually replied, which is the one value that
            // separates "the server refused" from "the caller saw a refusal".
            myrtos_line_str(&l, " last op ");
            myrtos_line_u32(&l, n[27]);
            myrtos_line_str(&l, " -> 0x");
            myrtos_line_hex(&l, n[28]);
            myrtos_line_str(&l, "\n");
        }
    }
    myrtos_line_str(&l, "repeating:      ");
    if (rk) { myrtos_line_str(&l, "HID usage "); myrtos_line_hex(&l, rk); }
    else    { myrtos_line_str(&l, "nothing"); }
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    uint32_t r = myrtos_usbinfo(MYRTOS_USB_ROOT);
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "root port:      ");
    myrtos_line_str(&l, (r & 1) ? "up" : "down");
    myrtos_line_str(&l, (r & 2) ? ", connected" : ", nothing attached");
    myrtos_line_str(&l, (r & 4) ? ", full speed" : ", low speed");
    if (r & 8) myrtos_line_str(&l, ", SUSPENDED");
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t h = myrtos_usbinfo(MYRTOS_USB_HID + i);
        if (!(h & (1u << 16))) continue;            // nothing wants this slot
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "hid slot ");
        myrtos_line_u32(&l, i);
        myrtos_line_str(&l, ": device ");
        myrtos_line_u32(&l, h & 0xFF);
        myrtos_line_str(&l, " instance ");
        myrtos_line_u32(&l, (h >> 8) & 0xFF);
        myrtos_line_str(&l, (h & (1u << 17)) ? ", report asked for" : ", NOT ASKED FOR");
        if ((h >> 24) & 0xFF) {
            myrtos_line_str(&l, ", idle sweeps ");
            myrtos_line_u32(&l, (h >> 24) & 0xFF);
        }
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    // The endpoints as the PIO layer holds them. 'queued' with a failed count
    // that keeps climbing and falling is a device that is not answering: three
    // failures end the transfer, the class driver asks again, and the count
    // starts over. Nothing above this line can tell you that.
    for (uint32_t i = 0; i < MYRTOS_USB_EP_COUNT; i++) {
        uint32_t e = myrtos_usbinfo(MYRTOS_USB_EP + i);
        uint32_t dev = e & 0xFF, ep = (e >> 8) & 0xFF;
        // Bit 19 is the library's own validity test: a closed slot keeps its
        // old address and endpoint number, and only the size says it is gone.
        if (!(e & (1u << 19))) continue;
        if (!dev && !ep) continue;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "endpoint ");
        myrtos_line_u32(&l, i);
        myrtos_line_str(&l, ": device ");
        myrtos_line_u32(&l, dev);
        // Named, because an unexplained high address on a board with an onboard
        // hub reads as a leak. TinyUSB numbers hubs above its device maximum.
        if (e & (1u << 20)) myrtos_line_str(&l, " (hub)");
        myrtos_line_str(&l, " ep ");
        myrtos_line_hex_byte(&l, ep);
        myrtos_line_str(&l, (e & (1u << 16)) ? ", queued" : ", NOTHING QUEUED");
        if (e & (1u << 18)) myrtos_line_str(&l, ", STALLED");
        myrtos_line_str(&l, ", failed ");
        myrtos_line_u32(&l, (e >> 24) & 0xFF);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }
}

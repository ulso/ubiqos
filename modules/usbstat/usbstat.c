#include "../../common/ubiqos_abi.h"

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
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, label);
    ubiqos_line_u32(&l, v);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

void module_main(void) {
    // First, because it is the question every other line here depends on: the
    // host loop runs on core 1 and nothing else does, so if it has stopped then
    // every count below is a photograph of the moment it stopped rather than a
    // description of now. An age of a few milliseconds is a loop going round;
    // an age that grows between two readings is one that is not.
    {
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "core 1 loop:    ");
        ubiqos_line_u32(&l, ubiqos_usbinfo(UBIQOS_USB_CORE1_BEATS));
        ubiqos_line_str(&l, " passes, last one ");
        ubiqos_line_u32(&l, ubiqos_usbinfo(UBIQOS_USB_CORE1_AGE));
        ubiqos_line_str(&l, " ms ago\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }

    kv("keys pushed:    ", ubiqos_usbinfo(UBIQOS_USB_KEYSIN));
    kv("rearms:         ", ubiqos_usbinfo(UBIQOS_USB_REARMS));
    kv("recoveries:     ", ubiqos_usbinfo(UBIQOS_USB_RECOVERIES));
    kv("cdc re-arms:    ", ubiqos_usbinfo(UBIQOS_USB_CDCREARMS));

    // What the HOST did to this device's bus. Zeroes after a night of the Mac
    // sleeping would say the board never saw it go -- which is an answer, and
    // a different one from having seen it and failed to come back.
    kv("host suspends:  ", ubiqos_usbinfo(UBIQOS_USB_SUSPENDS));
    kv("host resumes:   ", ubiqos_usbinfo(UBIQOS_USB_RESUMES));
    kv("host mounts:    ", ubiqos_usbinfo(UBIQOS_USB_MOUNTS));
    kv("host unmounts:  ", ubiqos_usbinfo(UBIQOS_USB_UNMOUNTS));
    kv("last one at ms: ", ubiqos_usbinfo(UBIQOS_USB_LASTEVENT));
    if (ubiqos_usbinfo(UBIQOS_USB_CDCGIVEUP))
        ubiqos_write_str(UBIQOS_STDOUT, "cdc:            given up, not asking again\n");

    uint32_t rk = ubiqos_usbinfo(UBIQOS_USB_REPEATKEY);
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    // The USB network device, which is a different thing from the host side
    // above: this is what the Mac sees when it enumerates this board.
    {
        uint32_t n[40];
        if (ubiqos_netdev_stats(n) == 0) {
            // What was observed, not a state that cannot be asked for: NCM
            // gives no link callback, so a frame arriving is the evidence.
            ubiqos_line_str(&l, "net: lwIP ");
            ubiqos_line_str(&l, n[0] ? "up" : "not started");
            ubiqos_line_str(&l, ", address ");
            if (n[2]) {
                for (int b = 3; b >= 0; b--) {
                    ubiqos_line_u32(&l, (n[2] >> (b * 8)) & 0xffu);
                    if (b) ubiqos_line_str(&l, ".");
                }
            } else {
                ubiqos_line_str(&l, "none yet");
            }
            ubiqos_line_str(&l, "\nnet frames:     in ");
            ubiqos_line_u32(&l, n[1]);
            ubiqos_line_str(&l, ", out ");
            ubiqos_line_u32(&l, n[3]);
            ubiqos_line_str(&l, ", dropped ");
            ubiqos_line_u32(&l, n[4]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);

            // Two lines, because ubiqos_line_t holds 96 bytes and one long
            // enough to say all of this is silently cut in half -- which read
            // as "lwip: link 0 drop" and looked like a finding.
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "lwip: arp in ");
            ubiqos_line_u32(&l, n[9]);
            ubiqos_line_str(&l, " out ");
            ubiqos_line_u32(&l, n[10]);
            ubiqos_line_str(&l, ", ip in ");
            ubiqos_line_u32(&l, n[11]);
            ubiqos_line_str(&l, " drop ");
            ubiqos_line_u32(&l, n[12]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);

            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "      icmp in ");
            ubiqos_line_u32(&l, n[13]);
            ubiqos_line_str(&l, " out ");
            ubiqos_line_u32(&l, n[14]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "      on the wire: arp ");
            ubiqos_line_u32(&l, n[16]);
            ubiqos_line_str(&l, ", ipv4 ");
            ubiqos_line_u32(&l, n[17]);
            ubiqos_line_str(&l, ", other ");
            ubiqos_line_u32(&l, n[18]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "      addressed to us ");
            ubiqos_line_u32(&l, n[19]);
            ubiqos_line_str(&l, ", of which icmp ");
            ubiqos_line_u32(&l, n[20]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "sock: served ");
            ubiqos_line_u32(&l, n[21]);
            ubiqos_line_str(&l, ", accepts queued ");
            ubiqos_line_u32(&l, n[22]);
            ubiqos_line_str(&l, " taken ");
            ubiqos_line_u32(&l, n[23]);
            ubiqos_line_str(&l, "\n      bytes in ");
            ubiqos_line_u32(&l, n[24]);
            ubiqos_line_str(&l, " out ");
            ubiqos_line_u32(&l, n[25]);
            ubiqos_line_str(&l, ", last refusal ");
            ubiqos_line_u32(&l, n[26]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "      lwip gave us ");
            ubiqos_line_u32(&l, n[29]);
            ubiqos_line_str(&l, " pbufs, ");
            ubiqos_line_u32(&l, n[30]);
            ubiqos_line_str(&l, " bytes\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, " tcp: recv ");
            ubiqos_line_u32(&l, n[31]);
            ubiqos_line_str(&l, " drop ");
            ubiqos_line_u32(&l, n[32]);
            ubiqos_line_str(&l, " err ");
            ubiqos_line_u32(&l, n[33]);
            ubiqos_line_str(&l, "\n      chkerr ");
            ubiqos_line_u32(&l, n[34]);
            ubiqos_line_str(&l, " proterr ");
            ubiqos_line_u32(&l, n[35]);
            ubiqos_line_str(&l, " xmit ");
            ubiqos_line_u32(&l, n[36]);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            // What the server actually replied, which is the one value that
            // separates "the server refused" from "the caller saw a refusal".
            ubiqos_line_str(&l, " last op ");
            ubiqos_line_u32(&l, n[27]);
            ubiqos_line_str(&l, " -> 0x");
            ubiqos_line_hex(&l, n[28]);
            ubiqos_line_str(&l, "\n");
        }
    }
    ubiqos_line_str(&l, "repeating:      ");
    if (rk) { ubiqos_line_str(&l, "HID usage "); ubiqos_line_hex(&l, rk); }
    else    { ubiqos_line_str(&l, "nothing"); }
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    uint32_t r = ubiqos_usbinfo(UBIQOS_USB_ROOT);
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "root port:      ");
    ubiqos_line_str(&l, (r & 1) ? "up" : "down");
    ubiqos_line_str(&l, (r & 2) ? ", connected" : ", nothing attached");
    ubiqos_line_str(&l, (r & 4) ? ", full speed" : ", low speed");
    if (r & 8) ubiqos_line_str(&l, ", SUSPENDED");
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t h = ubiqos_usbinfo(UBIQOS_USB_HID + i);
        if (!(h & (1u << 16))) continue;            // nothing wants this slot
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "hid slot ");
        ubiqos_line_u32(&l, i);
        ubiqos_line_str(&l, ": device ");
        ubiqos_line_u32(&l, h & 0xFF);
        ubiqos_line_str(&l, " instance ");
        ubiqos_line_u32(&l, (h >> 8) & 0xFF);
        ubiqos_line_str(&l, (h & (1u << 17)) ? ", report asked for" : ", NOT ASKED FOR");
        if ((h >> 24) & 0xFF) {
            ubiqos_line_str(&l, ", idle sweeps ");
            ubiqos_line_u32(&l, (h >> 24) & 0xFF);
        }
        ubiqos_line_str(&l, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }

    // The endpoints as the PIO layer holds them. 'queued' with a failed count
    // that keeps climbing and falling is a device that is not answering: three
    // failures end the transfer, the class driver asks again, and the count
    // starts over. Nothing above this line can tell you that.
    for (uint32_t i = 0; i < UBIQOS_USB_EP_COUNT; i++) {
        uint32_t e = ubiqos_usbinfo(UBIQOS_USB_EP + i);
        uint32_t dev = e & 0xFF, ep = (e >> 8) & 0xFF;
        // Bit 19 is the library's own validity test: a closed slot keeps its
        // old address and endpoint number, and only the size says it is gone.
        if (!(e & (1u << 19))) continue;
        if (!dev && !ep) continue;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "endpoint ");
        ubiqos_line_u32(&l, i);
        ubiqos_line_str(&l, ": device ");
        ubiqos_line_u32(&l, dev);
        // Named, because an unexplained high address on a board with an onboard
        // hub reads as a leak. TinyUSB numbers hubs above its device maximum.
        if (e & (1u << 20)) ubiqos_line_str(&l, " (hub)");
        ubiqos_line_str(&l, " ep ");
        ubiqos_line_hex_byte(&l, ep);
        ubiqos_line_str(&l, (e & (1u << 16)) ? ", queued" : ", NOTHING QUEUED");
        if (e & (1u << 18)) ubiqos_line_str(&l, ", STALLED");
        ubiqos_line_str(&l, ", failed ");
        ubiqos_line_u32(&l, (e >> 24) & 0xFF);
        ubiqos_line_str(&l, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}

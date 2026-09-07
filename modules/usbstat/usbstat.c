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
    if (myrtos_usbinfo(MYRTOS_USB_CDCGIVEUP))
        myrtos_write_str(MYRTOS_STDOUT, "cdc:            given up, not asking again\n");

    uint32_t rk = myrtos_usbinfo(MYRTOS_USB_REPEATKEY);
    myrtos_line_t l;
    myrtos_line_reset(&l);
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

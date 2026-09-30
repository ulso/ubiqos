// Bluetooth LE through the ESP32-C6's controller, raw HCI over ESP-Hosted.
//
// Enough of HCI for a program that listens: send a command and wait for its
// Command Complete, start and stop a passive scan, and take apart an LE
// advertising report. No stack -- a scanner needs none. The packets go through
// /dev/eh as UBIQOS_SS_EH_HCI_TX and come back as UBIQOS_SS_EH_HCI_RX, in H4
// form, the type byte first. The controller has to be up first: `ehrpc bt`.
//
// Used by modules/blescan and by modules/hibouair when there is no BleuIO.
#ifndef UBIQOS_HCI_H
#define UBIQOS_HCI_H

#include "ubiqos_abi.h"

#define UBIQOS_HCI_EVENT_MAX 264        // what the driver keeps of one packet

// Bring the controller up: `ehrpc bt`, run and waited for.
//
// Not while the driver is joining a network. Its join is RPCs too, on the
// same control plane with the same one inbox, and a question asked in the
// middle of it has its answer taken by whichever side reads first. A minute
// at most: a join that takes longer has failed in some way of its own.
static inline void ubiqos_hci_power_on(int32_t dev)
{
    for (int i = 0; i < 600; i++) {
        uint32_t state = 0;
        if (ubiqos_getstat(dev, UBIQOS_SS_EH_JOINED, &state, sizeof state) < 0 || state != 1) break;
        ubiqos_sleep(100);
    }
    const int32_t pid = ubiqos_exec("ehrpc", "bt");
    if (pid >= 0) ubiqos_wait(pid);
}

static inline int32_t ubiqos_hci_recv(int32_t dev, uint8_t *p, uint32_t cap)
{
    return ubiqos_getstat(dev, UBIQOS_SS_EH_HCI_RX, p, cap);
}

// One command and its Command Complete (04 0E len ncmds opcode status ...).
// 0 when the controller took it; its status when it refused; -1 when the
// driver would not send it or nothing answered in two seconds. Anything else
// that arrives first -- a report from a scan still running -- is let go.
static inline int32_t ubiqos_hci_command(int32_t dev, uint16_t opcode,
                                         const uint8_t *params, uint8_t plen)
{
    uint8_t pkt[4 + 32];
    if (plen > 32) return -1;
    pkt[0] = 0x01;
    pkt[1] = (uint8_t)opcode;
    pkt[2] = (uint8_t)(opcode >> 8);
    pkt[3] = plen;
    for (uint8_t i = 0; i < plen; i++) pkt[4 + i] = params[i];
    if (ubiqos_setstat(dev, UBIQOS_SS_EH_HCI_TX, pkt, 4u + plen) < 0) return -1;

    uint8_t ev[UBIQOS_HCI_EVENT_MAX];
    for (uint32_t waited = 0; waited < 2000; ) {
        const int32_t n = ubiqos_hci_recv(dev, ev, sizeof ev);
        if (n <= 0) { ubiqos_sleep(5); waited += 5; continue; }
        if (n >= 7 && ev[0] == 0x04 && ev[1] == 0x0E
                && ev[4] == (uint8_t)opcode && ev[5] == (uint8_t)(opcode >> 8))
            return ev[6];
    }
    return -1;
}

// Reset, the event mask, passive scan parameters, and scan on. Returns 0, or
// which of the four failed, 1 to 4.
//
// The event mask is the Bluetooth specification's default plus bit 61, the
// LE Meta event: every advertising report is one, and the default leaves it
// out -- the first scan got Command Complete for everything and then not a
// single report.
static inline int32_t ubiqos_hci_scan_start(int32_t dev)
{
    static const uint8_t event_mask[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20 };
    static const uint8_t params[] = {
        0x00,             // passive: listen, do not ask for scan responses
        0xA0, 0x00,       // interval 100 ms (160 x 0.625)
        0xA0, 0x00,       // window 100 ms: all the time
        0x00,             // our own address: public
        0x00,             // accept every advertiser
    };
    static const uint8_t on[] = { 0x01, 0x00 };     // on, duplicates kept
    if (ubiqos_hci_command(dev, 0x0C03, 0, 0) != 0) return 1;
    if (ubiqos_hci_command(dev, 0x0C01, event_mask, sizeof event_mask) != 0) return 2;
    if (ubiqos_hci_command(dev, 0x200B, params, sizeof params) != 0) return 3;
    if (ubiqos_hci_command(dev, 0x200C, on, sizeof on) != 0) return 4;
    return 0;
}

static inline void ubiqos_hci_scan_stop(int32_t dev)
{
    static const uint8_t off[] = { 0x00, 0x00 };
    (void)ubiqos_hci_command(dev, 0x200C, off, sizeof off);
}

// One LE Advertising Report, taken apart: 04 3E len 02 num, then event type,
// address type, address (least significant byte first), data length, data,
// RSSI. For one report, which is what the controller sends. false when the
// packet is not one.
typedef struct {
    const uint8_t *addr;          // six bytes, least significant first
    const uint8_t *data;          // the advertising data: length-type-value
    uint8_t        data_len;
    int8_t         rssi;
} ubiqos_hci_report_t;

static inline bool ubiqos_hci_report(const uint8_t *ev, int32_t n, ubiqos_hci_report_t *r)
{
    if (n < 5 + 9 || ev[0] != 0x04 || ev[1] != 0x3E || ev[3] != 0x02 || ev[4] < 1) return false;
    const uint8_t *p = ev + 5;
    const uint8_t dlen = p[8];
    if (n < 5 + 9 + dlen + 1) return false;
    r->addr = p + 2;
    r->data = p + 9;
    r->data_len = dlen;
    r->rssi = (int8_t)p[9 + dlen];
    return true;
}

// The value of the first AD structure of TYPE in a report's data, or 0.
static inline const uint8_t *ubiqos_hci_ad(const ubiqos_hci_report_t *r, uint8_t type, uint32_t *len)
{
    for (uint32_t i = 0; i + 1 < r->data_len; ) {
        const uint8_t l = r->data[i];
        if (!l || i + 1u + l > r->data_len) break;
        if (r->data[i + 1] == type) { *len = l - 1u; return r->data + i + 2; }
        i += 1u + l;
    }
    return 0;
}

#endif

#include "../../common/ubiqos_abi.h"

// blescan [SECONDS] -- what Bluetooth LE is saying nearby.
//
// No Bluetooth stack: a scanner needs none. It speaks HCI to the controller on
// the ESP32-C6 -- through ESP-Hosted, over the same SPI link as the network,
// see UBIQOS_SS_EH_HCI_TX -- and asks for three things: a reset, passive
// scanning, and scanning on. Then it listens to what the controller reports,
// one advertisement at a time, and keeps a table: each address once, its
// strongest signal, how often it was heard, and what it said its name was.
//
// A HibouAir sensor is named by its board id, the way the display names it:
// Smart Sensor Devices' company id, 0x075B, then the id in the beacon's
// bytes 4 to 6. That is the first thing a BLE-to-WiFi bridge has to hear.
//
// The controller has to be up first. `ehrpc bt` does that and this runs it.
UBIQOS_MEM_SIZE(4096);

#define HIBOU_COMPANY 0x075Bu
#define MAX_SEEN      24
#define HCI_EVENT_MAX 264          // what the driver keeps of one packet

typedef struct {
    uint8_t  addr[6];
    int8_t   best_rssi;
    uint16_t heard;
    uint16_t company;             // 0xffff when it said none
    uint32_t hibou_board;         // 0 when it is not a HibouAir
    char     name[20];
} seen_t;

static seen_t seen[MAX_SEEN];
static uint32_t nseen, overflow;
static int32_t dev = -1;

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static bool hci_send(const uint8_t *p, uint32_t n) {
    return ubiqos_setstat(dev, UBIQOS_SS_EH_HCI_TX, p, n) >= 0;
}

static int32_t hci_recv(uint8_t *p, uint32_t cap) {
    return ubiqos_getstat(dev, UBIQOS_SS_EH_HCI_RX, p, cap);
}

// A command, and its Command Complete: 04 0E len ncmds opcode(2) status ...
static bool command(const char *what, uint16_t opcode, const uint8_t *params, uint8_t plen) {
    uint8_t pkt[4 + 32];
    pkt[0] = 0x01;
    pkt[1] = (uint8_t)opcode;
    pkt[2] = (uint8_t)(opcode >> 8);
    pkt[3] = plen;
    for (uint8_t i = 0; i < plen; i++) pkt[4 + i] = params[i];
    if (!hci_send(pkt, 4u + plen)) { say("blescan: the driver would not take "); say(what); say("\r\n"); return false; }

    uint8_t ev[HCI_EVENT_MAX];
    for (uint32_t waited = 0; waited < 2000; ) {
        const int32_t n = hci_recv(ev, sizeof ev);
        if (n <= 0) { ubiqos_sleep(5); waited += 5; continue; }
        if (n >= 7 && ev[0] == 0x04 && ev[1] == 0x0E
                && ev[4] == (uint8_t)opcode && ev[5] == (uint8_t)(opcode >> 8)) {
            if (ev[6] == 0) return true;
            ubiqos_line_t l;
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "blescan: ");
            ubiqos_line_str(&l, what);
            ubiqos_line_str(&l, " refused, status ");
            ubiqos_line_u32(&l, ev[6]);
            ubiqos_line_str(&l, "\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            return false;
        }
        // Anything else before the answer -- an advertisement from a scan
        // somebody left on -- is not what was asked for, and is let go.
    }
    say("blescan: no answer to "); say(what); say(" -- is the controller up? 'ehrpc bt'\r\n");
    return false;
}

static seen_t *entry_for(const uint8_t *addr) {
    for (uint32_t i = 0; i < nseen; i++) {
        bool same = true;
        for (int k = 0; k < 6; k++) if (seen[i].addr[k] != addr[k]) same = false;
        if (same) return &seen[i];
    }
    if (nseen == MAX_SEEN) { overflow++; return 0; }
    seen_t *s = &seen[nseen++];
    for (int k = 0; k < 6; k++) s->addr[k] = addr[k];
    s->best_rssi = -128;
    s->company = 0xffff;
    return s;
}

// One advertising report's data: length-type-value structures.
static void read_ad(seen_t *s, const uint8_t *d, uint32_t len) {
    for (uint32_t i = 0; i + 1 < len; ) {
        const uint8_t l = d[i];
        if (!l || i + 1u + l > len) break;
        const uint8_t type = d[i + 1];
        const uint8_t *v = d + i + 2;
        const uint32_t vl = l - 1u;
        if ((type == 0x09 || type == 0x08) && !s->name[0]) {       // name, full or short
            uint32_t n = vl < sizeof s->name - 1 ? vl : sizeof s->name - 1;
            for (uint32_t k = 0; k < n; k++) s->name[k] = (char)v[k];
            s->name[n] = 0;
        }
        if (type == 0xFF && vl >= 2) {                              // manufacturer data
            s->company = (uint16_t)(v[0] | (v[1] << 8));
            if (s->company == HIBOU_COMPANY && vl >= 7)
                s->hibou_board = ((uint32_t)v[4] << 16) | ((uint32_t)v[5] << 8) | v[6];
        }
        i += 1u + l;
    }
}

// 04 3E len 02 num, then event type, address type, address, data length,
// data, RSSI -- for one report, which is what the controller sends.
static void advertising_report(const uint8_t *ev, int32_t n) {
    if (n < 5 || ev[3] != 0x02 || ev[4] < 1) return;
    const uint8_t *r = ev + 5;
    if (n < 5 + 9) return;
    const uint8_t *addr = r + 2;
    const uint8_t dlen = r[8];
    if (n < 5 + 9 + dlen + 1) return;
    const int8_t rssi = (int8_t)r[9 + dlen];
    seen_t *s = entry_for(addr);
    if (!s) return;
    s->heard++;
    if (rssi > s->best_rssi) s->best_rssi = rssi;
    read_ad(s, r + 9, dlen);
}

static void hex2(ubiqos_line_t *l, uint32_t v) {
    const char h[] = "0123456789ABCDEF";
    const char c[3] = { h[(v >> 4) & 15u], h[v & 15u], 0 };
    ubiqos_line_str(l, c);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: blescan [SECONDS]\n\n"
            "Listen to Bluetooth LE for SECONDS -- ten unless said -- and list what\n"
            "was heard: address, strongest signal, how often, and a name. A HibouAir\n"
            "sensor is named by its board id. Brings the controller up first.\n")) return;
    uint32_t seconds = 10;
    if (argc > 1) {
        seconds = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) seconds = seconds * 10u + (uint32_t)(*p - '0');
        if (!seconds) seconds = 10;
    }

    const int32_t up = ubiqos_exec("ehrpc", "bt");
    if (up >= 0) ubiqos_wait(up);

    dev = ubiqos_open("/dev/eh");
    if (dev < 0) { say("blescan: no /dev/eh\r\n"); return; }

    static const uint8_t scan_params[] = {
        0x00,             // passive: listen, do not ask for scan responses
        0xA0, 0x00,       // interval 100 ms (160 x 0.625)
        0xA0, 0x00,       // window 100 ms: all the time
        0x00,             // our own address: public
        0x00,             // accept every advertiser
    };
    // The Bluetooth specification's default event mask, 0x00001FFFFFFFFFFF,
    // leaves out bit 61, the LE Meta event -- and every advertising report is
    // one. The first scan got Command Complete for all three commands below
    // and then not a single report: the controller was scanning and keeping
    // what it heard to itself. The default plus bit 61.
    static const uint8_t event_mask[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20 };
    static const uint8_t scan_on[]  = { 0x01, 0x00 };   // on, duplicates kept
    static const uint8_t scan_off[] = { 0x00, 0x00 };

    if (!command("HCI reset", 0x0C03, 0, 0)
            || !command("the event mask", 0x0C01, event_mask, sizeof event_mask)
            || !command("the scan parameters", 0x200B, scan_params, sizeof scan_params)
            || !command("scan on", 0x200C, scan_on, sizeof scan_on)) {
        ubiqos_close(dev);
        return;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "listening for ");
    ubiqos_line_u32(&l, seconds);
    ubiqos_line_str(&l, " seconds...\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    uint32_t reports = 0;
    const uint32_t start = ubiqos_ticks_now();
    uint8_t ev[HCI_EVENT_MAX];
    while (ubiqos_ticks_now() - start < seconds * 1000u) {
        const int32_t n = hci_recv(ev, sizeof ev);
        if (n <= 0) { ubiqos_sleep(2); continue; }
        if (ev[0] == 0x04 && ev[1] == 0x3E) { reports++; advertising_report(ev, n); }
    }
    command("scan off", 0x200C, scan_off, sizeof scan_off);
    ubiqos_close(dev);

    // Strongest first.
    for (uint32_t i = 1; i < nseen; i++)
        for (uint32_t j = i; j > 0 && seen[j].best_rssi > seen[j - 1].best_rssi; j--) {
            seen_t t = seen[j]; seen[j] = seen[j - 1]; seen[j - 1] = t;
        }

    say("address            rssi  heard  what\r\n");
    for (uint32_t i = 0; i < nseen; i++) {
        const seen_t *s = &seen[i];
        ubiqos_line_reset(&l);
        for (int k = 5; k >= 0; k--) { hex2(&l, s->addr[k]); if (k) ubiqos_line_str(&l, ":"); }
        ubiqos_line_str(&l, "  ");
        ubiqos_line_str(&l, s->best_rssi < 0 ? "-" : " ");
        ubiqos_line_u32(&l, (uint32_t)(s->best_rssi < 0 ? -s->best_rssi : s->best_rssi));
        ubiqos_line_str(&l, "   ");
        ubiqos_line_u32(&l, s->heard);
        ubiqos_line_str(&l, "   ");
        if (s->hibou_board) {
            ubiqos_line_str(&l, "HibouAir ");
            hex2(&l, s->hibou_board >> 16); hex2(&l, s->hibou_board >> 8); hex2(&l, s->hibou_board);
        } else if (s->name[0]) {
            ubiqos_line_str(&l, s->name);
        } else if (s->company != 0xffff) {
            ubiqos_line_str(&l, "company 0x");
            hex2(&l, s->company >> 8); hex2(&l, s->company);
        }
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
    ubiqos_line_reset(&l);
    ubiqos_line_u32(&l, nseen);
    ubiqos_line_str(&l, " devices, ");
    ubiqos_line_u32(&l, reports);
    ubiqos_line_str(&l, " reports");
    if (overflow) { ubiqos_line_str(&l, ", and more than the table holds"); }
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

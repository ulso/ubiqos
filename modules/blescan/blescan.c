#include "../../common/ubiqos_abi.h"
#include "../../common/ubiqos_hci.h"

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

// One advertising report into the table.
static void take_report(const ubiqos_hci_report_t *r) {
    seen_t *s = entry_for(r->addr);
    if (!s) return;
    s->heard++;
    if (r->rssi > s->best_rssi) s->best_rssi = r->rssi;
    uint32_t len = 0;
    const uint8_t *v = ubiqos_hci_ad(r, 0x09, &len);
    if (!v) v = ubiqos_hci_ad(r, 0x08, &len);                  // name, full or short
    if (v && !s->name[0]) {
        uint32_t n = len < sizeof s->name - 1 ? len : sizeof s->name - 1;
        for (uint32_t k = 0; k < n; k++) s->name[k] = (char)v[k];
        s->name[n] = 0;
    }
    v = ubiqos_hci_ad(r, 0xFF, &len);                           // manufacturer data
    if (v && len >= 2) {
        s->company = (uint16_t)(v[0] | (v[1] << 8));
        if (s->company == HIBOU_COMPANY && len >= 7)
            s->hibou_board = ((uint32_t)v[4] << 16) | ((uint32_t)v[5] << 8) | v[6];
    }
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

    dev = ubiqos_open("/dev/eh");
    if (dev < 0) { say("blescan: no /dev/eh\r\n"); return; }
    ubiqos_hci_power_on(dev);

    static const char *const step[] = { "", "HCI reset", "the event mask", "the scan parameters", "scan on" };
    const int32_t failed = ubiqos_hci_scan_start(dev);
    if (failed) {
        say("blescan: "); say(step[failed]); say(" was not taken -- is the controller up? 'ehrpc bt'\r\n");
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
    uint8_t ev[UBIQOS_HCI_EVENT_MAX];
    while (ubiqos_ticks_now() - start < seconds * 1000u) {
        const int32_t n = ubiqos_hci_recv(dev, ev, sizeof ev);
        if (n <= 0) { ubiqos_sleep(2); continue; }
        ubiqos_hci_report_t r;
        if (ubiqos_hci_report(ev, n, &r)) { reports++; take_report(&r); }
    }
    ubiqos_hci_scan_stop(dev);
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

#include "../../common/ubiqos_abi.h"
#include "../../common/ubiqos_hci.h"

// bleadv SECONDS HEX [HEX ...] -- send Bluetooth LE advertisements.
//
// Each HEX is manufacturer data as a scanner sees it from the company id on --
// "5B070702A1A2A3F709133FE801" is a SondeAir temperature sensor in Smart Sensor
// Devices' own example -- and goes out as a non-connectable advertisement with
// the usual flags in front. With several, they take turns, half a second each,
// so one board can stand in for a room of sensors: a scanner that tells them
// apart by what is in the data, as airview and hibouair do by board id, sees
// them all.
//
// For testing a scanner against sensors that are not on the bench, which is
// what it was written for. blescan's sibling: HCI straight to the controller on
// the ESP32-C6, no stack. Nothing else may be using the controller meanwhile --
// 'kill hibouair' first on a board that runs it.
UBIQOS_MEM_SIZE(4096);

#define MAX_FRAMES 8
#define MAX_DATA   27              // 31 bytes of advertising, less flags and the AD header

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The advertising data for one frame: flags, then the manufacturer AD. The
// command's parameter is always 32 bytes, the length first.
static int32_t frame(const char *hex, uint8_t out[32])
{
    uint8_t data[MAX_DATA];
    uint32_t n = 0;
    for (const char *p = hex; p[0]; p += 2) {
        const int hi = hexval(p[0]), lo = p[1] ? hexval(p[1]) : -1;
        if (hi < 0 || lo < 0 || n >= MAX_DATA) return -1;
        data[n++] = (uint8_t)(hi << 4 | lo);
    }
    if (n < 2) return -1;

    for (uint32_t i = 0; i < 32; i++) out[i] = 0;
    uint32_t k = 1;
    out[k++] = 0x02; out[k++] = 0x01; out[k++] = 0x06;      // LE general, no BR/EDR
    out[k++] = (uint8_t)(n + 1); out[k++] = 0xFF;            // manufacturer specific
    for (uint32_t i = 0; i < n; i++) out[k++] = data[i];
    out[0] = (uint8_t)(k - 1);
    return 0;
}

void module_main(int argc, char **argv)
{
    if (ubiqos_help(argc, argv,
            "usage: bleadv SECONDS HEX [HEX ...]\n\n"
            "Advertise each HEX -- manufacturer data from the company id on --\n"
            "for SECONDS, taking turns half a second each when there are several.\n"
            "Non-connectable, with the usual flags in front. For testing a scanner;\n"
            "nothing else may use the controller meanwhile ('kill hibouair').\n")) return;
    if (argc < 3) { say("usage: bleadv SECONDS HEX [HEX ...]\r\n"); return; }

    uint32_t seconds = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) seconds = seconds * 10u + (uint32_t)(*p - '0');
    if (!seconds) { say("bleadv: SECONDS is a number above zero\r\n"); return; }

    uint8_t frames[MAX_FRAMES][32];         // 256 bytes of stack, not a static
    uint32_t count = 0;
    for (int i = 2; i < argc && count < MAX_FRAMES; i++) {
        if (frame(argv[i], frames[count]) < 0) {
            say("bleadv: not hex, or longer than 27 bytes: "); say(argv[i]); say("\r\n");
            return;
        }
        count++;
    }

    const int32_t dev = ubiqos_open("/dev/eh");
    if (dev < 0) { say("bleadv: no /dev/eh\r\n"); return; }
    ubiqos_hci_power_on(dev, false);

    // Every 100 ms, non-connectable, our public address, all three channels.
    static const uint8_t params[15] = {
        0xA0, 0x00, 0xA0, 0x00, 0x03, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0x07, 0x00,
    };
    static const uint8_t on[1] = { 1 }, off[1] = { 0 };
    if (ubiqos_hci_command(dev, 0x0C03, 0, 0) != 0 ||
        ubiqos_hci_command(dev, 0x2006, params, sizeof params) != 0 ||
        ubiqos_hci_command(dev, 0x2008, frames[0], 32) != 0 ||
        ubiqos_hci_command(dev, 0x200A, on, 1) != 0) {
        say("bleadv: the controller would not advertise -- is it up? 'ehrpc bt'\r\n");
        ubiqos_close(dev);
        return;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "advertising ");
    ubiqos_line_u32(&l, count);
    ubiqos_line_str(&l, count == 1 ? " frame for " : " frames in turn for ");
    ubiqos_line_u32(&l, seconds);
    ubiqos_line_str(&l, " seconds\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    // The data can be changed while advertising is on; the controller sends
    // the new data from its next event.
    const uint32_t start = ubiqos_ticks_now();
    for (uint32_t turn = 1; ubiqos_ticks_now() - start < seconds * 1000u; turn++) {
        ubiqos_sleep(500);
        if (count > 1) ubiqos_hci_command(dev, 0x2008, frames[turn % count], 32);
    }

    ubiqos_hci_command(dev, 0x200A, off, 1);
    say("bleadv: done\r\n");
    ubiqos_close(dev);
}

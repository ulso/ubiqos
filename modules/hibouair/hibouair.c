// hibouair -- scan for HibouAir sensors with a BleuIO dongle and list what
// they are measuring, one line per reading.
//
// Ctrl-C ends scanning.
//
// The dongle is an AT-command device on /dev/acm. Put into verbose mode it
// answers in JSON, which is why the parsing below looks for `"data":"` and not
// for a position in a line.
//
// The sensors broadcast; nothing is connected to. Each advertisement carries
// manufacturer-specific data, and the layout of it is Smart Sensor Devices'
// own. It is not guessed here: it is taken from the parser in the
// pico-io-bridge Rust project, which has tests against captured frames.

#include <stdbool.h>
#include "../../common/myrtos_stdio.h"

// The reference HibouAir reader, and the SDK's worked example of one.
//
// New sensor models appear, and each may carry a beacon this does not decode
// yet. Such changes belong with whoever ships them, so an application built
// against the SDK carries its OWN reader under its own module name rather than
// waiting on this one -- see docs/the-sdk.md. What stays here is the version
// that is known to work against the sensors on the bench, which is what makes
// it useful to read.

#define AT_ECHO_OFF    "ATE0\r"
#define AT_VERBOSE_ON  "ATV1\r"
#define AT_CENTRAL     "AT+CENTRAL\r"

// Scan for advertisements containing this: the manufacturer-data AD type
// followed by Smart Sensor Devices' company id, little endian. Filtering in the
// dongle rather than here keeps everything else off the wire -- a room has
// thirty other beacons in it and each one would be a line to read and discard.
#define AT_SCAN        "AT+FINDSCANDATA=FF5B07\r"

#define CHUNK          128
#define LINE_MAX       224

// What the advertisement holds, once found.
#define HIBOU_COMPANY  0x075Bu
#define HIBOU_BEACON   0x05u        // the frame that carries readings; there is
                                    // another that alternates with it and does not
#define PULSE_ACM      1
#define PULSE_INTR     2   // ctrl-C, asked for with myrtos_catch_intr
#define MAX_SENSORS    8
#define REDRAW_MS      2000

// The board types, from dxbleuio/src/models/hibouair.rs. What a sensor does not
// have it reports as zero, which is why one of these will show CO2 0 for ever
// and it is not a fault.
//
// A table of characters and not a switch returning string literals. The switch
// is the first trap docs/writing-modules.md warns about and it caught this on
// the first build: the literals become a table of pointers, the linker writes
// absolute addresses into it, and the module is refused. Here the names are
// values inside the table itself, so they travel with it.
static const struct { uint8_t code; char name[10]; } board_types[] = {
    { 0x02, "temp/hum " }, { 0x03, "PM       " }, { 0x04, "CO2      " },
    { 0x05, "NO2 wifi " }, { 0x06, "CO2 batt " }, { 0x07, "NO2 lte  " },
    { 0x08, "PIR      " }, { 0x09, "CO2/noise" }, { 0x0A, "duo mstr " },
    { 0x0B, "duo slv  " }, { 0x14, "matrix   " },
};

static const char *board_type_name(uint8_t t)
{
    static const char unknown[10] = "unknown  ";
    for (uint32_t i = 0; i < sizeof(board_types) / sizeof(board_types[0]); i++)
        if (board_types[i].code == t)
            return board_types[i].name;
    return unknown;
}

// 0 = old, 1 = resistance, 2 = ppm, 3 = IAQ.
static const char *voc_unit(uint8_t t)
{
    static const char units[4][4] = { "raw", "ohm", "ppm", "iaq" };
    return units[t < 4 ? t : 0];
}

typedef struct {
    uint32_t board;
    char     addr[20];
    uint8_t  type;
    uint8_t  voc_type;
    int32_t  temp;          // tenths
    uint32_t hum, bar, voc, pm1, pm25, co2;
    bool     used;
} sensor_t;

__thread sensor_t sensors[MAX_SENSORS];
__thread uint32_t sensor_count;
__thread uint32_t drawn_rows;

MYRTOS_LIBC_DEFINE

__thread uint8_t buf[CHUNK];
__thread char    line[LINE_MAX];
__thread uint32_t line_len;

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
 * send_command -- send AT command to BLE dongle.
 */
static int32_t send_command(int32_t dev, const char *cmd)
{
    if (flush_input(dev) < 0)
        return -1;

    return write(dev, cmd, strlen(cmd));
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * find_payload -- locate the manufacturer data in a JSON line and unpack it.
 *
 * The line looks like {"S":2,"addr":"[1]DA:84:...","data":"0201061BFF5B07..."}.
 * Inside the hex, FF5B07 marks the start of what we want: the AD type followed
 * by the company id. Returns how many bytes were unpacked from the 5B onwards,
 * or 0 if this line is not one of ours.
 */
static uint32_t find_payload(const char *s, uint8_t *out, uint32_t max)
{
    const char *d = strstr(s, "\"data\":\"");
    if (!d)
        return 0;
    d += 8;

    const char *m = strstr(d, "FF5B07");
    if (!m)
        return 0;
    m += 2;                         // step over the AD type; keep the company id

    uint32_t n = 0;
    while (n < max) {
        int hi = hex_digit(m[0]);
        int lo = hi < 0 ? -1 : hex_digit(m[1]);
        if (lo < 0)
            break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        m += 2;
    }
    return n;
}

static uint32_t le16(const uint8_t *b, uint32_t at)
{
    return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8);
}

/**
 * remember -- decode one advertisement into the table.
 *
 * The field layout is from pico-io-bridge/src/bleuio.rs and the struct in
 * dxbleuio/src/models/hibouair.rs. Everything is little endian except the CO2
 * reading, which is not, and that is not a mistake here.
 */
static void remember(const uint8_t *b, uint32_t n, const char *addr)
{
    if (n < 26 || le16(b, 0) != HIBOU_COMPANY || b[2] != HIBOU_BEACON)
        return;                     // the other frame type, or not a HibouAir

    uint32_t board = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];

    sensor_t *e = 0;
    for (uint32_t i = 0; i < sensor_count; i++)
        if (sensors[i].board == board) { e = &sensors[i]; break; }
    if (!e) {
        if (sensor_count >= MAX_SENSORS)
            return;
        e = &sensors[sensor_count++];
        e->board = board;
        e->used = true;
        uint32_t i = 0;
        while (i < sizeof(e->addr) - 1 && addr[i]) { e->addr[i] = addr[i]; i++; }
        e->addr[i] = 0;
    }

    e->type     = b[3];
    e->voc_type = b[25];
    e->temp     = (int32_t)(int16_t)le16(b, 11);
    e->bar      = le16(b, 9);
    e->hum      = le16(b, 13);
    e->voc      = le16(b, 15);
    e->pm1      = le16(b, 17);
    e->pm25     = le16(b, 19);
    e->co2      = ((le16(b, 23) & 0xffu) << 8) | (le16(b, 23) >> 8);
}

/**
 * redraw -- the whole table, in place.
 *
 * One line per sensor, rewritten where it stands rather than appended, because
 * a beacon that speaks ten times a second is a waterfall otherwise. The cursor
 * goes back up over what was printed last time; the console and the serial line
 * take the same escape sequence.
 */
// --- PUBLISHING ------------------------------------------------------------
// The sensors live in this process's memory and nothing else can see them, so
// a web page cannot show what the table shows. They are written out as JSON
// instead, to a file in /tmp -- which is a tmpfs in PSRAM, so this costs a copy
// and no card traffic.
//
// A file rather than a message interface, and the choice was Ulf's: it needs no
// IPC, it does not turn a command into a daemon that something must know how to
// ask, and the result can be read with cat. httpd then serves it as it serves
// any other file, which is why /api/sensors needed almost no code in the
// server.
//
// Written in place, because this filesystem has no rename. So a reader can in
// principle catch a half-written file -- the window is one truncate and a few
// hundred bytes into PSRAM, against a reader that asks every two seconds -- and
// the page treats a parse failure as "not this time" rather than as an error.
// Saying so is better than a comment claiming an atomicity that is not there.
//
// Two writers of this path would race. There is one scanner, because there is
// one dongle.
#define SENSORS_PATH "/tmp/sensors.json"

static void put_str(int32_t fd, const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    myrtos_write(fd, s, n);
}

// A tenths-of-a-unit integer as a decimal with one place. Every reading the
// dongle gives arrives this way except CO2 and VOC, which are whole numbers.
static void put_tenths(int32_t fd, int32_t v)
{
    char b[16];
    uint32_t n = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    uint32_t whole = (uint32_t)v / 10, frac = (uint32_t)v % 10;
    char t[12]; uint32_t m = 0;
    do { t[m++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (m) b[n++] = t[--m];
    b[n++] = '.';
    b[n++] = (char)('0' + frac);
    myrtos_write(fd, b, n);
}

static void put_u32(int32_t fd, uint32_t v)
{
    char t[12]; uint32_t m = 0;
    do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v);
    char b[12]; uint32_t n = 0;
    while (m) b[n++] = t[--m];
    myrtos_write(fd, b, n);
}

static void publish(void)
{
    int32_t fd = myrtos_open_flags(SENSORS_PATH,
                                   MYRTOS_O_WRONLY | MYRTOS_O_CREAT | MYRTOS_O_TRUNC);
    if (fd < 0)
        return;                      // no /tmp is not a reason to stop scanning

    put_str(fd, "{\"sensors\":[");
    for (uint32_t i = 0; i < sensor_count; i++) {
        sensor_t *e = &sensors[i];
        if (i) put_str(fd, ",");
        put_str(fd, "{\"board\":\"");
        // The board number is hex everywhere else it is shown -- on the table,
        // on the dongle's own label -- so it is a string here rather than a
        // number a reader would have to know to format.
        {
            static const char hex[] = "0123456789ABCDEF";
            char h[6];
            for (int k = 0; k < 6; k++) h[k] = hex[(e->board >> (20 - 4 * k)) & 0xf];
            myrtos_write(fd, h, 6);
        }
        put_str(fd, "\",\"addr\":\"");
        put_str(fd, e->addr);
        put_str(fd, "\",\"type\":\"");
        {
            // Trimmed: the table's names are padded to a fixed width so the
            // columns line up, and a page does its own alignment.
            const char *t = board_type_name(e->type);
            uint32_t n = 0, last = 0;
            while (t[n]) { if (t[n] != ' ') last = n + 1; n++; }
            myrtos_write(fd, t, last);
        }
        put_str(fd, "\",\"temp\":");     put_tenths(fd, e->temp);
        put_str(fd, ",\"humidity\":");   put_tenths(fd, (int32_t)e->hum);
        put_str(fd, ",\"pressure\":");   put_tenths(fd, (int32_t)e->bar);
        put_str(fd, ",\"voc\":");        put_u32(fd, e->voc);
        put_str(fd, ",\"vocUnit\":\"");  put_str(fd, voc_unit(e->voc_type));
        put_str(fd, "\",\"co2\":");      put_u32(fd, e->co2);
        put_str(fd, ",\"pm1\":");        put_tenths(fd, (int32_t)e->pm1);
        put_str(fd, ",\"pm25\":");       put_tenths(fd, (int32_t)e->pm25);
        put_str(fd, "}");
    }
    put_str(fd, "],\"count\":");
    put_u32(fd, sensor_count);
    put_str(fd, "}\n");
    myrtos_close(fd);
}

static void redraw(void)
{
    if (drawn_rows)
        printf("\x1b[%luA", (unsigned long)drawn_rows);

    printf("board   address            type       temp    humid   press     VOC        CO2   PM1/2.5\x1b[K\n");
    for (uint32_t i = 0; i < sensor_count; i++) {
        sensor_t *e = &sensors[i];
        printf("%06lX  %-17s  %s  %ld.%ld C  %lu.%lu %%  %lu.%lu  %5lu %s  %4lu  %lu.%lu/%lu.%lu\x1b[K\n",
               (unsigned long)e->board, e->addr, board_type_name(e->type),
               (long)(e->temp / 10), (long)(e->temp < 0 ? -(e->temp % 10) : e->temp % 10),
               (unsigned long)(e->hum / 10), (unsigned long)(e->hum % 10),
               (unsigned long)(e->bar / 10), (unsigned long)(e->bar % 10),
               (unsigned long)e->voc, voc_unit(e->voc_type),
               (unsigned long)e->co2,
               (unsigned long)(e->pm1 / 10),  (unsigned long)(e->pm1 % 10),
               (unsigned long)(e->pm25 / 10), (unsigned long)(e->pm25 % 10));
    }
    drawn_rows = sensor_count + 1;
}

/**
 * take_address -- the sender's address out of the same JSON line, without the
 * [0]/[1] address-type prefix the dongle puts in front of it.
 */
static void take_address(const char *s, char *out, uint32_t max)
{
    out[0] = 0;
    const char *a = strstr(s, "\"addr\":\"");
    if (!a)
        return;
    a += 8;
    if (a[0] == '[' && a[2] == ']')
        a += 3;

    uint32_t i = 0;
    while (i < max - 1 && a[i] && a[i] != '"') {
        out[i] = a[i];
        i++;
    }
    out[i] = 0;
}

/**
 * consume -- feed bytes in, and act on every complete line.
 */
static void consume(const uint8_t *p, uint32_t n)
{
    char addr[20];
    uint8_t payload[32];

    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == '\r')
            continue;
        if (p[i] != '\n') {
            if (line_len < LINE_MAX - 1)
                line[line_len++] = (char)p[i];
            continue;
        }
        line[line_len] = 0;
        if (line_len) {
            uint32_t got = find_payload(line, payload, sizeof(payload));
            if (got) {
                take_address(line, addr, sizeof(addr));
                remember(payload, got, addr);
            }
        }
        line_len = 0;
    }
}

void module_main(int argc, char **argv)
{
    if (myrtos_help(argc, argv,
            "usage: hibouair [-q]\n\n"
            "Scans for HibouAir sensors on the BleuIO dongle and shows a live\n"
            "table. Ctrl-C tells the dongle to stop and exits.\n\n"
            "Either way the readings are written to /tmp/sensors.json, which is\n"
            "what httpd serves at /api/sensors. -q draws no table, which is what\n"
            "it wants in the background: 'hibouair -q &'.\n")) return;

    // Quiet is for the background. A table drawn by a process nobody is looking
    // at is not merely wasted -- it lands on whatever terminal the shell was
    // using, in the middle of somebody else's output.
    bool quiet = false;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 'q' && !argv[i][2]) quiet = true;

    int32_t dev = myrtos_open("/dev/acm");
    if (dev < 0) {
        printf("hibouair: no dongle on /dev/acm\n");
        return;
    }

    send_command(dev, AT_ECHO_OFF);   myrtos_sleep(200);
    send_command(dev, AT_VERBOSE_ON); myrtos_sleep(200);
    send_command(dev, AT_CENTRAL);    myrtos_sleep(400);
    flush_input(dev);
    if (send_command(dev, AT_SCAN) < 0) {
        printf("hibouair: the dongle will not start scanning\n");
        return;
    }

    // Ctrl-C arrives as a pulse rather than ending the process, so the dongle
    // can be told to stop before we go. Killed outright it would keep scanning
    // and keep talking into a machine that is no longer listening -- and the
    // next program to open it would find a stream already running.
    myrtos_catch_intr(PULSE_INTR);

    if (!quiet) printf("scanning; ctrl-C to stop\n\n");

    uint32_t next_draw = myrtos_ticks_now() + REDRAW_MS;

    // Armed rather than polled: the process is off the run queue until the
    // dongle actually says something, instead of asking it every couple of
    // milliseconds and being told no.
    for (;;) {
        myrtos_arm(dev, PULSE_ACM);

        myrtos_msg_t m;
        int32_t from = myrtos_receive_tmo(&m, 30000);
        if (from == MYRTOS_RECV_TIMEOUT) {
            printf("hibouair: nothing heard for 30 s\n");
            continue;
        }
        if (from != 0)                  // a real message; not ours to answer
            continue;

        if (m.type == PULSE_INTR) {
            // The dongle stops scanning on a bare Ctrl-C, the same key that
            // brought us here. Half a second is what the kernel allows before
            // it ends the process regardless, which is far more than this needs.
            static const char stop[] = "\x03";
            myrtos_write(dev, stop, 1);
            myrtos_sleep(100);
            if (!quiet) printf("\nhibouair: told the dongle to stop\n");
            // The readings are stale the moment the scan stops, and a page
            // showing yesterday's air as though it were now is worse than a
            // page showing nothing.
            myrtos_fs_remove(SENSORS_PATH);
            break;
        }

        int32_t n = myrtos_read(dev, buf, sizeof(buf));
        if (n < 0) {
            printf("hibouair: the dongle is gone\n");
            break;
        }
        if (n > 0)
            consume(buf, (uint32_t)n);

        // Redrawing on a timer rather than on every advertisement: four sensors
        // beaconing ten times a second would otherwise spend the whole console
        // on writing the same numbers again.
        if ((int32_t)(myrtos_ticks_now() - next_draw) >= 0) {
            if (!quiet) redraw();
            publish();
            next_draw = myrtos_ticks_now() + REDRAW_MS;
        }
    }

    myrtos_disarm_all();
}

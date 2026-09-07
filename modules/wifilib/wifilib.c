// The ESP32-C6 on the Fruit Jam, over SPI.
//
// The chip runs NINA firmware, which carries its own TCP/IP stack: the host does
// not run one. It sends commands -- connect to this network, open this socket,
// send these bytes -- and the chip does the rest. That is why there is no lwIP
// here, and why the port of it in modules/lwipd is set aside rather than
// finished. See docs/lwip.
//
// The framing is Arduino's WiFiNINA protocol, which Adafruit forked:
//
//   to the chip     0xE0, command, parameter count, [parameters,] 0xEE
//   from the chip   0xE0, command|0x80, parameter count, length, data, 0xEE
//
// with a handshake line either side of it. The chip holds ACK low when it has
// nothing to say; it raises it once selected and ready to transfer. Both waits
// have timeouts, because a chip that is not there would otherwise hang the boot
// -- which is a lesson this project has already paid for once.

#include <stdint.h>
#include <stdbool.h>
#include "../../common/modules.h"   // myrtos_sleep and the message types

// A library module: code the kernel calls rather than runs. This was
// kernel/wifi.c until 6 Sep 2026, six and a half kilobytes of SRAM that only
// mattered to somebody who typed "wifi". It is loaded where modules are
// loaded, which is PSRAM, and the kernel image no longer carries it.
//
// Which is why there is not an SDK header in sight. A library runs in kernel
// context but is linked separately, so it cannot call the kernel's functions by
// name -- it is handed their addresses in myrtos_kernel_api_t, and everything
// below reaches the hardware through K. Even gpio_put and gpio_get, which are
// inline in the SDK's headers and would otherwise have dragged them in.
static const myrtos_kernel_api_t *K;

// The named priorities, which used to come from usbdev.h. A library cannot
// include a kernel header, and one number is not worth an interface.
#define MYRTOS_PRIO_WIFI 20

#define WIFI_SPI    (K->spi)
#define WIFI_SCK    30
#define WIFI_MOSI   31
#define WIFI_MISO   28
#define WIFI_CS     46
#define WIFI_ACK     3

#define START_CMD   0xE0u
#define END_CMD     0xEEu
#define ERR_CMD     0xEFu
#define REPLY_FLAG  0x80u
#define GET_FW_VERSION_CMD  0x37u
#define SCAN_NETWORKS_CMD   0x27u
#define START_SCAN_CMD      0x36u
#define GET_IDX_RSSI_CMD    0x32u
#define GET_CONN_STATUS_CMD 0x20u

static uint8_t xfer(uint8_t v) {
    uint8_t r = 0;
    K->spi_write_read(WIFI_SPI, &v, &r, 1);
    return r;
}

// Wait for the handshake line to reach a level, giving up rather than spinning.
static bool wait_ack(bool level, uint32_t ms) {
    // The SDK's absolute_time_t went with the SDK headers. A microsecond count
    // that never wraps says the same thing with less behind it.
    uint64_t deadline = K->time_us() + (uint64_t)ms * 1000u;
    while (K->gpio_get(WIFI_ACK) != level) {
        if (K->time_us() > deadline) return false;
    }
    return true;
}

// Ten milliseconds was a guess and too short. Only the very first command ever
// worked, which is what a handshake that has not settled between transactions
// looks like -- the second select gives up, the caller sees nothing, and the
// reply it was waiting for is read as "no answer" rather than "not yet".
static bool select_chip(void) {
    if (!wait_ack(false, 100)) return false;     // not busy
    K->gpio_put(WIFI_CS, 0);
    return wait_ack(true, 100);                  // selected and ready
}

// The same, but prepared to wait a long time for the chip to become ready.
//
// nina-fw's handler for SCAN_NETWORKS is not a request to start anything: it
// calls WiFi.scanNetworks() -- the blocking one -- and only builds its reply
// when the radio has finished, which is seconds. So the chip holds READY for
// that whole time and a hundred milliseconds was never going to see the end of
// it. The reference does not time out here at all.
//
// It has to yield rather than spin. This runs in the wifi server at priority
// 21, above the shell, and seconds of spinning is exactly what froze the
// machine when sleep_ms was used instead of myrtos_sleep.
static bool select_chip_slow(uint32_t ms) {
    for (uint32_t waited = 0; waited < ms; waited += 4) {
        if (!K->gpio_get(WIFI_ACK)) {               // ready is ACK low
            K->gpio_put(WIFI_CS, 0);
            return wait_ack(true, 100);
        }
        myrtos_sleep(4);
    }
    return false;
}

static void deselect_chip(void) {
    K->gpio_put(WIFI_CS, 1);
    K->busy_wait_us(100);                           // let the line settle
}

void myrtos_wifi_init(void) {
    K->spi_init(WIFI_SPI, 8 * 1000 * 1000);
    K->gpio_set_function(WIFI_SCK,  MYRTOS_GPIO_FUNC_SPI);
    K->gpio_set_function(WIFI_MOSI, MYRTOS_GPIO_FUNC_SPI);
    K->gpio_set_function(WIFI_MISO, MYRTOS_GPIO_FUNC_SPI);

    K->gpio_init(WIFI_CS);
    K->gpio_set_dir(WIFI_CS, MYRTOS_GPIO_OUT);
    K->gpio_put(WIFI_CS, 1);

    K->gpio_init(WIFI_ACK);
    K->gpio_set_dir(WIFI_ACK, MYRTOS_GPIO_IN);
}

// Ask the chip what firmware it is running. A version string coming back settles
// three things at once: the wiring, the handshake, and that the chip really does
// speak NINA rather than something that would have needed a stack of our own.
// Returns 0, or which of the three ways it failed -- the caller can then say so
// where the caller's output goes, rather than the kernel saying it on a console
// the asker may not be looking at.
int32_t myrtos_wifi_firmware(char *out, uint32_t max) {
    if (!select_chip()) {
        // Say what the line is actually doing rather than only that it did not
        // move. Reading it with each pull in turn tells driven from floating: a
        // driven line ignores the pull, a floating one follows it.
        K->gpio_set_pulls(WIFI_ACK, false, false);
        bool bare = K->gpio_get(WIFI_ACK);
        K->gpio_set_pulls(WIFI_ACK, true, false);
        K->busy_wait_us(50);
        bool with_up = K->gpio_get(WIFI_ACK);
        K->gpio_set_pulls(WIFI_ACK, false, true);
        K->busy_wait_us(50);
        bool with_down = K->gpio_get(WIFI_ACK);
        K->gpio_set_pulls(WIFI_ACK, false, false);

        if (max >= 16) {
            const char *v = bare ? "1" : "0";
            out[0] = 'a'; out[1] = 'c'; out[2] = 'k'; out[3] = '=';
            out[4] = v[0];
            out[5] = ' '; out[6] = 'u'; out[7] = 'p'; out[8] = '=';
            out[9] = with_up ? '1' : '0';
            out[10] = ' '; out[11] = 'd'; out[12] = 'n'; out[13] = '=';
            out[14] = with_down ? '1' : '0';
            out[15] = 0;
        }
        return -1;
    }
    xfer(START_CMD);
    xfer(GET_FW_VERSION_CMD & ~REPLY_FLAG);
    xfer(0);                                     // no parameters
    xfer(END_CMD);
    deselect_chip();

    if (!select_chip()) { return -2; }               // took it, never came back

    // The chip pads with 0xFF until it has something; read past that to the
    // start byte rather than assuming the first byte is meaningful.
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) {
        b = xfer(0xff);
        if (b == START_CMD || b == ERR_CMD) break;
    }
    if (b != START_CMD) { deselect_chip(); return -3; }   // answered, but not 0xE0

    uint8_t cmd = xfer(0xff);
    uint8_t nparam = xfer(0xff);
    if (cmd != (GET_FW_VERSION_CMD | REPLY_FLAG) || nparam != 1) {
        deselect_chip();
        return -4;                                   // wrong command or count
    }

    uint32_t len = xfer(0xff);
    uint32_t i = 0;
    for (; i < len; i++) {
        uint8_t v = xfer(0xff);
        if (i < max - 1) out[i] = (char)v;
    }
    out[i < max ? i : max - 1] = 0;
    xfer(0xff);                                  // END_CMD
    deselect_chip();
    return 0;
}

// Boot only sets the pins up. Asking the chip anything is what the `wifi`
// command is for: a line printed among thirty others at startup has scrolled
// past before anyone can read it, and this is a line worth reading.
void myrtos_wifi_probe(void) {
    myrtos_wifi_init();
}


// --- SCANNING ---------------------------------------------------------------
// Listing what is on the air needs no credentials: the chip is told to look, and
// asked afterwards what it found. Two commands, because the looking takes
// seconds and the answer is not ready when the first one returns.

#define WIFI_MAX_NETS 16
#define WIFI_SSID_MAX 33

// The list lives in PSRAM. Half a kilobyte is not much until SRAM is 300 kB of
// framebuffer and a kernel, and a scan result is touched once a minute at most.
typedef struct {
    char    ssid[WIFI_MAX_NETS][WIFI_SSID_MAX];
    int32_t rssi[WIFI_MAX_NETS];
} wifi_scan_t;



static wifi_scan_t *scan;
static uint32_t scan_count;
static uint8_t  start_reply;
static uint8_t  conn_status = 0xfe;

// What actually came back on the wire, so it can be reported instead of
// inferred. Three attempts at this problem have been guesses about the
// protocol; the bytes settle it.
static char     trace[32];
static uint32_t trace_n;
static void trace_reset(void) { trace_n = 0; trace[0] = 0; }
static void trace_ch(char c)  { if (trace_n < sizeof(trace) - 1) { trace[trace_n++] = c; trace[trace_n] = 0; } }
static void trace_hex(uint8_t v) {
    const char h[] = "0123456789abcdef";
    trace_ch(h[v >> 4]); trace_ch(h[v & 15]);
}

static bool scan_room(void) {
    if (scan) return true;
    scan = (wifi_scan_t*)K->bulk_alloc(sizeof(wifi_scan_t));
    return scan != 0;
}

// Read a reply whose parameter count is the answer rather than being known in
// advance -- one parameter per network found.
static int32_t read_list(uint8_t cmd) {
    uint8_t b = 0;
    trace_ch('L');
    for (int i = 0; i < 64; i++) {
        b = xfer(0xff);
        if (b == START_CMD || b == ERR_CMD) break;
    }
    trace_hex(b);
    if (b != START_CMD) return -1;
    uint8_t rc = xfer(0xff);
    trace_hex(rc);
    if (rc != (cmd | REPLY_FLAG)) return -1;

    uint32_t n = xfer(0xff);
    trace_ch('n'); trace_hex((uint8_t)n);
    if (n > WIFI_MAX_NETS) n = WIFI_MAX_NETS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = xfer(0xff);
        for (uint32_t k = 0; k < len; k++) {
            uint8_t v = xfer(0xff);
            if (k < WIFI_SSID_MAX - 1) scan->ssid[i][k] = (char)v;
        }
        scan->ssid[i][len < WIFI_SSID_MAX ? len : WIFI_SSID_MAX - 1] = 0;
    }
    xfer(0xff);                                  // END_CMD
    return (int32_t)n;
}

// --- FRAMING, WRITTEN ONCE ------------------------------------------------
// The commands above each spell the frame out, which was fine while there were
// five of them. The socket commands below are nine more, two of them with
// parameters longer than a byte can count, so the shape is written here instead.
//
// The reference pads every command to a multiple of four and the chip is
// entitled to expect it -- so the length is counted rather than worked out per
// command, which is what made param_cmd's "pad 6 to 8" a comment that has to be
// right by hand.
static uint32_t tx_len;

static void tx_begin(uint8_t cmd, uint8_t nparam) {
    xfer(START_CMD); xfer(cmd); xfer(nparam);
    tx_len = 3;
}

static void tx_param(const uint8_t *p, uint8_t len) {
    xfer(len); tx_len++;
    for (uint8_t i = 0; i < len; i++) { xfer(p[i]); tx_len++; }
}

static void tx_param8(uint8_t v) { tx_param(&v, 1); }

// Big-endian, which is this protocol's order for a port number and not ours.
static void tx_param16(uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    tx_param(b, 2);
}

// A parameter whose own length needs two bytes. SEND_DATA_TCP and
// GET_DATABUF_TCP are the only commands that use this form, and they use it
// because a buffer may be longer than 255 bytes -- which for a web server it
// always is.
static void tx_param_long(const uint8_t *p, uint16_t len) {
    xfer((uint8_t)(len >> 8)); xfer((uint8_t)len); tx_len += 2;
    for (uint16_t i = 0; i < len; i++) { xfer(p[i]); tx_len++; }
}

static void tx_end(void) {
    xfer(END_CMD); tx_len++;
    while (tx_len & 3u) { xfer(0xff); tx_len++; }
}

// The head of a reply: how many parameters follow, or -1 if the chip said
// something else. The skip loop is the one the commands above already use --
// the chip may send filler before it starts.
static int32_t rx_begin(uint8_t cmd) {
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
    if (b != START_CMD) return -1;
    if (xfer(0xff) != (cmd | REPLY_FLAG)) return -1;
    return (int32_t)xfer(0xff);
}

// One parameter, up to four bytes, as a little-endian number. That is the order
// the chip answers in, which is not the order it is asked in.
static uint32_t rx_param_u32(void) {
    uint32_t len = xfer(0xff);
    uint32_t v = 0;
    for (uint32_t i = 0; i < len && i < 4; i++) v |= (uint32_t)xfer(0xff) << (8 * i);
    for (uint32_t i = 4; i < len; i++) (void)xfer(0xff);
    return v;
}

static bool simple_cmd(uint8_t cmd) {
    if (!select_chip()) return false;
    xfer(START_CMD); xfer(cmd); xfer(0); xfer(END_CMD);
    deselect_chip();
    return true;
}

// The signal strength for one entry, a four-byte little-endian negative number.
#define GET_MACADDR_CMD     0x22u
#define SET_PASSPHRASE_CMD  0x11u
#define GET_IPADDR_CMD      0x21u

// One byte of parameter, padded to a multiple of four. The reference pads every
// command and this code did not: START, cmd, nparam, len, value, END is six
// bytes, and the chip is entitled to expect eight.
static bool param_cmd(uint8_t cmd, uint8_t value) {
    if (!select_chip()) return false;
    xfer(START_CMD); xfer(cmd); xfer(1);
    xfer(1); xfer(value);
    xfer(END_CMD);
    xfer(0xff); xfer(0xff);                      // pad 6 to 8
    deselect_chip();
    return true;
}

static int32_t rssi_of(uint32_t index) {
    if (!select_chip()) return 0;
    xfer(START_CMD); xfer(GET_IDX_RSSI_CMD); xfer(1);
    xfer(1); xfer((uint8_t)index);               // one parameter, one byte
    xfer(END_CMD);
    deselect_chip();

    if (!select_chip()) return 0;
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
    int32_t v = 0;
    if (b == START_CMD && xfer(0xff) == (GET_IDX_RSSI_CMD | REPLY_FLAG) && xfer(0xff) == 1) {
        uint32_t len = xfer(0xff);
        uint32_t raw = 0;
        for (uint32_t i = 0; i < len && i < 4; i++) raw |= (uint32_t)xfer(0xff) << (8 * i);
        xfer(0xff);
        v = (int32_t)raw;
    }
    deselect_chip();
    return v;
}

// Join a network. The request carries the name and the secret as two
// NUL-terminated strings back to back, so nothing has to be copied here: the
// sender is blocked in send, and its buffer therefore cannot move.
//
// SET_PASSPHRASE takes two parameters and the frame is padded to a multiple of
// four, which is the shape the reference sends and the shape that made the MAC
// command work. Then the chip is asked what it thinks, until it says connected
// or the patience runs out.
// The address the network gave us, which is the only proof that joining it
// achieved anything: associating is the chip's business, but an address means
// DHCP answered. The reply carries three parameters -- address, mask, gateway --
// so this reads all three and reports the first and the last.
int32_t myrtos_wifi_ipaddr(char *out, uint32_t max) {
    uint8_t p[3][4] = {{0}};
    if (!param_cmd(GET_IPADDR_CMD, 0xff)) return -1;
    if (!select_chip()) return -1;

    uint8_t b = 0;
    for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
    bool ok = false;
    if (b == START_CMD) {
        uint8_t rc = xfer(0xff);
        uint32_t np = xfer(0xff);
        if (rc == (GET_IPADDR_CMD | REPLY_FLAG)) {
            for (uint32_t k = 0; k < np; k++) {
                uint32_t len = xfer(0xff);
                for (uint32_t j = 0; j < len; j++) {
                    uint8_t v = xfer(0xff);
                    if (k < 3 && j < 4) p[k][j] = v;
                }
            }
            ok = np >= 1;
        }
        xfer(0xff);
    }
    deselect_chip();
    if (!ok) return -1;

    uint32_t o = 0;
    for (int k = 0; k < 3; k += 2) {                 // address, then gateway
        const char *tag = k ? " gw " : "ip ";
        for (const char *t = tag; *t && o < max - 1; t++) out[o++] = *t;
        for (int j = 0; j < 4; j++) {
            uint8_t v = p[k][j];
            if (v >= 100 && o < max - 1) out[o++] = (char)('0' + v / 100);
            if (v >= 10  && o < max - 1) out[o++] = (char)('0' + (v / 10) % 10);
            if (o < max - 1) out[o++] = (char)('0' + v % 10);
            if (j < 3 && o < max - 1) out[o++] = '.';
        }
    }
    out[o] = 0;
    return (p[0][0] || p[0][1] || p[0][2] || p[0][3]) ? 0 : -1;
}

int32_t myrtos_wifi_connect(const char *ssid, const char *pass) {
    uint32_t sl = 0, pl = 0;
    while (ssid[sl]) sl++;
    while (pass[pl]) pl++;
    if (!sl || sl > 32 || pl > 63) return -1;

    if (!select_chip()) return -1;
    xfer(START_CMD); xfer(SET_PASSPHRASE_CMD); xfer(2);
    xfer((uint8_t)sl); for (uint32_t i = 0; i < sl; i++) xfer((uint8_t)ssid[i]);
    xfer((uint8_t)pl); for (uint32_t i = 0; i < pl; i++) xfer((uint8_t)pass[i]);
    xfer(END_CMD);
    for (uint32_t n = 6 + sl + pl; n % 4; n++) xfer(0xff);      // pad
    deselect_chip();

    // Setting a passphrase makes the chip associate, which takes a while.
    if (!select_chip_slow(15000)) return -1;
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
    int32_t accepted = -1;
    if (b == START_CMD) {
        uint8_t rc = xfer(0xff);
        uint8_t np = xfer(0xff);
        if (rc == (SET_PASSPHRASE_CMD | REPLY_FLAG) && np == 1) {
            uint32_t len = xfer(0xff);
            for (uint32_t i = 0; i < len; i++) { uint8_t v = xfer(0xff); if (!i) accepted = v; }
        }
        xfer(0xff);
    }
    deselect_chip();
    if (accepted < 0) return -1;

    // WL_CONNECTED is 3. Ask until it says so, or for twenty seconds.
    for (int i = 0; i < 40; i++) {
        myrtos_sleep(500);
        if (!simple_cmd(GET_CONN_STATUS_CMD) || !select_chip()) continue;
        uint8_t c = 0;
        for (int k = 0; k < 64; k++) { c = xfer(0xff); if (c == START_CMD || c == ERR_CMD) break; }
        int32_t st = -1;
        if (c == START_CMD) {
            xfer(0xff);
            uint32_t np = xfer(0xff);
            for (uint32_t k = 0; k < np; k++) {
                uint32_t len = xfer(0xff);
                for (uint32_t j = 0; j < len; j++) { uint8_t v = xfer(0xff); if (!k && !j) st = v; }
            }
            xfer(0xff);
        }
        deselect_chip();
        if (st == 3) return 0;                    // WL_CONNECTED
        if (st == 4 || st == 6) return st;        // failed, or disconnected
    }
    return -2;                                    // still trying when we gave up
}

int32_t myrtos_wifi_scan(int32_t index, char *out, uint32_t max) {
    if (!scan_room()) return -1;

    if (index < 0) {
        scan_count = 0;

        // Ask its connection status first. The Arduino example waits on this in
        // a loop before it does anything else, and a question the reference
        // implementation asks first is worth asking first: whatever it wakes up
        // in the firmware, we want woken too.
        trace_reset();
        bool sent = simple_cmd(GET_CONN_STATUS_CMD);
        bool took = sent && select_chip();
        trace_ch(sent ? 's' : 'S');       // capital means it failed
        trace_ch(took ? 'k' : 'K');
        if (took) {
            uint8_t b = 0;
            for (int i = 0; i < 64; i++) { b = xfer(0xff); trace_hex(b); if (b == START_CMD || b == ERR_CMD) break; }
            if (b == START_CMD) {
                trace_ch('|');
                uint8_t rc = xfer(0xff); trace_hex(rc);
                uint32_t np = xfer(0xff); trace_hex((uint8_t)np);
                for (uint32_t k = 0; k < np; k++) {
                    uint32_t len = xfer(0xff); trace_ch('.'); trace_hex((uint8_t)len);
                    for (uint32_t j = 0; j < len; j++) {
                        uint8_t v = xfer(0xff); trace_hex(v);
                        if (k == 0 && j == 0) conn_status = v;
                    }
                }
                trace_ch('|');
                trace_hex(xfer(0xff));
            }
            deselect_chip();
        }
        // The MAC probe that used to be here is gone. It was a diagnostic, and
        // what it proved -- that a parameterised command needs its frame padded
        // to a multiple of four -- is now simply done.

        // Asking for the list without a scan running does not merely fail: the
        // chip stops raising READY afterwards and the next command times out.
        // Tried once, measured, and not again.

        myrtos_sleep(50);
        if (!simple_cmd(START_SCAN_CMD)) return -1;

        // Read the acknowledgement properly rather than throwing bytes away: a
        // frame left half-read is a frame the next command has to recover from.
        // Keep what the start command answered. The library checks it and gives
        // up when it says failure; throwing it away is how a refusal turns into
        // "no networks", which sends the reader looking in the wrong place.
        start_reply = 0xfe;
        if (select_chip()) {
            uint8_t b = 0;
            for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
            trace_ch('S'); trace_hex(b);
            if (b == ERR_CMD) start_reply = 0xef;
            if (b == START_CMD) {
                uint8_t rc = xfer(0xff); trace_hex(rc);      // command | reply
                uint32_t np = xfer(0xff); trace_hex((uint8_t)np);
                for (uint32_t k = 0; k < np; k++) {
                    uint32_t len = xfer(0xff); trace_ch('.'); trace_hex((uint8_t)len);
                    for (uint32_t j = 0; j < len; j++) {
                        uint8_t v = xfer(0xff); trace_hex(v);
                        if (k == 0 && j == 0) start_reply = v;
                    }
                }
                trace_ch('|'); trace_hex(xfer(0xff));        // END_CMD
            }
            deselect_chip();
        }

        // The chip is off looking, and it takes seconds. Asking too soon gets an
        // empty list rather than an error, which reads as "no networks" and is
        // worse than waiting -- two seconds, ten times, is what the Arduino
        // library waits and it is not being cautious for nothing.
        for (int tries = 0; tries < 3 && scan_count == 0; tries++) {
            // myrtos_sleep, not the SDK's sleep_ms. The SDK's spins, and a
            // kernel thread that spins never reaches the scheduler: it is only
            // preempted where it makes a system call. Two seconds of spinning
            // froze the whole machine, which is what the serial shell going
            // quiet alongside the keyboard was saying.
            // No delay before asking. The wait is not ours to do -- the chip
            // scans inside the command and answers when it is done.
            // As the reference does it, because reading the reference settled
            // that there is no alternative: GET_IDX_SSID_CMD = 0x31 is
            // commented out in wifi_spi.h, so the command does not exist and
            // the ERR the chip answered it with was correct. The list is the
            // only way to get names, and the chip refuses that too.
            if (!simple_cmd(SCAN_NETWORKS_CMD)) continue;
            if (!select_chip_slow(15000)) { trace_ch('T'); continue; }
            int32_t n = read_list(SCAN_NETWORKS_CMD);
            deselect_chip();
            if (n > 0) scan_count = (uint32_t)n;
        }
        for (uint32_t i = 0; i < scan_count; i++) scan->rssi[i] = rssi_of(i);

        // Nothing found: say what the chip said when told to look, since that is
        // the only part of the exchange we have not been able to see.
        if (scan_count == 0 && out && max >= 12) {
            const char hex[] = "0123456789abcdef";
            uint32_t o = 0;
            const char *p = "start=";
            while (*p && o < max - 1) out[o++] = *p++;
            if (o < max - 1) out[o++] = hex[(start_reply >> 4) & 15];
            if (o < max - 1) out[o++] = hex[start_reply & 15];
            if (o < max - 1) out[o++] = ' ';
            p = trace;
            while (*p && o < max - 1) out[o++] = *p++;
            out[o] = 0;
        }
        return (int32_t)scan_count;
    }

    if ((uint32_t)index >= scan_count) return -1;
    uint32_t i = 0;
    while (i < max - 1 && scan->ssid[index][i]) { out[i] = scan->ssid[index][i]; i++; }
    out[i] = 0;
    return scan->rssi[index];
}


// --- THE SERVICE ------------------------------------------------------------
// Every call here waits: for a handshake, or for a scan that takes twenty
// seconds. Done inside a trap that is twenty seconds with interrupts off, and
// the keyboard -- bit-banged on PIO and polled every millisecond -- does not
// survive it. This is the fifth time that root cause has surfaced in this
// project and the second time the answer was a process of its own.



static int32_t server_pid = -1;

int32_t myrtos_wifi_server_pid(void) { return server_pid; }

// --- SOCKETS ---------------------------------------------------------------
// The chip carries the TCP/IP stack, so this is not a stack: it is nine
// commands and some bookkeeping. Which is the whole argument for the NINA part
// being where it is -- a web server on this machine costs a driver, not a port
// of lwIP.
//
// The flow is Arduino's WiFiServer, because it is nina-fw's flow:
//
//   GET_SOCKET            -> a free socket number
//   START_SERVER_TCP      -> that socket now listens on a port
//   AVAIL_DATA_TCP(server)-> the socket of a client with something to say,
//                            or 255 when there is nobody
//   GET_DATABUF_TCP       -> read from the client's socket
//   SEND_DATA_TCP         -> write to it
//   STOP_CLIENT_TCP       -> close it
//
// Note what AVAIL_DATA_TCP means, because it is not what its name suggests: on
// a LISTENING socket it answers with a client socket number, and on a client
// socket it answers with a byte count. One command, two meanings, decided by
// which socket it is asked about.
#define GET_SOCKET_CMD        0x3Fu
#define START_SERVER_TCP_CMD  0x28u
#define AVAIL_DATA_TCP_CMD    0x2Bu
#define STOP_CLIENT_TCP_CMD   0x2Eu
#define GET_CLIENT_STATE_CMD  0x2Fu
#define SEND_DATA_TCP_CMD     0x44u
#define GET_DATABUF_TCP_CMD   0x45u
#define DATA_SENT_TCP_CMD     0x2Au
#define GET_STATE_TCP_CMD     0x29u

// What the chip says a socket is doing. These are TCP's own state names and the
// numbering is nina-fw's, which is lwIP's underneath.
#define TCP_CLOSED  0u
#define TCP_LISTEN  1u

#define TCP_MODE 0u
#define NO_SOCKET 255u

// --- WHOSE SOCKET IS IT ----------------------------------------------------
// The chip has no idea a process has died, and a listening socket it still
// holds keeps the port. That is not hypothetical: killing httpd and starting it
// again gave "the chip would not listen", because port 80 was still bound to a
// socket belonging to a process that no longer existed.
//
// So a socket is owned, the way a path is owned, and the owner going away
// releases it -- which is what myrtos_io_close_all does for paths in reap().
// The difference is that this cannot be done in reap(): releasing a socket
// means SPI transactions with handshakes and waits, and reap runs in kernel
// context where waiting stops the machine.
//
// Hence two halves. The kernel MARKS, which is a memory write and safe
// anywhere; the wifi thread SWEEPS, at the top of the next request it handles,
// where talking to the chip is what it is for. An orphan therefore lingers
// until somebody asks for something -- and the somebody is the next listen,
// which is exactly the call that needs the port back.
// Ten, which is nina-fw's own number and not a round one of ours. Sixteen was
// a guess and it cost the network: sockstat asked GET_STATE_TCP about sockets
// 10 to 15, the firmware indexed past its array, and the association went with
// it. Ask a chip only about things it has.
#define NINA_SOCKETS 10
#define OWNER_NONE   (-1)
#define OWNER_DEAD   (-2)

// Defined below, beside the commands that use it most; the bookkeeping above
// needs to ask the chip a question too.
static int32_t sock_cmd_u8(uint8_t cmd, uint8_t arg);

static int32_t  sock_owner[NINA_SOCKETS];
// Which port a socket was put to listening on, so a server can be found again.
// Zero means it is not a server.
static uint16_t sock_port[NINA_SOCKETS];
static bool     owners_ready;

static void owners_init(void) {
    if (owners_ready) return;
    for (int i = 0; i < NINA_SOCKETS; i++) { sock_owner[i] = OWNER_NONE; sock_port[i] = 0; }
    owners_ready = true;
}

static void own(int32_t sock, int32_t pid) {
    owners_init();
    if (sock >= 0 && sock < NINA_SOCKETS) sock_owner[sock] = pid;
}

static void disown(int32_t sock) {
    own(sock, OWNER_NONE);
    if (sock >= 0 && sock < NINA_SOCKETS) sock_port[sock] = 0;
}

// What the chip believes about a socket, which is the only opinion that counts
// -- but only for a socket it can have. Out of range is answered here rather
// than passed on, because passing it on is what took the network down.
int32_t myrtos_wifi_state(uint8_t sock)
{
    if (sock >= NINA_SOCKETS) return -1;
    return sock_cmd_u8(GET_STATE_TCP_CMD, sock);
}

// Who asked for it, for the same command to report. OWNER_NONE and OWNER_DEAD
// come through as themselves.
int32_t myrtos_wifi_owner(uint8_t sock)
{
    owners_init();
    return sock < NINA_SOCKETS ? sock_owner[sock] : OWNER_NONE;
}

int32_t myrtos_wifi_port_of(uint8_t sock)
{
    owners_init();
    return sock < NINA_SOCKETS ? (int32_t)sock_port[sock] : 0;
}

// Called from the kernel when a process is reaped. Marks only.
void myrtos_wifi_forget_pid(int32_t pid) {
    owners_init();
    for (int i = 0; i < NINA_SOCKETS; i++)
        if (sock_owner[i] == pid) sock_owner[i] = OWNER_DEAD;
}

// A command with one byte of parameter and one number back. Six of the nine are
// this shape.
static int32_t sock_cmd_u8(uint8_t cmd, uint8_t arg)
{
    if (!select_chip()) return -1;
    tx_begin(cmd, 1);
    tx_param8(arg);
    tx_end();
    deselect_chip();

    if (!select_chip()) return -1;
    int32_t np = rx_begin(cmd);
    int32_t v = (np == 1) ? (int32_t)rx_param_u32() : -1;
    if (np >= 0) (void)xfer(0xff);                 // END_CMD
    deselect_chip();
    return v;
}

// Close whatever the dead have left behind. Runs in the wifi thread, where
// talking to the chip is allowed, at the top of every request.
//
// A LISTENING socket is left alone on purpose, and this is the correction to
// the first attempt at this. Stopping one frees the socket NUMBER without
// tearing down the listener behind it, so the port stayed taken while a fresh
// bind reported success and then answered nothing -- worse than the failure it
// replaced, because it failed silently. nina-fw offers no way to stop a server;
// Arduino's WiFiServer has no end() either, which is the same fact seen from
// the other side.
//
// So a server outlives its process and is ADOPTED by the next one that asks for
// that port. See myrtos_wifi_listen.
static void sweep_orphans(void)
{
    owners_init();
    for (int i = 0; i < NINA_SOCKETS; i++) {
        if (sock_owner[i] != OWNER_DEAD) continue;
        if (sock_port[i]) { sock_owner[i] = OWNER_NONE; continue; }   // a server, kept
        sock_owner[i] = OWNER_NONE;
        (void)sock_cmd_u8(STOP_CLIENT_TCP_CMD, (uint8_t)i);
    }
}

// Take a socket and put it to listening on a port. Returns the socket, or -1.
//
// A server left behind by a process that has gone is adopted rather than
// replaced. The chip has no command that stops one, so the choice is between
// handing back the listener that already exists and leaving the port unusable
// until the board is restarted -- and the first is what the caller wanted.
//
// It is checked against the chip and not against this table alone: the state
// has to still be LISTEN. A socket this side believes in and the chip has
// forgotten is exactly the situation that produced a server which bound
// successfully and then never answered.
int32_t myrtos_wifi_listen(uint16_t port)
{
    owners_init();
    for (int i = 0; i < NINA_SOCKETS; i++) {
        if (sock_port[i] != port) continue;
        if (myrtos_wifi_state((uint8_t)i) == (int32_t)TCP_LISTEN) return i;
        sock_port[i] = 0;                          // stale: the chip disagrees
    }

    int32_t sock = sock_cmd_u8(GET_SOCKET_CMD, 0xff);
    if (sock < 0 || sock == (int32_t)NO_SOCKET) return -1;

    if (!select_chip()) return -1;
    tx_begin(START_SERVER_TCP_CMD, 3);
    tx_param16(port);
    tx_param8((uint8_t)sock);
    tx_param8(TCP_MODE);
    tx_end();
    deselect_chip();

    if (!select_chip()) return -1;
    int32_t np = rx_begin(START_SERVER_TCP_CMD);
    uint32_t ok = (np == 1) ? rx_param_u32() : 0;
    if (np >= 0) (void)xfer(0xff);
    deselect_chip();
    if (!ok) return -1;
    if (sock < NINA_SOCKETS) sock_port[sock] = port;
    return sock;
}

// Is anybody there? The client's socket, or -1 for nobody. Asked repeatedly by
// whoever is serving, so it must be cheap and must not block.
int32_t myrtos_wifi_accept(uint8_t server_sock)
{
    int32_t v = sock_cmd_u8(AVAIL_DATA_TCP_CMD, server_sock);
    if (v < 0 || v == (int32_t)NO_SOCKET || v == (int32_t)server_sock) return -1;
    return v;
}

// Read what a client has sent. Zero means nothing yet, not end of stream -- the
// caller decides how long to keep asking, because only the caller knows what it
// is waiting for.
int32_t myrtos_wifi_recv(uint8_t sock, uint8_t *buf, uint32_t len)
{
    if (!len) return 0;
    if (len > 4000u) len = 4000u;                  // the chip's own buffer limit

    if (!select_chip()) return -1;
    tx_begin(GET_DATABUF_TCP_CMD, 2);
    // Both parameters carry two-byte lengths in this command, the socket
    // included -- the length prefix is a property of the command and not of the
    // parameter, which is the part that is easy to get wrong.
    uint8_t s = sock;
    tx_param_long(&s, 1);
    uint8_t want[2] = { (uint8_t)(len & 0xffu), (uint8_t)(len >> 8) };
    tx_param_long(want, 2);
    tx_end();
    deselect_chip();

    if (!select_chip()) return -1;
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
    if (b != START_CMD) { deselect_chip(); return -1; }
    int32_t got = -1;
    if (xfer(0xff) == (GET_DATABUF_TCP_CMD | REPLY_FLAG) && xfer(0xff) == 1) {
        // And the reply's length is two bytes as well, little-endian.
        uint32_t lo = xfer(0xff), hi = xfer(0xff);
        uint32_t n = lo | (hi << 8);
        if (n > len) n = len;
        for (uint32_t i = 0; i < n; i++) buf[i] = xfer(0xff);
        (void)xfer(0xff);                          // END_CMD
        got = (int32_t)n;
    }
    deselect_chip();
    return got;
}

// Hand a block to the chip and wait for it to say it went. The wait matters:
// without it a close can overtake the data, and the browser gets an empty
// answer for a page that was written correctly.
int32_t myrtos_wifi_send(uint8_t sock, const uint8_t *buf, uint32_t len)
{
    if (!len) return 0;
    if (len > 2000u) len = 2000u;                  // one chip buffer at a time

    if (!select_chip()) return -1;
    tx_begin(SEND_DATA_TCP_CMD, 2);
    uint8_t s = sock;
    tx_param_long(&s, 1);
    tx_param_long(buf, (uint16_t)len);
    tx_end();
    deselect_chip();

    if (!select_chip()) return -1;
    int32_t np = rx_begin(SEND_DATA_TCP_CMD);
    uint32_t sent = (np == 1) ? rx_param_u32() : 0;
    if (np >= 0) (void)xfer(0xff);
    deselect_chip();
    if (!sent) return -1;

    // Yielding, not spinning. This runs in the wifi thread above the shell, and
    // the lesson about spinning here was learned once already at the scan.
    for (int i = 0; i < 100; i++) {
        if (sock_cmd_u8(DATA_SENT_TCP_CMD, sock) == 1) return (int32_t)sent;
        myrtos_sleep(2);
    }
    return (int32_t)sent;
}

int32_t myrtos_wifi_close(uint8_t sock)
{
    disown(sock);
    return sock_cmd_u8(STOP_CLIENT_TCP_CMD, sock) >= 0 ? 0 : -1;
}

static int32_t handle(const myrtos_msg_t *m, int32_t from) {
    const myrtos_wifi_req_t *r = (const myrtos_wifi_req_t*)m->data;
    switch (m->type) {
    case MYRTOS_MSG_WIFI_VER:  return myrtos_wifi_firmware(r->buf, r->len);
    case MYRTOS_MSG_WIFI_SCAN: return myrtos_wifi_scan(r->index, r->buf, r->len);
    case MYRTOS_MSG_WIFI_ADDR: return myrtos_wifi_ipaddr(r->buf, r->len);
    case MYRTOS_MSG_WIFI_SOCK: {
        const myrtos_wifi_sock_t *q = (const myrtos_wifi_sock_t*)m->data;
        // Before anything else, and cheap when there is nothing to do: a socket
        // whose owner has been reaped is closed here, because this is the first
        // place after the reaping where the chip may be spoken to.
        sweep_orphans();
        switch (q->op) {
        case MYRTOS_SOCK_LISTEN: {
            int32_t s = myrtos_wifi_listen((uint16_t)q->arg);
            own(s, from);
            return s;
        }
        case MYRTOS_SOCK_ACCEPT: {
            int32_t c = myrtos_wifi_accept((uint8_t)q->arg);
            own(c, from);
            return c;
        }
        case MYRTOS_SOCK_RECV:   return myrtos_wifi_recv((uint8_t)q->arg, q->buf, q->len);
        case MYRTOS_SOCK_SEND:   return myrtos_wifi_send((uint8_t)q->arg, q->buf, q->len);
        case MYRTOS_SOCK_CLOSE:  return myrtos_wifi_close((uint8_t)q->arg);
        // Diagnostics. Three numbers about one socket, which is what was
        // missing while three explanations were argued over in an evening.
        case MYRTOS_SOCK_STATE:  return myrtos_wifi_state((uint8_t)q->arg);
        case MYRTOS_SOCK_OWNER:  return myrtos_wifi_owner((uint8_t)q->arg);
        case MYRTOS_SOCK_PORT:   return myrtos_wifi_port_of((uint8_t)q->arg);
        default:                 return -1;
        }
    }
    case MYRTOS_MSG_WIFI_JOIN: {
        // name, NUL, secret, NUL -- in the caller's own memory, which is stable
        // because the caller is blocked in send.
        const char *ssid = r->buf;
        const char *pass = ssid;
        while (*pass) pass++;
        return myrtos_wifi_connect(ssid, pass + 1);
    }
    default:                   return -1;
    }
}

static void wifi_thread(void) {
    for (;;) {
        myrtos_msg_t m;
        int32_t from = myrtos_receive(&m);
        if (from < 0) continue;
        // Who asked, which is who owns whatever socket comes back. The kernel
        // blocks the sender until the reply, so this pid is alive right now --
        // and if it dies later, reap tells us.
        myrtos_reply(handle(&m, from));
    }
}

void myrtos_wifi_start_server(void) {
    // Below the USB task, which is the thing this exists to stop starving.
    server_pid = K->kernel_thread(wifi_thread, 2048, MYRTOS_PRIO_WIFI);
    if (server_pid < 0) K->print("WiFi: could not start its service process\n");
}

// --- WHAT THE KERNEL CALLS -------------------------------------------------
// Entry zero takes the kernel's own table and must be called first; everything
// else here reaches the hardware through it, so calling anything else before it
// would be following a null pointer. The kernel checks the ABI word before it
// calls entry zero, and entry zero checks it again -- once on each side of an
// interface is not one time too many when the alternative is a wild jump.
static bool wifi_lib_init(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_lib_table_t myrtos_lib = {
    .abi   = MYRTOS_LIB_ABI,
    .count = 5,
    .fn    = {
        (void*)wifi_lib_init,            // 0: take the kernel's table
        (void*)myrtos_wifi_probe,        // 1: find the chip and say what it is
        (void*)myrtos_wifi_start_server, // 2: start the thread that serves it
        (void*)myrtos_wifi_server_pid,   // 3: who to send to, or -1
        (void*)myrtos_wifi_forget_pid,   // 4: this process is gone; mark, do not talk
    },
};

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
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define WIFI_SPI    spi1
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

static uint8_t xfer(uint8_t v) {
    uint8_t r = 0;
    spi_write_read_blocking(WIFI_SPI, &v, &r, 1);
    return r;
}

// Wait for the handshake line to reach a level, giving up rather than spinning.
static bool wait_ack(bool level, uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (gpio_get(WIFI_ACK) != level) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
    }
    return true;
}

static bool select_chip(void) {
    if (!wait_ack(false, 10)) return false;      // not busy
    gpio_put(WIFI_CS, 0);
    return wait_ack(true, 10);                   // selected and ready
}

static void deselect_chip(void) { gpio_put(WIFI_CS, 1); }

void myrtos_wifi_init(void) {
    spi_init(WIFI_SPI, 8 * 1000 * 1000);
    gpio_set_function(WIFI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(WIFI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(WIFI_MISO, GPIO_FUNC_SPI);

    gpio_init(WIFI_CS);
    gpio_set_dir(WIFI_CS, GPIO_OUT);
    gpio_put(WIFI_CS, 1);

    gpio_init(WIFI_ACK);
    gpio_set_dir(WIFI_ACK, GPIO_IN);
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
        gpio_set_pulls(WIFI_ACK, false, false);
        bool bare = gpio_get(WIFI_ACK);
        gpio_set_pulls(WIFI_ACK, true, false);
        busy_wait_us(50);
        bool with_up = gpio_get(WIFI_ACK);
        gpio_set_pulls(WIFI_ACK, false, true);
        busy_wait_us(50);
        bool with_down = gpio_get(WIFI_ACK);
        gpio_set_pulls(WIFI_ACK, false, false);

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

extern void *myrtos_tlsf_malloc(void *pool, uint32_t size);
extern void *myrtos_bulk_pool;

static wifi_scan_t *scan;
static uint32_t scan_count;

static bool scan_room(void) {
    if (scan) return true;
    if (!myrtos_bulk_pool) return false;
    scan = (wifi_scan_t*)myrtos_tlsf_malloc(myrtos_bulk_pool, sizeof(wifi_scan_t));
    return scan != 0;
}

// Read a reply whose parameter count is the answer rather than being known in
// advance -- one parameter per network found.
static int32_t read_list(uint8_t cmd) {
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) {
        b = xfer(0xff);
        if (b == START_CMD || b == ERR_CMD) break;
    }
    if (b != START_CMD) return -1;
    if (xfer(0xff) != (cmd | REPLY_FLAG)) return -1;

    uint32_t n = xfer(0xff);
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

static bool simple_cmd(uint8_t cmd) {
    if (!select_chip()) return false;
    xfer(START_CMD); xfer(cmd); xfer(0); xfer(END_CMD);
    deselect_chip();
    return true;
}

// The signal strength for one entry, a four-byte little-endian negative number.
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

int32_t myrtos_wifi_scan(int32_t index, char *out, uint32_t max) {
    if (!scan_room()) return -1;

    if (index < 0) {
        scan_count = 0;
        if (!simple_cmd(START_SCAN_CMD)) return -1;

        // Read the acknowledgement properly rather than throwing bytes away: a
        // frame left half-read is a frame the next command has to recover from.
        if (select_chip()) {
            uint8_t b = 0;
            for (int i = 0; i < 64; i++) { b = xfer(0xff); if (b == START_CMD || b == ERR_CMD) break; }
            if (b == START_CMD) {
                xfer(0xff);                        // command | reply
                uint32_t np = xfer(0xff);
                for (uint32_t k = 0; k < np; k++) {
                    uint32_t len = xfer(0xff);
                    while (len--) xfer(0xff);
                }
                xfer(0xff);                        // END_CMD
            }
            deselect_chip();
        }

        // The chip is off looking, and it takes seconds. Asking too soon gets an
        // empty list rather than an error, which reads as "no networks" and is
        // worse than waiting -- two seconds, ten times, is what the Arduino
        // library waits and it is not being cautious for nothing.
        for (int tries = 0; tries < 10 && scan_count == 0; tries++) {
            sleep_ms(2000);
            if (!simple_cmd(SCAN_NETWORKS_CMD)) continue;
            if (!select_chip()) continue;
            int32_t n = read_list(SCAN_NETWORKS_CMD);
            deselect_chip();
            if (n > 0) scan_count = (uint32_t)n;
        }
        for (uint32_t i = 0; i < scan_count; i++) scan->rssi[i] = rssi_of(i);
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

#include "../common/modules.h"
#include "usbdev.h"      // the named priorities

int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);

static int32_t server_pid = -1;

int32_t myrtos_wifi_server_pid(void) { return server_pid; }

static int32_t handle(const myrtos_msg_t *m) {
    const myrtos_wifi_req_t *r = (const myrtos_wifi_req_t*)m->data;
    switch (m->type) {
    case MYRTOS_MSG_WIFI_VER:  return myrtos_wifi_firmware(r->buf, r->len);
    case MYRTOS_MSG_WIFI_SCAN: return myrtos_wifi_scan(r->index, r->buf, r->len);
    default:                   return -1;
    }
}

static void wifi_thread(void) {
    for (;;) {
        myrtos_msg_t m;
        int32_t from = myrtos_receive(&m);
        if (from < 0) continue;
        myrtos_reply(handle(&m));
    }
}

void myrtos_wifi_start_server(void) {
    // Below the USB task, which is the thing this exists to stop starving.
    server_pid = myrtos_kernel_thread(wifi_thread, 2048, MYRTOS_PRIO_WIFI);
    if (server_pid < 0) myrtos_print("WiFi: could not start its service process\n");
}

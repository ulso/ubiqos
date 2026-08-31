#include "../common/modules.h"   // the keymap type, through the shared ABI
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "hardware/structs/sysinfo.h"
#include "tusb.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

// A USB host on two PIO state machines, so a keyboard can be plugged in while
// the hardware controller stays busy being our console.
//
// The pins are the board's: D+ on GP1, D- on GP2 -- adjacent, which PIO-USB
// requires -- and the 5V supply switched on GP11.

#define USB_HOST_DP_PIN  1
#define USB_HOST_POWER   11

// The board holds its peripherals in reset until this is driven high. The USB
// hub behind the two host sockets is one of them, so nothing enumerates while
// it is low -- which looks exactly like a host that is not working.
#define PERIPH_RESET     22
#define ESP_BOOT          0   // to the ESP32-C6's GPIO9, and the BOOT button

static uint8_t keys[32];
static uint32_t head, tail;

// TinyUSB asks the port for the time. It is declared in tusb.h and defined
// nowhere in the SDK, so it is ours to supply. The hardware clock rather than
// our tick, because tuh_init runs before the scheduler's timer starts.
uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

void myrtos_usbhost_init(void) {
    // GP22 releases the USB hub, the audio DAC and the ESP32-C6 together, so the
    // ESP's reset happens here whether or not anybody wants WiFi.
    //
    // And it has to happen with GP0 high. The ESP samples its GPIO9 as it leaves
    // reset -- low means the serial bootloader, high means run the firmware --
    // and that pin is wired to GP0 on this board. An RP2350 pin comes out of
    // reset as an input with its PULL-DOWN on, so GP0 was holding the ESP in
    // bootloader mode every time. The chip had power and drove its busy line,
    // which is what made it look present but permanently not ready.
    gpio_init(ESP_BOOT);
    gpio_set_dir(ESP_BOOT, GPIO_IN);
    gpio_set_pulls(ESP_BOOT, true, false);   // pull up, and leave the button alone
    sleep_ms(1);

    gpio_init(PERIPH_RESET);
    gpio_set_dir(PERIPH_RESET, GPIO_OUT);
    gpio_put(PERIPH_RESET, 0);            // a real pulse, not just a release
    sleep_ms(10);
    gpio_put(PERIPH_RESET, 1);            // let the on-board peripherals go

    gpio_init(USB_HOST_POWER);
    gpio_set_dir(USB_HOST_POWER, GPIO_OUT);
    gpio_put(USB_HOST_POWER, 1);          // the port is dead without this

    sleep_ms(100);                        // the hub needs a moment to come up

    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = USB_HOST_DP_PIN;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);

    if (!tuh_init(1)) { myrtos_print("USB host: tuh_init failed\n"); return; }
    myrtos_print("USB host started on PIO, D+ GP1, power GP11\n");
}

void myrtos_usbhost_task(void) { tuh_task(); }

// What the driver hands out. A ring, because keys arrive in an interrupt-ish
// context and are read from a system call.
int32_t myrtos_usbhost_read(uint8_t *buf, uint32_t len) {
    uint32_t n = 0;
    while (n < len && head != tail) {
        buf[n++] = keys[tail];
        tail = (tail + 1) % sizeof(keys);
    }
    return (int32_t)n;
}

uint32_t myrtos_usbhost_available(void) {
    return (head - tail) % sizeof(keys);
}

static void push(uint8_t c) {
    uint32_t next = (head + 1) % sizeof(keys);
    if (next != tail) { keys[head] = c; head = next; }
}

// --- what TinyUSB calls back ----------------------------------------------

void tuh_mount_cb(uint8_t addr) {
    myrtos_print("USB host: device ");
    myrtos_print_u32(addr);
    myrtos_print(" attached\n");
}

void tuh_umount_cb(uint8_t addr) {
    myrtos_print("USB host: device ");
    myrtos_print_u32(addr);
    myrtos_print(" removed\n");
}

void tuh_hid_mount_cb(uint8_t addr, uint8_t instance,
                      uint8_t const *desc, uint16_t len) {
    (void)desc; (void)len;
    uint8_t proto = tuh_hid_interface_protocol(addr, instance);
    myrtos_print(proto == HID_ITF_PROTOCOL_KEYBOARD
                 ? "USB host: keyboard ready\n" : "USB host: HID device, not a keyboard\n");
    tuh_hid_receive_report(addr, instance);
}

void tuh_hid_umount_cb(uint8_t addr, uint8_t instance) {
    (void)addr; (void)instance;
    myrtos_print("USB host: HID gone\n");
}

// The layout comes from the keyboard's descriptor, which is where it belongs:
// changing it is a matter of replacing one module rather than rebuilding the
// kernel. Until a descriptor has been registered these two lines stand in --
// enough to type a command, and American whatever is printed on the keys.
static const myrtos_keymap_t *keymap;

void myrtos_usbhost_set_keymap(const myrtos_keymap_t *k) { keymap = k; }

// Auto-repeat. When a held key first stopped flooding the queue it stopped
// repeating altogether, which is the other half of the problem: a report says
// which keys are DOWN and arrives on every poll, so emitting all of them gave
// "hhjjjkkkk", and emitting only the new ones gives no repeat at all. The answer
// is neither -- it is a clock.
//
// The repeat runs from the USB task rather than from the report callback,
// because a keyboard with nothing to say answers a poll with NAK and no
// callback arrives. Waiting for one would mean a key that repeats only while
// some other key is being pressed.
#define REPEAT_DELAY_MS 400      // before the first repeat
#define REPEAT_RATE_MS   35      // between them after that

static uint8_t  repeat_key;      // 0 when nothing is held
static uint8_t  repeat_mods;
static uint32_t repeat_due;

static uint8_t translate(uint8_t k, uint8_t mods);

// Ctrl-C never reaches the queue while something is running in front of the
// screen: it ends that process instead. With nothing running it goes through as
// an ordinary character, because then there is a shell reading and it can do
// something better with it -- clearing the line -- than the kernel can.
bool myrtos_io_interrupt(const char *device_name);

static void emit(uint8_t c) {
    if (c == 3 && (myrtos_io_interrupt("con") || myrtos_io_interrupt("kbd")))
        return;
    push(c);
}

// The arrows and their neighbours, as the escape sequences every terminal has
// sent for them since the VT100. They are not in the keymap and should not be:
// a layout says which letter is on a key, and an arrow is an arrow on every
// keyboard in the world. Putting them here also means the serial port and the
// screen deliver the same bytes for the same key, which is what lets the shell
// have one idea of how to edit a line rather than two.
//
// HID usages: 0x4a Home, 0x4b PgUp, 0x4c Delete, 0x4d End, 0x4e PgDn,
// 0x4f right, 0x50 left, 0x51 down, 0x52 up.
static const char *nav_sequence(uint8_t k) {
    switch (k) {
    case 0x4a: return "\x1b[H";
    case 0x4b: return "\x1b[5~";
    case 0x4c: return "\x1b[3~";
    case 0x4d: return "\x1b[F";
    case 0x4e: return "\x1b[6~";
    case 0x4f: return "\x1b[C";
    case 0x50: return "\x1b[D";
    case 0x51: return "\x1b[B";
    case 0x52: return "\x1b[A";
    default:   return 0;
    }
}

// A whole sequence or none of it. Half an escape sequence in the queue is worse
// than a dropped keystroke: the reader would take the tail for text.
//
// Not static: the console answers a cursor-position report through here. That
// looks like a strange direction until you remember what a terminal is -- the
// screen's reply to the program goes to the program's input, and on this
// machine the console's input is the keyboard.
void myrtos_usbhost_push_str(const char *sq) {
    uint32_t n = 0;
    while (sq[n]) n++;
    uint32_t free_slots = (tail - head - 1 + sizeof(keys)) % sizeof(keys);
    if (free_slots < n) return;
    for (uint32_t i = 0; i < n; i++) push((uint8_t)sq[i]);
}

void myrtos_usbhost_repeat(void) {
    if (!repeat_key) return;
    uint32_t now = tusb_time_millis_api();
    if ((int32_t)(now - repeat_due) < 0) return;
    const char *sq = nav_sequence(repeat_key);
    if (sq) {
        myrtos_usbhost_push_str(sq);
    } else {
        uint8_t c = translate(repeat_key, repeat_mods);
        if (c) emit(c);
    }
    repeat_due = now + REPEAT_RATE_MS;
}

// Return gives a carriage return, as every terminal has since the teletype --
// not a line feed. The shell takes either and so never noticed, but cu passes
// bytes through untouched, and the device at the other end may well care: a
// BleuIO ends its commands on CR and answers nothing at all to a line feed.
static const char plain[] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\\0;'`,./";
static const char shift[] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|\0:\"~<>?";

// One keycode and the modifier byte to a character, through whichever layout is
// in force. Shared by the first press and by every repeat after it.
// Control turns a letter into the code it has stood for since teletypes: ctrl-A
// is 1, ctrl-L is 12. It is applied after the layout rather than through it, so
// it works whatever letter the key carries and needs no entry in the keymap --
// and only to letters, because ctrl with anything else has no agreed meaning
// worth inventing one for.
static uint8_t control(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 1);
    return 0;
}

static uint8_t translate(uint8_t k, uint8_t mods) {
    bool sh   = (mods & 0x22) != 0;             // either shift
    bool alt  = (mods & 0x40) != 0;             // right alt, which is AltGr
    bool ctrl = (mods & 0x11) != 0;             // either control

    if (keymap) {
        if (k >= MYRTOS_KEYMAP_KEYS) return 0;
        uint8_t c = alt ? keymap->altgr[k] : (sh ? keymap->shift[k] : keymap->plain[k]);
        // AltGr on a key with nothing there falls back to the unshifted
        // character, as it does everywhere else.
        if (alt && !c) c = keymap->plain[k];
        return ctrl ? control(c) : c;
    }
    if (k < sizeof(plain) - 1) {
        uint8_t c = (uint8_t)(sh ? shift[k] : plain[k]);
        return ctrl ? control(c) : c;
    }
    return 0;
}

// --- CDC-ACM ON THE HOST SIDE ---------------------------------------------
// The serial port that is not a serial port: a BLE dongle, a modem, a sensor.
// TinyUSB's class driver does the protocol; what is kept here is which
// interface index turned up, because the myrtos device that a process opens has
// to point at something.
//
// One at a time. The class is configured for one interface and a second would
// need a device name of its own to be reachable, which is a descriptor question
// rather than a driver one.
static int32_t cdc_index = -1;

int32_t myrtos_usbhost_cdc_index(void) { return cdc_index; }

void tuh_cdc_mount_cb(uint8_t idx) {
    cdc_index = (int32_t)idx;

    // Say which device, because "it is connected but does not answer" is a
    // question about what the device is, and nothing else here can answer it.
    tuh_itf_info_t info;
    uint16_t vid = 0, pid = 0;
    if (tuh_cdc_itf_get_info(idx, &info))
        tuh_vid_pid_get(info.daddr, &vid, &pid);

    myrtos_print("USB host: CDC-ACM ready as 'acm', ");
    myrtos_print_hex(vid);
    myrtos_print(":");
    myrtos_print_hex(pid);
    myrtos_print(tuh_cdc_get_dtr(idx) ? ", DTR high\n" : ", DTR low\n");
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (cdc_index == (int32_t)idx) cdc_index = -1;
    myrtos_print("USB host: CDC-ACM device gone\n");
}

void tuh_hid_report_received_cb(uint8_t addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (len >= 8 && tuh_hid_interface_protocol(addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        // A report lists the keys that are DOWN, not the ones just pressed, and
        // it arrives on every poll. Emitting all of them each time turned one
        // held key into a stream of them -- "hhjjjkkkk" for three keystrokes.
        // So each report is compared with the one before, and only keys that
        // were not already down produce a character.
        static uint8_t was[6];

        for (int i = 2; i < 8; i++) {
            uint8_t k = report[i];
            if (!k) continue;

            bool held = false;
            for (int j = 0; j < 6; j++) if (was[j] == k) held = true;
            if (held) continue;

            const char *sq = nav_sequence(k);
            if (sq) {
                myrtos_usbhost_push_str(sq);
            } else {
                uint8_t c = translate(k, report[0]);
                if (c) emit(c);
            }

            // The newest key down is the one that repeats, as it is everywhere:
            // hold a, then hold b, and it is b that runs away.
            repeat_key  = k;
            repeat_mods = report[0];
            repeat_due  = tusb_time_millis_api() + REPEAT_DELAY_MS;
        }
        // A key that has been let go stops repeating. Modifiers are taken
        // afresh, so shift released mid-repeat turns capitals into small ones.
        if (repeat_key) {
            bool still = false;
            for (int i = 2; i < 8; i++) if (report[i] == repeat_key) still = true;
            if (!still) repeat_key = 0;
            else repeat_mods = report[0];
        }

        for (int i = 0; i < 6; i++) was[i] = report[i + 2];
    }
    tuh_hid_receive_report(addr, instance);
}

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

static uint8_t keys[32];
static uint32_t head, tail;

// TinyUSB asks the port for the time. It is declared in tusb.h and defined
// nowhere in the SDK, so it is ours to supply. The hardware clock rather than
// our tick, because tuh_init runs before the scheduler's timer starts.
uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

void myrtos_usbhost_init(void) {
    gpio_init(PERIPH_RESET);
    gpio_set_dir(PERIPH_RESET, GPIO_OUT);
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

void myrtos_usbhost_repeat(void) {
    if (!repeat_key) return;
    uint32_t now = tusb_time_millis_api();
    if ((int32_t)(now - repeat_due) < 0) return;
    uint8_t c = translate(repeat_key, repeat_mods);
    if (c) push(c);
    repeat_due = now + REPEAT_RATE_MS;
}

static const char plain[] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\0;'`,./";
static const char shift[] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|\0:\"~<>?";

// One keycode and the modifier byte to a character, through whichever layout is
// in force. Shared by the first press and by every repeat after it.
static uint8_t translate(uint8_t k, uint8_t mods) {
    bool sh  = (mods & 0x22) != 0;              // either shift
    bool alt = (mods & 0x40) != 0;              // right alt, which is AltGr

    if (keymap) {
        if (k >= MYRTOS_KEYMAP_KEYS) return 0;
        uint8_t c = alt ? keymap->altgr[k] : (sh ? keymap->shift[k] : keymap->plain[k]);
        // AltGr on a key with nothing there falls back to the unshifted
        // character, as it does everywhere else.
        if (alt && !c) c = keymap->plain[k];
        return c;
    }
    if (k < sizeof(plain) - 1) return (uint8_t)(sh ? shift[k] : plain[k]);
    return 0;
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

            uint8_t c = translate(k, report[0]);
            if (c) push(c);

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

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

// The smallest table that turns a keycode into a character. Not a layout, and
// deliberately not one: a real one belongs in a descriptor on the card, where
// it can be changed without rebuilding the kernel.
static const char plain[] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\0;'`,./";
static const char shift[] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|\0:\"~<>?";

void tuh_hid_report_received_cb(uint8_t addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (len >= 8 && tuh_hid_interface_protocol(addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        // A report lists the keys that are DOWN, not the ones just pressed, and
        // it arrives on every poll. Emitting all of them each time turned one
        // held key into a stream of them -- "hhjjjkkkk" for three keystrokes.
        // So each report is compared with the one before, and only keys that
        // were not already down produce a character.
        static uint8_t was[6];
        bool sh = (report[0] & 0x22) != 0;          // either shift

        for (int i = 2; i < 8; i++) {
            uint8_t k = report[i];
            if (!k) continue;

            bool held = false;
            for (int j = 0; j < 6; j++) if (was[j] == k) held = true;
            if (held) continue;

            if (k < sizeof(plain) - 1) {
                char c = sh ? shift[k] : plain[k];
                if (c) push((uint8_t)c);
            }
        }
        for (int i = 0; i < 6; i++) was[i] = report[i + 2];
    }
    tuh_hid_receive_report(addr, instance);
}

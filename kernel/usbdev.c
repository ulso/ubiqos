#include "usbdev.h"
#include "tusb.h"
#include "../common/modules.h"   // myrtos_sleep, through the shared ABI

void myrtos_print(const char *s);

void myrtos_usb_init(void) {
    // TinyUSB's RP2040 port registers itself for USBCTRL_IRQ through the SDK's
    // irq_add_shared_handler. That only works now that we stopped taking over
    // mtvec: the SDK's external dispatch is what looks up in that table.
    tud_init(0);
    myrtos_print("USB device started, CDC console on the USB port\n");
}

// TinyUSB does its work here, not in the interrupt. The kernel's idle loop
// calls it, which suffices: everything time critical happens in the handler.
void myrtos_usb_task(void) {
    tud_task();
}

bool myrtos_usb_ready(void) {
    return tud_cdc_connected();
}

int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len) {
    // No gate on tud_cdc_connected(): it mirrors DTR, and a host that opened
    // the port without asserting DTR then had its data silently dropped.
    // TinyUSB buffers, and discards on its own when nobody is listening.
    if (!tud_mounted()) return -1;

    // Writes as much as fits and says how much that was. It used to call
    // tud_task here when the buffer filled, which cannot work: this runs in the
    // trap handler with interrupts off, so the transfer that would drain the
    // buffer can never complete, and the USB process may be inside tud_task at
    // the same moment. The text was simply cut off at 256 bytes.
    uint32_t written = 0;
    while (written < len) {
        // The same translation the UART driver does: a lone line feed is
        // preceded by a carriage return. Without it the output staircases to
        // the right, and every utility would have to write \r\n itself. The
        // pair goes in together or not at all, so a retry cannot repeat the CR.
        uint32_t need = (buf[written] == '\n') ? 2u : 1u;
        if (tud_cdc_write_available() < need) break;
        if (need == 2) { char cr = '\r'; tud_cdc_write(&cr, 1); }
        tud_cdc_write(buf + written, 1);
        written++;
    }
    if (written) tud_cdc_write_flush();
    return (int32_t)written;
}

// USB is serviced by a process of its own, high enough that an application
// cannot silence the console by being busy. One millisecond is far more often
// than needed: CDC data has no deadline at all -- a late poll costs throughput,
// since the host simply retries -- and the tightest real limit is the 50 ms USB
// gives a device to answer a standard request with no data stage. Fifty times
// the margin, for a sleep that costs nothing.
static void usb_thread(void) {
    extern void myrtos_usbhost_task(void);
    for (;;) {
        myrtos_usb_task();          // the console, on the hardware controller
        myrtos_usbhost_task();      // the keyboard, on PIO
        myrtos_sleep(1);
    }
}

void myrtos_usb_start_task(void) {
    extern int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes,
                                        uint32_t priority);
    if (myrtos_kernel_thread(usb_thread, 4096, MYRTOS_PRIO_USB) < 0) {
        myrtos_print("USB: could not start its service process\n");
    }
}

uint32_t myrtos_usb_writable(void) {
    return tud_mounted() ? tud_cdc_write_available() : 0;
}

uint32_t myrtos_usb_available(void) {
    return tud_cdc_connected() || tud_mounted() ? tud_cdc_available() : 0;
}

int32_t myrtos_usb_read(uint8_t *buf, uint32_t len) {
    if (!tud_mounted() || !tud_cdc_available()) return 0;
    return (int32_t)tud_cdc_read(buf, len);
}

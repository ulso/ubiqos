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
//
// Ctrl-C is looked for here rather than in the read, because while a command is
// running nobody is reading -- which is precisely when it is typed. Peeking
// rather than draining: the head of the FIFO is all TinyUSB will show without
// consuming, and everything behind it is type-ahead the shell is owed.
//
// So it is caught when it is the next byte, which it is unless something was
// typed first and left unread. Draining into a buffer of our own would close
// that gap and cost a quarter kilobyte, which this machine has not got.
bool myrtos_io_interrupt(const char *device_name);

void myrtos_usb_task(void) {
    tud_task();

    uint8_t c;
    if (tud_mounted() && tud_cdc_available() && tud_cdc_peek(&c) && c == 3) {
        if (myrtos_io_interrupt("usb")) {
            tud_cdc_read(&c, 1);        // consumed: it was never data
        }
    }
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
    static char last_out;       // the byte before this one, for the CR above
    uint32_t written = 0;
    while (written < len) {
        // The same translation the UART driver does: a LONE line feed is
        // preceded by a carriage return. Without it the output staircases to
        // the right, and every utility would have to write \r\n itself. The
        // pair goes in together or not at all, so a retry cannot repeat the CR.
        //
        // Lone is the point. It used to add one to every line feed, so a stream
        // that already had its own came out as \r\r\n -- which a terminal
        // forgives and a program passing bytes through should not be doing at
        // all. Remembered across calls, because a write may end on the CR.
        bool lone = (buf[written] == '\n') && (last_out != '\r');
        uint32_t need = lone ? 2u : 1u;
        if (tud_cdc_write_available() < need) break;
        if (lone) { char cr = '\r'; tud_cdc_write(&cr, 1); }
        tud_cdc_write(buf + written, 1);
        last_out = (char)buf[written];
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
        { extern void myrtos_usbhost_drain(void); myrtos_usbhost_drain(); }
        { extern void myrtos_usbhost_repeat(void); myrtos_usbhost_repeat(); }
        // A refused request for the next HID report is retried here rather than
        // being the end of the keyboard. See the note in usbhost.c.
        { extern void myrtos_usbhost_rearm(void); myrtos_usbhost_rearm(); }
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

#include "usbdev.h"
#include "tusb.h"

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

    uint32_t written = 0;
    while (written < len) {
        // The same translation the UART driver does: a lone line feed is
        // preceded by a carriage return. Without it the output staircases to
        // the right, and every utility would have to write \r\n itself.
        if (buf[written] == '\n') {
            char cr = '\r';
            if (!tud_cdc_write(&cr, 1)) { tud_cdc_write_flush(); tud_task(); continue; }
        }
        uint32_t n = tud_cdc_write(buf + written, 1);
        written += n;
        if (!n) {
            // The FIFO is full: let the stack drain it before we go on.
            tud_cdc_write_flush();
            tud_task();
            if (!tud_mounted()) break;
        }
    }
    tud_cdc_write_flush();
    return (int32_t)written;
}

uint32_t myrtos_usb_available(void) {
    return tud_cdc_connected() || tud_mounted() ? tud_cdc_available() : 0;
}

int32_t myrtos_usb_read(uint8_t *buf, uint32_t len) {
    if (!tud_mounted() || !tud_cdc_available()) return 0;
    return (int32_t)tud_cdc_read(buf, len);
}

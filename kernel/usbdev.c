#include "usbdev.h"
#include "tusb.h"

void myrtos_print(const char *s);

void myrtos_usb_init(void) {
    // TinyUSB:s RP2040-port registrerar sig själv för USBCTRL_IRQ genom SDK:ns
    // irq_add_shared_handler. Det fungerar först sedan vi slutade ta över
    // mtvec: SDK:ns externa dispatch är den som slår upp i den tabellen.
    tud_init(0);
    myrtos_print("USB device started, CDC console on the USB port\n");
}

// TinyUSB gör sitt arbete här, inte i avbrottet. Kärnans tomgångsprocess
// anropar den, vilket räcker: allt tidskritiskt sker i avbrottshanteraren.
void myrtos_usb_task(void) {
    tud_task();
}

bool myrtos_usb_ready(void) {
    return tud_cdc_connected();
}

int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len) {
    // Ingen grind på tud_cdc_connected(): den speglar DTR, och en värd som
    // öppnar porten utan att sätta DTR fick då sina data tysta kastade.
    // TinyUSB buffrar och kastar själv när ingen lyssnar.
    if (!tud_mounted()) return -1;

    uint32_t written = 0;
    while (written < len) {
        // Samma översättning som UART-drivrutinen gör: en ensam radmatning
        // föregås av vagnretur. Utan den trappar utskriften åt höger, och
        // verktygen skulle behöva skriva \r\n överallt.
        if (buf[written] == '\n') {
            char cr = '\r';
            if (!tud_cdc_write(&cr, 1)) { tud_cdc_write_flush(); tud_task(); continue; }
        }
        uint32_t n = tud_cdc_write(buf + written, 1);
        written += n;
        if (!n) {
            // FIFO:n är full: låt stacken tömma den innan vi fortsätter.
            tud_cdc_write_flush();
            tud_task();
            if (!tud_mounted()) break;
        }
    }
    tud_cdc_write_flush();
    return (int32_t)written;
}

int32_t myrtos_usb_read(uint8_t *buf, uint32_t len) {
    if (!tud_mounted() || !tud_cdc_available()) return 0;
    return (int32_t)tud_cdc_read(buf, len);
}

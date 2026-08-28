#ifndef MYRTOS_USBDEV_H
#define MYRTOS_USBDEV_H

#include <stdint.h>
#include <stdbool.h>

// USB device i myrtos. TinyUSB bor i kärnan och inte i en modul: den är full
// av statiska tabeller med funktionspekare och skulle aldrig klara
// positionsoberoendekontrollen. Enheten exponeras i stället som vilken annan
// enhet som helst, genom en beskrivare.
//
// Avbrottet går genom vår egen dispatch, inte SDK:ns vektortabell.

void myrtos_usb_init(void);
void myrtos_usb_task(void);      // måste köras regelbundet; kärnans tomgång gör det
bool myrtos_usb_ready(void);
int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len);
int32_t myrtos_usb_read(uint8_t *buf, uint32_t len);

#endif

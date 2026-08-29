#ifndef MYRTOS_USBDEV_H
#define MYRTOS_USBDEV_H

#include <stdint.h>
#include <stdbool.h>

// USB device support in myrtos. TinyUSB lives in the kernel rather than in a
// module: it is full of static tables of function pointers and would never pass
// the position-independence check. The device is instead exposed like any
// other, through a descriptor.
//
// The interrupt goes through the SDK's vector table. TinyUSB registers itself
// with irq_add_shared_handler, which is why the kernel no longer owns mtvec.

void myrtos_usb_init(void);
void myrtos_usb_task(void);      // must run regularly
void myrtos_usb_start_task(void); // starts the process that does so

// Above anything an application is given by default, so a busy program cannot
// stop the console being serviced. Below the top, which is left free.
#define MYRTOS_PRIO_USB 30
bool myrtos_usb_ready(void);
int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len);
int32_t myrtos_usb_read(uint8_t *buf, uint32_t len);
uint32_t myrtos_usb_available(void);   // bytes waiting, without consuming them

#endif

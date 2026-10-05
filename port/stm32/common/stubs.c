// What the rest of the kernel expects from subsystems this board does not have
// yet, answered as absence. Each block says what the missing hardware is and
// what the answer means, so that a command that asks gets "not here" rather
// than a hang or a fault.
//
// These go one by one as the port grows: the key store went when the flash
// driver was written, the network when lwIP came; the USB device waits for
// TinyUSB.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../../common/ubiqos_abi.h"
#include "keystore.h"
#include "usbdev.h"
#include "sdcard.h"
#include "fat32.h"

// --- USB device: none on the boards built without it --------------------------
// The Feather STM32F405 has it: kernel/usbdev.c and TinyUSB. The NUCLEO boards
// do not yet.
#if !UBIQOS_STM32_USB
void     stm32_usb_hw_init(void) { }
void     ubiqos_usb_init(void) { }
void     ubiqos_usb_start_task(void) { }
int32_t  ubiqos_usb_write(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; return -1; }
int32_t  ubiqos_usb_read(uint8_t *buf, uint32_t len) { (void)buf; (void)len; return 0; }
uint32_t ubiqos_usb_available(void) { return 0; }
uint32_t ubiqos_usb_writable(void) { return 0; }
void     ubiqos_usb_net_id(const uint8_t *unique, uint32_t n) { (void)unique; (void)n; }
bool     ubiqos_msc_hand_over(void) { return false; }
void     ubiqos_msc_take_back(void) { }
bool     ubiqos_msc_host_has_card(void) { return false; }
#else
// The card lent to the host: no card driver, and no storage function either.
bool     ubiqos_msc_hand_over(void) { return false; }
void     ubiqos_msc_take_back(void) { }
bool     ubiqos_msc_host_has_card(void) { return false; }
#endif

// --- USB host: no keyboard, no /dev/acm -------------------------------------
void     ubiqos_usbhost_queue_init(void) { }
int32_t  ubiqos_usbhost_read(uint8_t *buf, uint32_t len) { (void)buf; (void)len; return 0; }
uint32_t ubiqos_usbhost_available(void) { return 0; }
int32_t  ubiqos_usbhost_cdc_index(void) { return -1; }
uint32_t ubiqos_usbhost_cdc_id(void) { return 0; }
int32_t  ubiqos_usbhost_cdc_reset(void) { return -1; }
uint32_t ubiqos_usbhost_info(uint32_t what) { (void)what; return 0; }
void     ubiqos_usbhost_set_keymap(const ubiqos_keymap_t *k) { (void)k; }
void     ubiqos_usbhost_drain(void) { }

// --- PIO, WiFi: RP2350 and Fruit Jam hardware -----------------------------
void     ubiqos_pio_probe(void) { }
void     ubiqos_wifi_start_server(void) { }
int32_t  ubiqos_wifi_server_pid(void) { return -1; }
void     ubiqos_wifi_forget_pid(int32_t pid) { (void)pid; }

// --- The SD card: none on this board --------------------------------------
bool     ubiqos_sd_have_driver(void) { return false; }
bool     ubiqos_sd_init(void) { return false; }
bool     ubiqos_sd_try_sdio(void) { return false; }
bool     ubiqos_sd_is_sdio(void) { return false; }
bool     ubiqos_sd_failed(void) { return true; }
void     ubiqos_sd_forget(void) { }
bool     ubiqos_fat_mount(void) { return false; }
int32_t  ubiqos_fat_stat(const char *path, uint32_t *size_out) { (void)path; (void)size_out; return -1; }
const ubiqos_fsops_t *ubiqos_fat_ops_ptr(void) { return NULL; }

// --- The C library's heap: there is none ------------------------------------
// mDNS formats with snprintf, and newlib's formatter can reach malloc -- not for
// a fixed buffer, which is all it is given, but the reference is enough to pull
// malloc in, and malloc wants sbrk. On the RP2350 the pico-sdk's own printf
// stands in and none of it is linked. Here sbrk refuses: a malloc that is ever
// called gets NULL, which is an answer, rather than memory from nowhere.
#include <errno.h>
void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}

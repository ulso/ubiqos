// Not the pico-sdk but TinyUSB, and not TinyUSB either: the few CDC-host calls
// kernel/io.c makes for /dev/acm, answered as a host with nothing plugged in.
// The USB host comes to this port later, with a TinyUSB new enough to have a
// driver for the H5's controller; until then /dev/acm says it is absent.
//
// A board with the USB device gets TinyUSB itself as well -- the next tusb.h
// on the include path, the real one -- with its host side built out, so these
// stand in for it there too.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#if UBIQOS_STM32_USB
#include_next "tusb.h"
#endif
static inline bool     tuh_cdc_mounted(uint8_t idx)                { (void)idx; return false; }
static inline uint32_t tuh_cdc_write_available(uint8_t idx)        { (void)idx; return 0; }
static inline uint32_t tuh_cdc_write(uint8_t idx, const void *b, uint32_t n) { (void)idx; (void)b; (void)n; return 0; }
static inline uint32_t tuh_cdc_write_flush(uint8_t idx)            { (void)idx; return 0; }
static inline uint32_t tuh_cdc_read(uint8_t idx, void *b, uint32_t n) { (void)idx; (void)b; (void)n; return 0; }
static inline uint32_t tuh_cdc_read_available(uint8_t idx)         { (void)idx; return 0; }

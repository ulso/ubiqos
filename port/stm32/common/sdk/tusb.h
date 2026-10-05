// Not the pico-sdk but TinyUSB, and not TinyUSB either: the few CDC-host calls
// kernel/io.c makes for /dev/acm, answered as a host with nothing plugged in.
// The USB host comes to this port later, with a TinyUSB new enough to have a
// driver for the H5's controller; until then /dev/acm says it is absent.
#pragma once
#include <stdint.h>
#include <stdbool.h>
static inline bool     tuh_cdc_mounted(uint8_t idx)                { (void)idx; return false; }
static inline uint32_t tuh_cdc_write_available(uint8_t idx)        { (void)idx; return 0; }
static inline uint32_t tuh_cdc_write(uint8_t idx, const void *b, uint32_t n) { (void)idx; (void)b; (void)n; return 0; }
static inline uint32_t tuh_cdc_write_flush(uint8_t idx)            { (void)idx; return 0; }
static inline uint32_t tuh_cdc_read(uint8_t idx, void *b, uint32_t n) { (void)idx; (void)b; (void)n; return 0; }
static inline uint32_t tuh_cdc_read_available(uint8_t idx)         { (void)idx; return 0; }

// See ../README.md. The SDK's two flash calls, over the chip's own controller:
// offsets from the start of flash, whole sectors erased, whole sectors written.
// The key store and the settings take their sector size from here -- see each
// family's flash.c for what the controller needs.
#pragma once
#include <stdint.h>
#include <stddef.h>

#if UBIQOS_STM32_F4
// The F4's sectors run from 16 kB to 128 kB; this is the small ones', the
// only ones a key store or the settings are put in.
#define FLASH_SECTOR_SIZE 0x4000u
#define FLASH_PAGE_SIZE   4u          // a word, the unit programmed at 2.7 V and up
#else
// The H5's sector is 8 kB, not the RP2350's 4 -- and its CMSIS header says so
// as well, under the same name.
#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 0x2000u
#endif
#define FLASH_PAGE_SIZE   16u         // a quad-word, the unit the H5 programs in
#endif

void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);

// See ../README.md. The SDK's two flash calls, over the H5's own controller:
// offsets from the start of flash, whole sectors erased, whole sectors written.
// The H5's sector is 8 kB, not the RP2350's 4, and the key store takes its
// size from here -- see flash.c for what the controller needs.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 0x2000u
#endif
#define FLASH_PAGE_SIZE   16u         // a quad-word, the unit the H5 programs in

void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);

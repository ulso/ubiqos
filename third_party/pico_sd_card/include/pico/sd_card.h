/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_SD_CARD_H
#define _PICO_SD_CARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pico.h"

#define SD_OK (0)
#define SD_ERR_STUCK (-1)
#define SD_ERR_BAD_RESPONSE (-2)
#define SD_ERR_CRC (-3)
#define SD_ERR_BAD_PARAM (-4)

#ifndef PICO_SD_CLK_PIN
#define PICO_SD_CLK_PIN 23
#endif

#ifndef PICO_SD_CMD_PIN
#define PICO_SD_CMD_PIN 24
#endif

#ifndef PICO_SD_DAT0_PIN
#define PICO_SD_DAT0_PIN 19
#endif

// LOCAL CHANGE: which PIO block the driver takes. Upstream names pio1
// throughout -- the block, the DMA requests and the pin function -- and on a
// board where pio1 is somebody else's that is five places to be wrong in. One
// number now, pio1 unless the build says otherwise. The block's GPIO window is
// the caller's to set, as before.
#ifndef MYRTOS_SD_PIO_INDEX
#define MYRTOS_SD_PIO_INDEX 1
#endif

// todo for now
#define PICO_SD_MAX_BLOCK_COUNT 32
// todo buffer pool
int sd_init_4pins();
int sd_init_1pin();
#define SD_SECTOR_SIZE 512
int sd_readblocks_sync(uint32_t *buf, uint32_t block, uint block_count);
int sd_readblocks_async(uint32_t *buf, uint32_t block, uint block_count);
int sd_readblocks_scatter_async(uint32_t *control_words, uint32_t block, uint block_count);
void sd_set_byteswap_on_read(bool swap);
bool sd_scatter_read_complete(int *status);
int sd_writeblocks_async(const uint32_t *data, uint32_t sector_num, uint sector_count);
bool sd_write_complete(int *status);

// LOCAL: bounded wait for the card to leave the programming state after a
// write. See the definition in sd_card.c.
int sd_wait_not_busy(uint32_t ms);

// LOCAL: one line of driver state, for after a failure.
void sd_dump_state(void);

// LOCAL: the bus has stopped answering. Set by the first wait that times out;
// every wait after it returns SD_ERR_STUCK without spinning or printing.
// Cleared only by bringing the card up again.
bool sd_bus_dead(void);
void sd_bus_revive(void);
int sd_read_sectors_1bit_crc_async(uint32_t *sector_buf, uint32_t sector, uint sector_count);
int sd_set_wide_bus(bool wide);
int sd_set_clock_divider(uint div);

// LOCAL: whether the card said it was SDHC or SDXC as it came ready. Those are
// addressed by block; a standard-capacity card is addressed by BYTE, and the
// driver passes whatever it is given straight to CMD17 and CMD24 -- so for such
// a card the caller multiplies by 512. Upstream assumed high capacity.
bool sd_is_high_capacity(void);

// LOCAL CHANGE: the driver's three DMA buffers are allocated rather than
// declared, so that this can be built into a library module whose own memory is
// PSRAM -- which DMA cannot use. Ask for the size, hand it SRAM, once, before
// sd_init_4pins. See the note at the definitions.
uint32_t sd_dma_buffer_words(void);
void sd_set_dma_buffers(uint32_t *sram);

#endif

#ifdef __cplusplus
}
#endif
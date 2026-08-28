#ifndef MYRTOS_SDCARD_H
#define MYRTOS_SDCARD_H

#include <stdint.h>
#include <stdbool.h>

// SD cards in SPI mode. The Fruit Jam has the card on SPI0: SCK GP34, MOSI
// GP35, MISO GP36, CS GP39, and card detect on GP33.
//
// SPI mode is slower than SDIO but needs no PIO and no four-bit bus, which is
// ample for reading modules in.

bool    myrtos_sd_init(void);
bool    myrtos_sd_read_block(uint32_t lba, uint8_t *buf);
bool    myrtos_sd_present(void);

#endif

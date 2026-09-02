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

// Write one 512-byte block. The card is polled until it releases the bus, so a
// caller that gets true can issue the next command straight away.
bool myrtos_sd_write_block(uint32_t lba, const uint8_t *buf);
// myrtos_sd_present was here. GP33 is not connected on this board -- see the
// note at SD_DETECT_PIN in sdcard.c.

// Four-bit SDIO, asked for rather than assumed -- see the note in sdcard.c.
bool    myrtos_sd_try_sdio(void);
bool    myrtos_sd_is_sdio(void);

// True once the card has stopped answering. Cleared by mounting it again.
bool    myrtos_sd_failed(void);

// The card has gone. Refuse both buses until something mounts again, and when
// it does, start from CMD0 rather than from what the last card was doing.
void    myrtos_sd_forget(void);
extern const bool myrtos_sd_sdio_writes_allowed;   // false: SDIO reads only

#endif

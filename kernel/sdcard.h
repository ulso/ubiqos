#ifndef MYRTOS_SDCARD_H
#define MYRTOS_SDCARD_H

#include <stdint.h>
#include <stdbool.h>

// SD-kort i SPI-läge. Fruit Jam har korten på SPI0: SCK GP34, MOSI GP35,
// MISO GP36, CS GP39, och korddetektering på GP33.
//
// SPI-läget är långsammare än SDIO men kräver ingen PIO och ingen
// fyrbitarsbuss, vilket räcker gott för att läsa in moduler.

bool    myrtos_sd_init(void);
bool    myrtos_sd_read_block(uint32_t lba, uint8_t *buf);
bool    myrtos_sd_present(void);

#endif

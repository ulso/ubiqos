#ifndef MYRTOS_FAT32_H
#define MYRTOS_FAT32_H

#include <stdint.h>
#include <stdbool.h>

// Läsbart FAT32, tillräckligt för att hämta moduler från kortet: en partition,
// korta 8.3-namn, rotkatalogen. Skrivning, långa namn och underkataloger
// finns inte, och ska läggas till när något behöver dem.

bool     myrtos_fat_mount(void);
int32_t  myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len);

// Räkna upp filer i rotkatalogen med en given ändelse. index börjar på noll;
// returnerar false när det inte finns fler. name_out fylls med elva tecken
// plus nolltecken, alltså rå 8.3-form som myrtos_fat_read_file tar.
bool     myrtos_fat_find_nth(const char *ext_3, uint32_t index, char *name_out);

#endif

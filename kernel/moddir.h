#ifndef MYRTOS_MODDIR_H
#define MYRTOS_MODDIR_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"

// Modulkatalogen, i OS-9:s mening. En modul finns i minnet i EN kopia hur många
// processer som än kör den; varje process har bara sitt eget dataområde.
// Länkräknaren avgör när kopian får försvinna, precis som F$Link och F$UnLink.
//
// Att det går bygger på att modulen inte har några skrivbara data: bygget
// kontrollerar att .data och .bss är tomma, så koden kan delas utan att två
// processer trampar på varandra.

#define MYRTOS_MAX_MODULES 8

typedef struct {
    const myrtos_module_header_t *header;
    uint32_t links;             // hur många processer som kör den
    void    *owned;             // heapminne att lämna tillbaka, NULL om resident
    char     name[12];
} myrtos_module_entry_t;

void  myrtos_moddir_init(void);

// Registrera en modul som redan ligger läsbart i minnet -- i flash, eller i en
// buffert som inte ska frigöras. Ingen kopiering sker.
bool  myrtos_moddir_add_resident(const myrtos_module_header_t *header, const char *name);

// Kopiera in en modul i heapen en gång och registrera den.
bool  myrtos_moddir_add_copy(const uint8_t *src, uint32_t len, const char *name);

// Slå upp och räkna upp länken. Returnerar NULL om modulen inte finns.
const myrtos_module_header_t *myrtos_moddir_link(const char *name);

// Slå upp på ett användarskrivet namn: skiftlägesokänsligt, utan utfyllnad
// och utan ändelse. "mdir", "MDIR" och "Mdir" hittar alla samma modul.
const char *myrtos_moddir_match(const char *user_name);
void  myrtos_moddir_unlink(const myrtos_module_header_t *header);

uint32_t myrtos_moddir_count(void);
const myrtos_module_entry_t *myrtos_moddir_entry(uint32_t index);

#endif

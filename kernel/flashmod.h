#ifndef MYRTOS_FLASHMOD_H
#define MYRTOS_FLASHMOD_H

#include <stdint.h>

// Modulregionen i flash. Kärnbilden slutar strax efter 24 kB; en megabyte in
// ger den gott om luft att växa. Resten -- närmare 15 MB -- är moduler.
#define MYRTOS_FLASH_MODULE_BASE 0x10100000u
#define MYRTOS_FLASH_END         0x11000000u

// Sök igenom flashregionen efter modulhuvuden och registrera det som hittas
// som residenta moduler. De körs där de ligger och kopieras aldrig -- exakt
// vad OS-9 gjorde med ROM-moduler, och skälet till att ett modulsystem inte
// behöver något filsystem för att hitta kod.
uint32_t myrtos_flash_scan(void);

#endif

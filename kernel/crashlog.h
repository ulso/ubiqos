#ifndef MYRTOS_CRASHLOG_H
#define MYRTOS_CRASHLOG_H

#include <stdint.h>

// What the machine leaves behind when it stops. Read it with the probe:
//   nm os_kernel.elf | grep myrtos_crash    -- then mem32 that address 5
// See crashlog.c for why this exists.
#define MYRTOS_CRASH_MAGIC 0x43524148u   // "CRAH"

#define MYRTOS_CRASH_PANIC 1   // a = format string, b = caller
#define MYRTOS_CRASH_TRAP  2   // a = mepc, b = mcause, c = mtval

typedef struct {
    uint32_t magic;
    uint32_t kind;
    uint32_t a, b, c;
} myrtos_crash_t;

extern myrtos_crash_t myrtos_crash;

void myrtos_crash_note (uint32_t kind, uint32_t a, uint32_t b, uint32_t c);

#endif

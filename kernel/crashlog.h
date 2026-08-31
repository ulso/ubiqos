#ifndef MYRTOS_CRASHLOG_H
#define MYRTOS_CRASHLOG_H

#include <stdint.h>

// What the machine leaves behind when it stops. Read it with the probe:
//   nm os_kernel.elf | grep myrtos_crash    -- then mem32 that address 5
// See crashlog.c for why this exists.
#define MYRTOS_CRASH_MAGIC 0x43524148u   // "CRAH"

#define MYRTOS_CRASH_PANIC  1   // a = format string, b = caller
#define MYRTOS_CRASH_TRAP   2   // a = mepc, b = mcause, c = mtval
#define MYRTOS_CRASH_ASSERT 3   // a = where the ebreak was

// The crash record keeps the first assertion, because the first is the one that
// says why. These keep the tally, so a stack asserting on every poll is visible
// as more than one line printed long ago.
extern uint32_t myrtos_asserts_seen;
extern uint32_t myrtos_assert_last;

typedef struct {
    uint32_t magic;
    uint32_t kind;
    uint32_t a, b, c;
} myrtos_crash_t;

extern myrtos_crash_t myrtos_crash;

void myrtos_crash_note (uint32_t kind, uint32_t a, uint32_t b, uint32_t c);

#endif

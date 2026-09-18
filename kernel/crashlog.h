#ifndef UBIQOS_CRASHLOG_H
#define UBIQOS_CRASHLOG_H

#include <stdint.h>

// What the machine leaves behind when it stops. Read it with the probe:
//   nm os_kernel.elf | grep ubiqos_crash    -- then mem32 that address 5
// See crashlog.c for why this exists.
#define UBIQOS_CRASH_MAGIC 0x43524148u   // "CRAH"

#define UBIQOS_CRASH_PANIC  1   // a = format string, b = caller
#define UBIQOS_CRASH_TRAP   2   // a = pc, b = cause, c = faulting address
#define UBIQOS_CRASH_ASSERT 3   // a = where the ebreak was

// The crash record keeps the first assertion, because the first is the one that
// says why. These keep the tally, so a stack asserting on every poll is visible
// as more than one line printed long ago.
extern uint32_t ubiqos_asserts_seen;
extern uint32_t ubiqos_assert_last;

typedef struct {
    uint32_t magic;
    uint32_t kind;
    uint32_t a, b, c;
} ubiqos_crash_t;

extern ubiqos_crash_t ubiqos_crash;

void ubiqos_crash_note (uint32_t kind, uint32_t a, uint32_t b, uint32_t c);

// Put the last crash in the log, once, and forget it. Called at boot: the
// record lives in memory the startup code does not clear, so it survives the
// reset that was needed to get the machine back.
void ubiqos_crash_report(void);

#endif

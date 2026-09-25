#include "../../common/ubiqos_abi.h"

// stackbomb -- runs out of stack on purpose, to show what the kernel does.
//
// Each level of the recursion keeps 256 bytes on the stack and says how deep it
// is. A module gets 4 kB of data and stack together, so it should get a dozen
// or so levels down before the stack limit stops it: on Arm the kernel then
// ends this process and says so, and the shell carries on. Before PSPLIM it
// went on down through its own thread-local block and whatever the allocator
// kept below that, and the damage showed up later somewhere else.
//
// A test, not a command: built, not resident. Put it on the card to run it.

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

__attribute__((noinline))
static uint32_t down(uint32_t level)
{
    volatile uint8_t keep[256];
    for (uint32_t i = 0; i < sizeof keep; i++) keep[i] = (uint8_t)(level + i);

    char line[24];
    uint32_t n = 0, v = level;
    char digits[10];
    uint32_t d = 0;
    do { digits[d++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (const char *p = "level "; *p; p++) line[n++] = *p;
    while (d) line[n++] = digits[--d];
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    say(line);

    return keep[level & 0xffu] + down(level + 1u);   // not a tail call
}

void module_main(void)
{
    say("stackbomb: going down until the stack runs out\r\n");
    down(1);
    say("stackbomb: came back, which it should not have\r\n");
}

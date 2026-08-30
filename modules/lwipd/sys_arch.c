#include "../../common/myrtos_abi.h"
#include "lwip/sys.h"

// The clock lwIP measures its timeouts against. Milliseconds since the timer
// started, which is what myrtos counts anyway.
u32_t sys_now(void) {
    return (u32_t)myrtos_ticks_now();
}

// Randomness for mDNS's reply delay. A xorshift is ample: what it prevents is
// several machines answering the same query in the same millisecond, not
// anything an adversary is interested in.
static uint32_t seed;

uint32_t myrtos_lwip_rand(void) {
    if (!seed) seed = myrtos_ticks_now() | 1u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

void myrtos_lwip_assert(const char *why) {
    myrtos_write_str(MYRTOS_STDOUT, "lwip: ");
    myrtos_write_str(MYRTOS_STDOUT, why ? why : "assertion failed");
    myrtos_write_str(MYRTOS_STDOUT, "\n");
    myrtos_exit();
}

#include <stddef.h>

// memcmp, one byte at a time, in place of the C library's.
//
// The RISC-V toolchain's newlib memcmp compares a word at a time without
// looking at alignment first, and Hazard3 traps on a misaligned load (mcause
// 4). Nothing on Arm shows it -- the Cortex-M33 takes a misaligned word
// without complaint -- so the RISC-V network builds went out in two releases
// and died on the first packet: lwIP compares addresses and mDNS compares
// names at wherever they fall in a frame, and the first link-up announcement
// was enough. memcpy, memmove, memset and strlen in the same library do check,
// which is why this was the only one to go.
//
// Defined here, it is found before the library's. Byte at a time because what
// this compares is short -- six-byte addresses, name labels -- and a loop that
// is right on both machines is worth more than one that is quick on one.
// Loop-pattern recognition is off so the compiler cannot turn this loop back
// into a call to itself.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return (int)*p - (int)*q;
    return 0;
}

// See ../README.md: PRIMASK, as the SDK does it on the Cortex-M33.
#pragma once
#include <stdint.h>

static inline uint32_t save_and_disable_interrupts(void)
{
    uint32_t st;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(st) :: "memory");
    return st;
}

static inline void restore_interrupts(uint32_t st)
{
    __asm__ volatile("msr primask, %0" :: "r"(st) : "memory");
}

static inline void __dmb(void) { __asm__ volatile("dmb" ::: "memory"); }
static inline void __dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void __isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void __wfi(void) { __asm__ volatile("wfi"); }
static inline void tight_loop_contents(void) { }

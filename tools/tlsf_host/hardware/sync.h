// Stands in for the SDK's on the host. The allocator's critical sections are
// against interrupts, and there are none here.
#pragma once
#include <stdint.h>
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }

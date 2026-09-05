#ifndef MYRTOS_TRAP_H
#define MYRTOS_TRAP_H

#include <stdint.h>

// The frame, and the few questions the shared code asks about a trap, come
// from whichever machine this is being built for. Everything below the include
// is the same on both.
//
// The frames themselves are not alike and there is no use pretending: RISC-V
// saves thirty-three words because it must save them all, and Cortex-M33 saves
// eighteen because the core pushes the other half itself. What is alike is the
// contract -- the handler is given a frame and answers with the frame to resume
// -- and the names the syscall ABI uses: the number in a7, the arguments in a0
// to a2, the answer in a0.
#if defined(__riscv)
#include "riscv/trap.h"
#elif defined(__arm__) || defined(__thumb__)
#include "arm/trap.h"
#else
#error "myrtos does not know this machine"
#endif

uint32_t myrtos_switch(uint32_t current_sp);
void myrtos_process_exit(void);
void myrtos_timer_rearm(void);
void myrtos_timer_init(uint32_t interval_ticks);
uint64_t myrtos_timer_now(void);

#endif

#ifndef MYRTOS_TRAP_H
#define MYRTOS_TRAP_H

#include <stdint.h>

// Måste stämma exakt med sparordningen i myrtos_trap_vector (scheduler.S).
// FRAME_SIZE där är 144, alltså 33 ord plus utfyllnad till 16-byte-justering.
typedef struct {
    uint32_t ra, gp, tp;
    uint32_t t0, t1, t2;
    uint32_t s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;
    uint32_t mepc, mcause, mtval;
    uint32_t _pad[3];
} myrtos_frame_t;

_Static_assert(sizeof(myrtos_frame_t) == 144, "ramen måste matcha FRAME_SIZE");

uint32_t myrtos_switch(uint32_t current_sp);
void myrtos_process_exit(void);
void myrtos_timer_rearm(void);
void myrtos_timer_init(uint32_t interval_ticks);
uint64_t myrtos_timer_now(void);

#endif

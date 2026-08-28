// The system call layer. In OS-9 everything went through SWI2; here it is
// ecall, and the trap vector in scheduler.S is the way in.

#include <stdint.h>
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "moddir.h"
#include "tlsf.h"

void myrtos_print(const char *s);
void myrtos_putc(char c);
int32_t myrtos_current_pid(void);
uint32_t myrtos_process_count(void);
int32_t myrtos_process_create(const myrtos_module_header_t *m, const char *args);
uint32_t myrtos_process_get_args(char *buf, uint32_t len);
extern tlsf_pool_t myrtos_mem_pool;

// The system call numbers come from common/myrtos_abi.h, shared with modules.

#define MCAUSE_INTERRUPT_BIT    0x80000000u
#define MCAUSE_CODE_MASK        0x7fffffffu
#define MCAUSE_ECALL_M          11u
#define MCAUSE_MACHINE_TIMER     7u
#define MCAUSE_MACHINE_EXTERNAL 11u

volatile uint32_t myrtos_ticks = 0;
volatile uint32_t myrtos_trap_count = 0;
volatile uint32_t myrtos_last_mcause = 0;
volatile uint32_t myrtos_last_mepc = 0;

// The return value is the stack pointer to resume. Same in as out means we
// continue in the same process; a different one is a context switch.
uint32_t myrtos_trap_handler(myrtos_frame_t *frame) {
    uint32_t sp = (uint32_t)(uintptr_t)frame;

    myrtos_trap_count++;
    myrtos_last_mcause = frame->mcause;
    myrtos_last_mepc = frame->mepc;

    if (frame->mcause & MCAUSE_INTERRUPT_BIT) {
        if ((frame->mcause & MCAUSE_CODE_MASK) == MCAUSE_MACHINE_TIMER) {
            myrtos_ticks++;
// The interrupt stays pending until mtimecmp moves forward. Without
// this it recurs immediately and the machine does nothing else.
            myrtos_timer_rearm();
// Time slicing: on every tick the next runnable process takes over.
            return myrtos_switch(sp);
        }
        return sp;
    }

    if ((frame->mcause & MCAUSE_CODE_MASK) == MCAUSE_ECALL_M) {
// mepc points at the ecall instruction itself. Without this step
// mret returns to the same instruction and the machine loops.
        frame->mepc += 4;

        switch (frame->a7) {
        case SYS_NULL:
            frame->a0 = 0;
            break;
        case SYS_IO_PUTC:
            myrtos_putc((char)frame->a0);
            frame->a0 = 0;
            break;
        case SYS_OPEN:
            frame->a0 = (uint32_t)myrtos_io_open((const char*)(uintptr_t)frame->a0,
                                                 myrtos_current_pid());
            break;
        case SYS_WRITE:
            frame->a0 = (uint32_t)myrtos_io_write((int32_t)frame->a0,
                                                  (const uint8_t*)(uintptr_t)frame->a1,
                                                  frame->a2,
                                                  myrtos_current_pid());
            break;
        case SYS_READ:
            frame->a0 = (uint32_t)myrtos_io_read((int32_t)frame->a0,
                                                 (uint8_t*)(uintptr_t)frame->a1,
                                                 frame->a2,
                                                 myrtos_current_pid());
            break;
        case SYS_EXEC: {
// OS-9's F$Link and F$Fork in one: look the module up, bump
// its link count, and make a process of it. The code is shared --
// only the data area is new.
            const char *want = (const char*)(uintptr_t)frame->a0;
            const char *stored = myrtos_moddir_match(want);
            if (!stored) { frame->a0 = (uint32_t)-1; break; }
            const myrtos_module_header_t *m = myrtos_moddir_link(stored);
// The arguments are passed at creation: they are copied into the
// process's own memory before the frame is built, so a0 can point past them.
            int32_t pid = m ? myrtos_process_create(m, (const char*)(uintptr_t)frame->a1) : -1;
            if (pid >= 0) myrtos_io_inherit(myrtos_current_pid(), pid);
            frame->a0 = (uint32_t)pid;
            break;
        }
        case SYS_ARGS:
            frame->a0 = myrtos_process_get_args((char*)(uintptr_t)frame->a0, frame->a1);
            break;
        case SYS_CLOSE:
            frame->a0 = (uint32_t)myrtos_io_close((int32_t)frame->a0, myrtos_current_pid());
            break;
        case SYS_MODDIR: {
            const myrtos_module_entry_t *e = myrtos_moddir_entry(frame->a0);
            if (!e) { frame->a0 = (uint32_t)-1; break; }
            char *out = (char*)(uintptr_t)frame->a1;
            for (int i = 0; i < 12; i++) out[i] = e->name[i];
            frame->a0 = e->links;
            break;
        }
        case SYS_MEMINFO:
            frame->a0 = (frame->a0 == MYRTOS_MEM_PROCESSES)
                ? myrtos_process_count()
                : (uint32_t)myrtos_tlsf_largest_free(myrtos_mem_pool);
            break;
        case SYS_EXIT:
// The process is not to be resumed, so we switch away at once.
            myrtos_process_exit();
            return myrtos_switch(sp);
        default:
            myrtos_print("*** MYRTOS: unknown system call ***\n");
            frame->a0 = (uint32_t)-1;
            break;
        }
        return sp;
    }

    myrtos_print("\n*** MYRTOS TRAP: unhandled exception ***\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

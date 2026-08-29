// The system call layer. In OS-9 everything went through SWI2; here it is
// ecall, and the trap vector in scheduler.S is the way in.

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "fat32.h"
#include "moddir.h"
#include "tlsf.h"

void myrtos_print(const char *s);
void myrtos_putc(char c);
void myrtos_print_u32(uint32_t v);
int32_t myrtos_current_pid(void);
uint32_t myrtos_process_count(void);
int32_t myrtos_process_create(const myrtos_module_header_t *m, const char *args);
uint32_t myrtos_process_get_args(char *buf, uint32_t len);
void myrtos_block_on_read(int32_t path);
void myrtos_block_on_write(int32_t path);
bool myrtos_block_on_child(int32_t pid);
void myrtos_wake_readers(void);
void myrtos_sleep_begin(uint32_t ticks);
uint32_t myrtos_set_priority(uint32_t prio);
int32_t myrtos_process_info(uint32_t slot, myrtos_psinfo_t *out);
void myrtos_reboot_bootsel(void);
void *myrtos_mem_alloc(uint32_t size);
int32_t myrtos_mem_free(void *ptr);
void *myrtos_mem_realloc(void *ptr, uint32_t size);
void myrtos_sleep_tick(void);
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
            // A blocked reader is woken here rather than by the driver: TinyUSB
            // delivers into its own buffers, and asking once per tick is both
            // simpler and enough at keyboard speed.
            myrtos_wake_readers();
            myrtos_sleep_tick();
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
        case SYS_WRITE: {
            int32_t wpath = (int32_t)frame->a0;
            int32_t wn = myrtos_io_write(wpath, (const uint8_t*)(uintptr_t)frame->a1,
                                         frame->a2, myrtos_current_pid());
            if (wn == 0 && frame->a2 && myrtos_current_pid() != 0) {
                // No room. Same shape as a blocking read: step back onto the
                // ecall and wait, so the call is simply made again with its
                // arguments intact once the device can take something.
                frame->mepc -= 4;
                myrtos_block_on_write(wpath);
                return myrtos_switch(sp);
            }
            frame->a0 = (uint32_t)wn;
            break;
        }
        case SYS_READ: {
            int32_t path = (int32_t)frame->a0;
            int32_t n = myrtos_io_read(path, (uint8_t*)(uintptr_t)frame->a1,
                                       frame->a2, myrtos_current_pid());
            if (n == 0 && myrtos_current_pid() != 0) {
                // Nothing there. Step mepc back onto the ecall and block: when
                // the process runs again it re-executes the call with its
                // arguments still in place, so nothing has to be remembered
                // about a half-finished read.
                frame->mepc -= 4;
                myrtos_block_on_read(path);
                return myrtos_switch(sp);
            }
            frame->a0 = (uint32_t)n;
            break;
        }
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
        case SYS_FSDIR:
            frame->a0 = (uint32_t)myrtos_fat_stat_nth(frame->a0,
                                                      (char*)(uintptr_t)frame->a1,
                                                      (uint32_t*)(uintptr_t)frame->a2);
            break;
        case SYS_FSREAD: {
            // The name arrives as the user typed it; padding it into 8.3 form is
            // the filesystem's job, not every utility's.
            const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)(uintptr_t)frame->a0;
            char name_83[12];
            if (!myrtos_fat_name_to_83(r->name, name_83)) { frame->a0 = (uint32_t)-1; break; }
            frame->a0 = (uint32_t)myrtos_fat_read_at(name_83, r->offset, r->buf, r->len);
            break;
        }
        case SYS_FSWRITE: {
            const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)(uintptr_t)frame->a0;
            char name_83[12];
            if (!myrtos_fat_name_to_83(r->name, name_83)) { frame->a0 = (uint32_t)-1; break; }
            frame->a0 = (uint32_t)myrtos_fat_write_at(name_83, r->offset, r->buf, r->len);
            break;
        }
        case SYS_FSREMOVE: {
            char name_83[12];
            if (!myrtos_fat_name_to_83((const char*)(uintptr_t)frame->a0, name_83)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            frame->a0 = myrtos_fat_remove(name_83) ? 0u : (uint32_t)-1;
            break;
        }
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
        case SYS_ALLOC:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_mem_alloc(frame->a0);
            break;
        case SYS_FREE:
            frame->a0 = (uint32_t)myrtos_mem_free((void*)(uintptr_t)frame->a0);
            break;
        case SYS_REALLOC:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_mem_realloc(
                            (void*)(uintptr_t)frame->a0, frame->a1);
            break;
        case SYS_BOOTSEL:
            myrtos_reboot_bootsel();    // does not return
            break;
        case SYS_PSINFO:
            frame->a0 = (uint32_t)myrtos_process_info(frame->a0,
                            (myrtos_psinfo_t*)(uintptr_t)frame->a1);
            break;
        case SYS_TICKS:
            frame->a0 = (uint32_t)myrtos_ticks;
            break;
        case SYS_SETPRIO:
            frame->a0 = myrtos_set_priority(frame->a0);
            break;
        case SYS_SLEEP: {
            uint32_t ms = frame->a0;                // read before a0 is the result
            frame->a0 = 0;
            if (myrtos_current_pid() == 0) break;   // the kernel does not sleep
            if (ms == 0) return myrtos_switch(sp);  // zero is a yield
            myrtos_sleep_begin(ms);
            return myrtos_switch(sp);
        }
        case SYS_WAIT:
            if (myrtos_block_on_child((int32_t)frame->a0)) {
                frame->a0 = 0;
                return myrtos_switch(sp);
            }
            frame->a0 = 0;      // already gone; nothing to wait for
            break;
        case SYS_EXIT:
// The process is not to be resumed, so we switch away at once.
            myrtos_process_exit();
            return myrtos_switch(sp);
        default:
            // The number matters: without it the message says only that
            // something is wrong, which cost an hour when a stale kernel met a
            // module built against a newer one.
            myrtos_print("*** MYRTOS: unknown system call ");
            myrtos_print_u32(frame->a7);
            myrtos_print(" ***\n");
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

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tlsf.h"
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "moddir.h"

#define MAX_PROCESSES 8
#define KERNEL_PID    0     // The kernel is itself a process, always runnable.

// A process that is waiting is not runnable. Until this existed, waiting meant
// asking again as fast as the scheduler would let you, which spends the whole
// machine on a process that has nothing to do.
typedef enum {
    PROC_STATE_FREE,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_WAIT_READ,       // waiting for a device to have something
    PROC_STATE_WAIT_CHILD       // waiting for another process to exit
} proc_state_t;

typedef struct {
    uint32_t pid;
    proc_state_t state;
    uintptr_t entry_point;
    const myrtos_module_header_t *module;   // shared code, one copy for all
    void* mem_base;           // bottom of the data area, private per process
    uint32_t mem_size;        // data + stack, as the module header asked for
    uint32_t saved_sp;        // the trap frame, hence the entire context
    const char *args;         // points into the process's OWN memory, not here
    int32_t  wait_path;       // WAIT_READ: the path being waited on
    int32_t  wait_pid;        // WAIT_CHILD: the process being waited for
} pcb_t;

static pcb_t process_table[MAX_PROCESSES];
static int32_t current_pid = KERNEL_PID;

// The kernel's global and thread pointers. crt0 sets gp to __global_pointer$,
// and the kernel's own C code addresses its small globals relative to it. A new
// process must inherit them: otherwise the trap vector restores the process's
// zeroed gp when it traps into the kernel, and the handler writes at random.
static uint32_t kernel_gp, kernel_tp;

extern tlsf_pool_t myrtos_mem_pool;
void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

#define SYS_EXIT 2u

// Where a process returns when its module_main is done. It cannot return to
// the kernel -- it has no such call chain -- so it asks to be terminated
// instead.
static void myrtos_process_return(void) {
    register uint32_t id __asm__("a7") = SYS_EXIT;
    __asm__ volatile("ecall" : : "r"(id) : "memory");
    for (;;) { __asm__ volatile("wfi"); }
}

void myrtos_scheduler_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid = i;
        process_table[i].state = PROC_STATE_FREE;
        process_table[i].mem_base = NULL;
    }
    // The kernel is pid 0. Its context is filled in at the first trap, since
    // it is already running on its own stack.
    process_table[KERNEL_PID].state = PROC_STATE_RUNNING;
    current_pid = KERNEL_PID;
    __asm__ volatile("mv %0, gp" : "=r"(kernel_gp));
    __asm__ volatile("mv %0, tp" : "=r"(kernel_tp));
    myrtos_print("Real-time process scheduler initialized.\n");
}

int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr,
                              const char *args) {
    int32_t slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {      // 0 is the kernel
        if (process_table[i].state == PROC_STATE_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        myrtos_print("Error: process table full.\n");
        return -1;
    }

    // One contiguous area: data at the bottom, stack downwards from the top.
    uint32_t bytes = module_ptr->mem_size;
    void *mem = myrtos_tlsf_malloc(myrtos_mem_pool, bytes);
    if (!mem) {
        myrtos_print("Error: failed to allocate process memory.\n");
        return -1;
    }

    // The command line at the bottom of the process's own area, followed by an
    // argv vector. Both are released with the rest when the process dies, so
    // neither a separate allocation nor a separate free is needed -- and the
    // only limit is mem_size.
    //
    // The layout is the Unix one: the string is split in place with NULs, and
    // the pointers are placed after it. The module gets argc in a0 and argv in
    // a1, which is exactly what main(int, char**) expects.
    char *dst = (char*)mem;
    uint32_t n = 0;
    if (args) while (args[n] && n < bytes / 2) { dst[n] = args[n]; n++; }
    dst[n] = 0;

    char **argv = (char**)(((uintptr_t)mem + n + 1 + 3) & ~(uintptr_t)3);
    int argc = 0;

    // argv[0] is the module's own name, as in every system since Unix.
    argv[argc++] = (char*)((uintptr_t)module_ptr + module_ptr->name_offset);

    // The split understands quotes: a quoted run is ONE argument, and the
    // quote characters themselves disappear. Since characters are only ever
    // removed, never added, the result can be compacted into the same buffer --
    // the write pointer always stays behind the read pointer.
    char *p = dst;      // reads
    char *w = dst;      // skriver
    while (*p && argc < 16) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = w;
        while (*p && *p != ' ') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) *w++ = *p++;
                if (*p) p++;          // skip the closing quote
            } else {
                *w++ = *p++;
            }
        }
        // The delimiter must be consumed BEFORE the word is terminated. With
        // no quotes the pointers move in step, and the NUL would otherwise
        // land on the very space the read pointer is on -- the splitter then
        // saw end of string and dropped everything after the first word.
        while (*p == ' ') p++;
        *w++ = 0;
    }
    argv[argc] = 0;

    uintptr_t data_base = ((uintptr_t)&argv[argc + 1] + 3) & ~(uintptr_t)3;
    (void)data_base;   // reserved for the module's own data area

    uintptr_t stack_top = ((uintptr_t)mem + bytes) & ~(uintptr_t)15;
    myrtos_frame_t *frame = (myrtos_frame_t*)(stack_top - sizeof(myrtos_frame_t));
    for (uint32_t i = 0; i < sizeof(myrtos_frame_t) / 4; i++) {
        ((uint32_t*)frame)[i] = 0;
    }
    // When the scheduler picks the process, the vector restores these values
    // and mret jumps to mepc. That is how a process starts: as though it had
    // just been interrupted immediately before its first instruction.
    frame->mepc = (uint32_t)((uintptr_t)module_ptr + module_ptr->exec_offset);
    frame->ra   = (uint32_t)(uintptr_t)myrtos_process_return;
    frame->a0   = (uint32_t)argc;             // main(int argc, ...)
    frame->a1   = (uint32_t)(uintptr_t)argv;  //          ..., char **argv)
    frame->gp   = kernel_gp;
    frame->tp   = kernel_tp;

    process_table[slot].entry_point = frame->mepc;
    process_table[slot].module = module_ptr;
    process_table[slot].mem_base = mem;
    process_table[slot].mem_size = bytes;
    process_table[slot].saved_sp = (uint32_t)(uintptr_t)frame;
    process_table[slot].args = (const char*)mem;
    process_table[slot].state = PROC_STATE_READY;

    // The addresses are the whole point: if two processes run the same module,
    // the code should be at the same place and the data areas at different ones.
    myrtos_print("  pid ");
    myrtos_print_u32(slot);
    myrtos_print(": code at 0x");
    myrtos_print_hex((uint32_t)(uintptr_t)module_ptr);
    myrtos_print(", data at 0x");
    myrtos_print_hex((uint32_t)(uintptr_t)mem);
    myrtos_print("\n");
    return slot;
}

// Called from the trap handler. Saves the interrupted process's stack and
// returns the one to take over -- round robin over everything runnable.
uint32_t myrtos_switch(uint32_t current_sp) {
    process_table[current_pid].saved_sp = current_sp;
    if (process_table[current_pid].state == PROC_STATE_RUNNING) {
        process_table[current_pid].state = PROC_STATE_READY;
    }

    // The kernel is the fallback, not the current process: if the current one
    // has just blocked, resuming it is exactly what must not happen.
    int32_t next = KERNEL_PID;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
        int32_t cand = (current_pid + i) % MAX_PROCESSES;
        if (process_table[cand].state == PROC_STATE_READY) { next = cand; break; }
    }

    current_pid = next;
    process_table[next].state = PROC_STATE_RUNNING;
    return process_table[next].saved_sp;
}

int32_t myrtos_current_pid(void) { return current_pid; }

uint32_t myrtos_process_get_args(char *buf, uint32_t len) {
    const char *src = process_table[current_pid].args;
    if (!src || !len) return 0;
    uint32_t i = 0;
    while (src[i] && i < len - 1) { buf[i] = src[i]; i++; }
    if (len) buf[i] = 0;
    return i;
}

uint32_t myrtos_process_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].state != PROC_STATE_FREE) n++;
    return n;
}

// A process has asked to die. Its memory goes back and its slot is freed.
// Block the running process. It is not made ready again here -- something else
// has to notice that what it waits for has happened.
void myrtos_block_on_read(int32_t path) {
    process_table[current_pid].state = PROC_STATE_WAIT_READ;
    process_table[current_pid].wait_path = path;
}

// Wait for a process to exit. False means there is nothing to wait for, either
// because the pid is out of range or because it has already finished -- the
// caller then carries on rather than blocking forever.
bool myrtos_block_on_child(int32_t pid) {
    if (pid <= 0 || pid >= MAX_PROCESSES) return false;
    if (process_table[pid].state == PROC_STATE_FREE) return false;
    process_table[current_pid].state = PROC_STATE_WAIT_CHILD;
    process_table[current_pid].wait_pid = pid;
    return true;
}

// Called from the timer tick. A device driver knows whether it has anything
// waiting; asking it once per millisecond costs the kernel a few comparisons and
// costs the blocked process nothing at all.
void myrtos_wake_readers(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_STATE_WAIT_READ) continue;
        if (!myrtos_io_readable(process_table[i].wait_path, i)) continue;
        process_table[i].state = PROC_STATE_READY;
    }
}

void myrtos_process_exit(void) {
    if (current_pid == KERNEL_PID) return;      // the kernel is never terminated
    myrtos_print("Process ");
    myrtos_print_u32(current_pid);
    myrtos_print(" exited.\n");
    myrtos_io_close_all(current_pid);
    myrtos_moddir_unlink(process_table[current_pid].module);
    myrtos_tlsf_free(myrtos_mem_pool, process_table[current_pid].mem_base);
    process_table[current_pid].state = PROC_STATE_FREE;
    process_table[current_pid].mem_base = NULL;

    // Whoever was waiting for this one can run again. Exact, unlike the read
    // wake-up: the event is this line, and nothing has to be polled to see it.
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_STATE_WAIT_CHILD) continue;
        if (process_table[i].wait_pid != current_pid) continue;
        process_table[i].state = PROC_STATE_READY;
    }
}

// --- THE MACHINE TIMER ----------------------------------------------------
// Hazard3 has a standard RISC-V machine timer in SIO. mtime counts from the
// tick generator that runtime_init sets to one pulse per microsecond, and an
// interrupt fires when mtime reaches mtimecmp.

#define SIO_BASE_ADDR   0xd0000000u
#define MTIME_CTRL      (*(volatile uint32_t*)(SIO_BASE_ADDR + 0x1a4))
#define MTIME_LO        (*(volatile uint32_t*)(SIO_BASE_ADDR + 0x1b0))
#define MTIME_HI        (*(volatile uint32_t*)(SIO_BASE_ADDR + 0x1b4))
#define MTIMECMP_LO     (*(volatile uint32_t*)(SIO_BASE_ADDR + 0x1b8))
#define MTIMECMP_HI     (*(volatile uint32_t*)(SIO_BASE_ADDR + 0x1bc))

#define MSTATUS_MIE     (1u << 3)
#define MIE_MTIE        (1u << 7)

static uint32_t timer_interval;

static uint64_t mtime_read(void) {
    uint32_t hi, lo, hi2;
    do { hi = MTIME_HI; lo = MTIME_LO; hi2 = MTIME_HI; } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static void mtimecmp_write(uint64_t value) {
    MTIMECMP_HI = 0xffffffffu;
    MTIMECMP_LO = (uint32_t)value;
    MTIMECMP_HI = (uint32_t)(value >> 32);
}

void myrtos_timer_rearm(void) {
    mtimecmp_write(mtime_read() + timer_interval);
}

void myrtos_timer_init(uint32_t interval_ticks) {
    timer_interval = interval_ticks;
    MTIME_CTRL |= 1u;
    myrtos_timer_rearm();
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MTIE));
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
}

uint64_t myrtos_timer_now(void) { return mtime_read(); }

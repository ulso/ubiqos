#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tlsf.h"
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "moddir.h"

#define MAX_PROCESSES MYRTOS_MAX_PROCESSES
#define KERNEL_PID    0     // The kernel is itself a process, always runnable.

// A process that is waiting is not runnable. Until this existed, waiting meant
// asking again as fast as the scheduler would let you, which spends the whole
// machine on a process that has nothing to do.
typedef enum {
    PROC_STATE_FREE,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_WAIT_READ,       // waiting for a device to have something
    PROC_STATE_WAIT_WRITE,      // waiting for room to write
    PROC_STATE_WAIT_CHILD,      // waiting for another process to exit
    PROC_STATE_SLEEPING,        // waiting for a length of time
    PROC_STATE_WAIT_RECV,       // a server with nothing to serve
    PROC_STATE_WAIT_REPLY,      // a sender whose message has not been answered

    // Killed, but a server still holds a pointer into its memory. It is gone as
    // far as scheduling and waiting are concerned; what is left is the block,
    // and that goes when the reply comes. See myrtos_process_kill.
    PROC_STATE_ZOMBIE
} proc_state_t;

typedef struct {
    uint32_t pid;
    proc_state_t state;
    uintptr_t entry_point;
    const myrtos_module_header_t *module;   // shared code, one copy for all
    void* mem_base;           // bottom of the allocation, private per process
    void* code_base;          // a relocated copy of the module, if it needed one
    void* data_base;          // where the module's own state may start
    uint32_t data_size;       // how far it reaches before the stack comes down
    uint32_t mem_size;        // data + stack, as the module header asked for
    uint32_t saved_sp;        // the trap frame, hence the entire context
    const char *args;         // points into the process's OWN memory, not here
    int32_t  wait_path;       // WAIT_READ: the path being waited on
    int32_t  wait_pid;        // WAIT_CHILD: the process being waited for
    int32_t  sleep_delta;     // SLEEPING: ticks after the process ahead of it
    int32_t  sleep_next;      // SLEEPING: next in the delta list, -1 at the end
    bool     recv_timed;      // WAIT_RECV: also on the sleep list, and will time out
    uint32_t priority;        // 0 is the idle process, 31 the most urgent
    int32_t  next_ready;      // READY: next in this priority's queue, -1 at the end
    struct alloc_hdr *allocs; // everything this process has been given

    // Messages. A rendezvous queues senders, not messages: the message stays in
    // the sender's memory, which cannot change because the sender is stopped.
    // So this is one more list through the process table, like the two above.
    int32_t  msg_next;        // WAIT_REPLY: next sender queued on the same server
    int32_t  msg_head;        // senders waiting for ME, -1 when none
    int32_t  msg_tail;
    int32_t  msg_serving;     // the most recent sender received, -1 when none
    int32_t  msg_dest;        // WAIT_REPLY: the server this sender is waiting on
    myrtos_msg_t msg;         // WAIT_REPLY: what this sender is offering
    myrtos_msg_t *msg_out;    // WAIT_RECV: where the message is to be delivered

    // Ctrl-C, for a process that asked to hear about it rather than be ended by
    // it. Zero means it did not ask and the key kills, which is what every
    // command here wants. See myrtos_intr_request.
    uint32_t intr_pulse;      // the pulse type to send, 0 for none
    int32_t  intr_deadline;   // tick to kill at once asked, -1 when not asked

    // Where relative paths start. Kept as text rather than a cluster number so
    // that it can be shown, and so a directory removed underneath a process
    // fails at the next lookup instead of silently becoming somewhere else.
    char cwd[64];
} pcb_t;

// --- MEMORY HANDED TO PROCESSES -------------------------------------------
// Every allocation carries a header naming its owner and linking it into that
// process's list, so death returns everything rather than leaking it.
//
// The owner is a field rather than an implication because it will have to move.
// Message passing in the manner of OSE hands a buffer to another process
// without copying it: the sender loses its pointer and the receiver gains one.
// That is an unlink, a relink and a store to this field -- but only if the
// ownership was written down in the first place.
typedef struct alloc_hdr {
    struct alloc_hdr *next;   // next block owned by the same process
    uint32_t owner;           // pid; moves when a block is handed on
    uint32_t size;            // what the caller asked for
    uint32_t magic;           // a free() of something else should not be silent
} alloc_hdr_t;

#define ALLOC_MAGIC 0x4d454d21u   // "MEM!"

tlsf_pool_t myrtos_pool_for(const myrtos_module_header_t *m);
tlsf_pool_t myrtos_pool_of_address(void *p);

// The second pool. SRAM holds what has timing constraints -- module code runs
// from there, and so do stacks -- while PSRAM takes what is merely large.
// Eight megabytes against seventy kilobytes of headroom is not a close call for
// a framebuffer, but it sits on QSPI behind the XIP cache, so what goes there
// must not care when it arrives.
tlsf_pool_t myrtos_bulk_pool;

static pcb_t process_table[MAX_PROCESSES];
static int32_t current_pid = KERNEL_PID;

// --- THE READY QUEUES -----------------------------------------------------
// One queue per priority, and a bitmap saying which of them are not empty.
// Choosing what runs next is then finding the highest set bit, which on this
// core is a single clz instruction -- the cost does not grow with the number of
// runnable processes the way scanning the process table did.
//
// Round robin survives inside a level: a process that used up its quantum goes
// to the back of its own queue, so equals share. Nothing shares across levels,
// which is the point of a priority scheduler and also its sharp edge -- a busy
// process starves everything below it for as long as it runs.
#define MYRTOS_PRIO_LEVELS  32
#define MYRTOS_PRIO_IDLE    0
#define MYRTOS_PRIO_DEFAULT 16

static int32_t  ready_head[MYRTOS_PRIO_LEVELS];
static int32_t  ready_tail[MYRTOS_PRIO_LEVELS];
static uint32_t ready_bitmap;

static void ready_enqueue(int32_t pid) {
    uint32_t p = process_table[pid].priority;
    process_table[pid].next_ready = -1;
    if (ready_head[p] < 0) ready_head[p] = pid;
    else process_table[ready_tail[p]].next_ready = pid;
    ready_tail[p] = pid;
    ready_bitmap |= (1u << p);
}

// Out of the queue it is standing in. Only killing needs this -- a process that
// blocks is simply not put back, which is why nothing else ever had to remove
// one. Leaving a dead process linked would hand the processor to a free slot.
static void ready_remove(int32_t pid) {
    uint32_t p = process_table[pid].priority;
    int32_t cur = ready_head[p], prev = -1;
    while (cur >= 0) {
        if (cur == pid) {
            int32_t next = process_table[cur].next_ready;
            if (prev < 0) ready_head[p] = next;
            else          process_table[prev].next_ready = next;
            if (ready_tail[p] == pid) ready_tail[p] = prev;
            if (ready_head[p] < 0) ready_bitmap &= ~(1u << p);
            process_table[pid].next_ready = -1;
            return;
        }
        prev = cur;
        cur = process_table[cur].next_ready;
    }
}

// The highest priority with anyone in it. The idle process is always ready, so
// bit 0 is set whenever nothing else is and the bitmap is never zero here.
static int32_t ready_take_highest(void) {
    uint32_t p = 31u - (uint32_t)__builtin_clz(ready_bitmap);
    int32_t pid = ready_head[p];
    ready_head[p] = process_table[pid].next_ready;
    if (ready_head[p] < 0) ready_bitmap &= ~(1u << p);
    process_table[pid].next_ready = -1;
    return pid;
}

// --- THE SLEEP LIST -------------------------------------------------------
// A delta list, as in Comer's XINU. Each sleeper stores not when it wakes but
// how many ticks after the one ahead of it, so the timer decrements exactly one
// number per tick no matter how many processes are asleep. Storing absolute
// wake times instead would mean comparing every sleeper against the clock on
// every tick, and would need an answer for what happens when the clock wraps.
//
// The cost moves to insertion, which walks the list summing deltas -- but a
// process sleeps once and is ticked many times, so that is the right way round.
static int32_t sleep_head = -1;

// The kernel's global and thread pointers. crt0 sets gp to __global_pointer$,
// and the kernel's own C code addresses its small globals relative to it. A new
// process must inherit them: otherwise the trap vector restores the process's
// zeroed gp when it traps into the kernel, and the handler writes at random.
// Not static: the machine's own header lays out a starting frame and needs it.
// gp addresses the kernel's small data on RISC-V, so a process without it
// cannot call into the kernel at all.
uint32_t myrtos_kernel_gp;
static uint32_t kernel_tp;

extern tlsf_pool_t myrtos_mem_pool;
void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

#define SYS_EXIT 2u

// Where a process returns when its module_main is done. It cannot return to
// the kernel -- it has no such call chain -- so it asks to be terminated
// instead.
static void myrtos_process_return(void) {
    // The same call any module makes, through the same stub -- which knows
    // which instruction its machine traps with, so this does not have to.
    myrtos_syscall(SYS_EXIT, 0, 0, 0);
    for (;;) { __asm__ volatile("wfi"); }
}

void myrtos_scheduler_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid = i;
        process_table[i].state = PROC_STATE_FREE;
        process_table[i].mem_base = NULL;
        process_table[i].code_base = NULL;
        process_table[i].sleep_next = -1;
        process_table[i].recv_timed = false;
        process_table[i].next_ready = -1;
        process_table[i].allocs = NULL;
        process_table[i].priority = MYRTOS_PRIO_DEFAULT;
    }
    sleep_head = -1;
    for (int p = 0; p < MYRTOS_PRIO_LEVELS; p++) { ready_head[p] = -1; ready_tail[p] = -1; }
    ready_bitmap = 0;
    // The kernel is pid 0. Its context is filled in at the first trap, since
    // it is already running on its own stack.
    // The idle process sits alone at the bottom. It never blocks, so once it is
    // running or queued the bitmap is never empty and picking the next process
    // needs no special case for "nobody is ready".
    process_table[KERNEL_PID].priority = MYRTOS_PRIO_IDLE;
    process_table[KERNEL_PID].state = PROC_STATE_RUNNING;
    current_pid = KERNEL_PID;
    // What the kernel is running with, so that a process can be given the same.
    // Neither register exists on ARM: the kernel is reached through svc rather
    // than through gp, and a kernel thread has no thread-local block to point
    // r9 at -- so both stay zero there, which is what a process with no data
    // area of its own should see.
#ifdef __riscv
    __asm__ volatile("mv %0, gp" : "=r"(myrtos_kernel_gp));
    __asm__ volatile("mv %0, tp" : "=r"(kernel_tp));
#endif
    myrtos_print("Real-time process scheduler initialized.\n");
}

// A process that runs kernel code. It has no module and no arguments, only a
// stack and an entry point, but is otherwise ordinary: scheduled by priority,
// able to sleep, and preemptible.
//
// This exists because servicing USB from the idle process turned out to be
// untenable once priorities arrived. TinyUSB's received data only reaches its
// FIFO when tud_task runs, so anything busy above the idle process silenced the
// console in both directions -- not merely its output.
int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority) {
    int32_t slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_STATE_FREE) { slot = i; break; }
    }
    if (slot < 0) return -1;

    void *mem = myrtos_tlsf_malloc(myrtos_mem_pool, stack_bytes);
    if (!mem) return -1;

    uintptr_t stack_top = ((uintptr_t)mem + stack_bytes) & ~(uintptr_t)15;
    myrtos_frame_t *frame = (myrtos_frame_t*)(stack_top - sizeof(myrtos_frame_t));
    for (uint32_t i = 0; i < sizeof(myrtos_frame_t) / 4; i++) ((uint32_t*)frame)[i] = 0;
    // A kernel thread takes no arguments and has no data area of its own, so it
    // is given the kernel's own thread pointer.
    myrtos_frame_start(frame, (uintptr_t)entry,
                       (uintptr_t)myrtos_process_return, 0, 0, kernel_tp);

    process_table[slot].entry_point = frame->pc;
    process_table[slot].module   = NULL;      // nothing to unlink when it ends
    process_table[slot].mem_base = mem;
    process_table[slot].data_base = NULL;     // a kernel thread keeps its state
    process_table[slot].data_size = 0;        // in the kernel's own variables
    process_table[slot].mem_size = stack_bytes;
    process_table[slot].saved_sp = (uint32_t)(uintptr_t)frame;
    process_table[slot].msg_next = process_table[slot].msg_head = -1;
    process_table[slot].msg_tail = process_table[slot].msg_serving = -1;
    process_table[slot].msg_dest = -1;
    process_table[slot].msg_out = 0;
    process_table[slot].cwd[0] = '/';
    process_table[slot].cwd[1] = 0;
    process_table[slot].args     = NULL;
    process_table[slot].intr_pulse    = 0;
    process_table[slot].intr_deadline = -1;
    process_table[slot].sleep_next = -1;
    process_table[slot].recv_timed = false;
    process_table[slot].allocs = NULL;
    process_table[slot].priority = priority;
    process_table[slot].state    = PROC_STATE_READY;
    ready_enqueue(slot);
    return slot;
}

// Write the addresses a module could not know until it was loaded.
//
// The table says where and of what kind; make_module put both there because
// neither can be recovered from the module itself. A pointer in data holds no
// clue once it is an offset, and an instruction pair holds twenty bits in one
// place and twelve in another with no way back to the whole.
//
// Four kinds, and only the first exists in a module that links no library. The
// other three are what newlib and libgcc bring: they come prebuilt in the
// toolchain's default code model and reach globals absolutely with lui, and the
// very first one of those to be executed -- calloc reading _impure_ptr -- is
// what took the board down when this handled only the first kind.
//
// Byte at a time throughout, and not out of caution. A HI20 sits wherever the
// instruction stream put it, and compressed instructions mean that is often a
// two-byte boundary; Hazard3 traps on a word access to one rather than fixing
// it up.
#define RELOC_ABS32   0
#define RELOC_HI20    1
#define RELOC_LO12_I  2
#define RELOC_LO12_S  3

static uint32_t reloc_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void reloc_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void relocate_image(uint8_t *image, const myrtos_module_header_t *src)
{
    const uint8_t *table = (const uint8_t*)src + src->reloc_offset;
    uint32_t base = (uint32_t)(uintptr_t)image;

    for (uint32_t i = 0; i < src->reloc_count; i++) {
        uint32_t w0 = reloc_get32(table + i * 8);
        uint32_t v  = base + reloc_get32(table + i * 8 + 4);

        uint8_t *p = image + (w0 & 0x0fffffffu);
        switch (w0 >> 28) {
        case RELOC_ABS32:
            reloc_put32(p, v);
            break;

        // U-type: the top twenty bits, rounded so that the signed twelve-bit
        // half added afterwards lands on the intended address.
        case RELOC_HI20:
            reloc_put32(p, (reloc_get32(p) & 0x00000fffu)
                         | ((v + 0x800u) & 0xfffff000u));
            break;

        // I-type: imm[11:0] in the top twelve bits of the instruction.
        case RELOC_LO12_I:
            reloc_put32(p, (reloc_get32(p) & 0x000fffffu)
                         | ((v & 0xfffu) << 20));
            break;

        // S-type: the same twelve bits, split -- imm[11:5] high, imm[4:0] in
        // the middle, with the register fields untouched between them.
        case RELOC_LO12_S: {
            uint32_t lo = v & 0xfffu;
            reloc_put32(p, (reloc_get32(p) & ~0xfe000f80u)
                         | ((lo >> 5) << 25) | ((lo & 0x1fu) << 7));
            break;
        }
        default:
            break;
        }
    }
}

// A relocated copy of a module, ready to run. The allocation itself is handed
// back separately, because it is what has to be freed and it is not the address
// anything runs at.
//
// Factored out of myrtos_process_create for the library loader, which needs
// exactly this and no process: a library is code the kernel calls, so it is
// copied, zeroed and relocated the same way and then simply not started.
uint8_t *myrtos_module_relocated_copy(const myrtos_module_header_t *m, void **owned_out)
{
    uint32_t image = myrtos_module_image_size(m);
    uint32_t total = image + m->bss_size;

    // Sixteen bytes, and the allocator is not why.
    //
    // A module's sections carry their own alignment -- wasm's .sdata and .bss
    // both want eight -- and every one of them is measured from wherever the
    // copy lands. At the fixed address a single-instance module used to be
    // loaded at, half a megabyte aligned, that was free. From an allocator it is
    // not: an eight-byte load against a copy that landed four-past-eight is a
    // misaligned access, and Hazard3 does not fix those up, it traps.
    void *raw = myrtos_tlsf_malloc(myrtos_pool_for(m), total + 15);
    if (!raw) return 0;
    uint8_t *aligned = (uint8_t*)(((uintptr_t)raw + 15) & ~(uintptr_t)15);

    uint32_t *d32 = (uint32_t*)aligned;
    const uint32_t *s32 = (const uint32_t*)m;
    uint32_t words = image / 4;
    for (uint32_t i = 0; i < words; i++) d32[i] = s32[i];
    uint8_t *d8 = aligned;
    const uint8_t *s8 = (const uint8_t*)m;
    for (uint32_t i = words * 4; i < image; i++) d8[i] = s8[i];

    // What the file does not carry. .bss and .sbss have a size and no bytes,
    // and a module is entitled to find them zero.
    for (uint32_t i = image; i < total; i++) d8[i] = 0;

    relocate_image(aligned, m);
    if (owned_out) *owned_out = raw;
    return aligned;
}


int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr,
                              const char *args) {
    // A module without the re-entrant attribute has writable data that every
    // instance would share, so there may only be one. OS-9 said the same thing
    // with the same bit. A service that owns hardware, or a protocol stack with
    // tables of its own, is one of these by nature -- and forcing its globals
    // into per-process storage would be ceremony for a process there is one of.
    if (module_ptr && !((module_ptr->attr_rev >> 8) & MYRTOS_ATTR_REENTRANT)) {
        for (int i = 1; i < MAX_PROCESSES; i++) {
            if (process_table[i].state == PROC_STATE_FREE) continue;
            if (process_table[i].module != module_ptr) continue;
            myrtos_print("  refused: ");
            myrtos_print((const char*)((uintptr_t)module_ptr + module_ptr->name_offset));
            myrtos_print(" is not re-entrant and is already running\n");
            return -2;      // distinct from -1, so a shell can say which it was
        }
    }

    const myrtos_module_header_t *run = module_ptr;

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
    void *mem = myrtos_tlsf_malloc(myrtos_pool_for(module_ptr), bytes);
    if (!mem) {
        myrtos_print("Error: failed to allocate process memory.\n");
        return -1;
    }

    // A module is copied when it cannot run where it lies, and the module says
    // so itself: MYRTOS_ATTR_PRIVATE is set at build time for anything with
    // writable data, addresses to fix, or a .bss to zero. A module with none of
    // those runs straight out of flash and costs no memory at all, which is
    // most of them.
    //
    // A copy per process rather than one shared between them. It is the simple
    // thing and the modules are a few kilobytes; sharing one relocated copy
    // needs a reference count on something whose lifetime is not the process's,
    // and that is worth having only once the cost shows up somewhere.
    bool reentrant = ((module_ptr->attr_rev >> 8) & MYRTOS_ATTR_REENTRANT) != 0;
    bool needs_copy = ((module_ptr->attr_rev >> 8) & MYRTOS_ATTR_PRIVATE) != 0;
    void *code_copy = 0;
    if (!reentrant || needs_copy) {
        code_copy = 0;
        uint8_t *aligned = myrtos_module_relocated_copy(module_ptr, &code_copy);
        if (!aligned) {
            myrtos_print("Error: failed to allocate room to relocate a module.\n");
            myrtos_tlsf_free(myrtos_pool_for(module_ptr), mem);
            return -1;
        }

        // run is the aligned copy; code_base keeps the allocation itself, which
        // is what has to be given back.
        run = (const myrtos_module_header_t*)aligned;
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
    char *w = dst;      // writes
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

    // What is left after the command line and its vector belongs to the module.
    //
    // The thread-local block goes first, because the thread pointer points at it
    // and the linker assigned every variable an offset within it. The module's
    // initial values are copied in and the rest zeroed -- the same job OS-9's
    // loader did with a module's data section. Whatever follows is the area
    // myrtos_data_area hands out for raw use.
    //
    // MYRTOS_TLS_TCB_BYTES is what the machine's ABI reserves ahead of the
    // variables. RISC-V reserves nothing; ARM reserves eight for a thread
    // control block, and the linker resolves every local-exec reference to
    // eight plus the offset. The thread pointer is the base either way -- only
    // where the data sits inside the block differs.
    uintptr_t tls_base = ((uintptr_t)&argv[argc + 1] + 3) & ~(uintptr_t)3;
    uint8_t *tls_data = (uint8_t*)tls_base + MYRTOS_TLS_TCB_BYTES;

    for (uint32_t i = 0; i < MYRTOS_TLS_TCB_BYTES; i++) ((uint8_t*)tls_base)[i] = 0;

    const uint8_t *tls_src = module_ptr->tls_init
        ? (const uint8_t*)module_ptr + module_ptr->tls_offset : 0;
    for (uint32_t i = 0; i < module_ptr->tls_total; i++) {
        tls_data[i] = (tls_src && i < module_ptr->tls_init) ? tls_src[i] : 0;
    }

    uintptr_t data_base = (tls_base + MYRTOS_TLS_TCB_BYTES
                                    + module_ptr->tls_total + 3) & ~(uintptr_t)3;

    uintptr_t stack_top = ((uintptr_t)mem + bytes) & ~(uintptr_t)15;
    myrtos_frame_t *frame = (myrtos_frame_t*)(stack_top - sizeof(myrtos_frame_t));
    for (uint32_t i = 0; i < sizeof(myrtos_frame_t) / 4; i++) {
        ((uint32_t*)frame)[i] = 0;
    }
    // When the scheduler picks the process, the vector restores these values
    // and the return jumps to pc. That is how a process starts: as though it had
    // just been interrupted immediately before its first instruction.
    // The last argument is the thread pointer, and it carries the data area:
    // this process's own storage, which is thread-local storage with our layout
    // rather than the compiler's. The trap frame saves and restores it per
    // process, so a module reads its own state without a system call -- one
    // instruction on RISC-V, where tp is a register, and a two-instruction call
    // to __aeabi_read_tp on ARM, where r9 stands in for it. It is OS-9's U
    // register either way.
    //
    // It is the thread-local block and not the raw area: the offsets the linker
    // baked into the code all count from here.
    myrtos_frame_start(frame, (uintptr_t)run + run->exec_offset,
                       (uintptr_t)myrtos_process_return,
                       (uint32_t)argc,                     // main(int argc, ...)
                       (uint32_t)(uintptr_t)argv,          //     ..., char **argv)
                       (uint32_t)tls_base);

    process_table[slot].entry_point = frame->pc;
    process_table[slot].module = module_ptr;
    process_table[slot].mem_base = mem;
    process_table[slot].code_base = code_copy;
    // The stack comes down into the same span, so this is what is available
    // rather than what is safe. A module that wants a lot asks for a bigger
    // mem_size; nothing here can tell how deep its calls will go.
    process_table[slot].data_base = (void*)data_base;
    process_table[slot].data_size = (uint32_t)(stack_top - sizeof(myrtos_frame_t)
                                               - data_base);
    process_table[slot].mem_size = bytes;
    process_table[slot].saved_sp = (uint32_t)(uintptr_t)frame;
    process_table[slot].msg_next = process_table[slot].msg_head = -1;
    process_table[slot].msg_tail = process_table[slot].msg_serving = -1;
    process_table[slot].msg_dest = -1;
    process_table[slot].msg_out = 0;
    process_table[slot].cwd[0] = '/';
    process_table[slot].cwd[1] = 0;
    process_table[slot].args = (const char*)mem;
    process_table[slot].sleep_next = -1;
    process_table[slot].recv_timed = false;

    // Priority is inherited, as paths are: that is what lets `nice` work
    // without the started program knowing anything about priorities. The
    // exception is the kernel, which creates the first process from the idle
    // level -- inheriting that would leave the shell below everything.
    process_table[slot].allocs = NULL;
    // A slot is reused, so these are set rather than assumed. The first version
    // put them in myrtos_kernel_thread by mistake and left process_create alone,
    // and a fresh slot's zero deadline read as "already asked" -- so Ctrl-C
    // killed a process that had asked to be told about it, silently and every
    // time.
    process_table[slot].intr_pulse    = 0;
    process_table[slot].intr_deadline = -1;

    process_table[slot].priority = (current_pid == KERNEL_PID)
                                 ? MYRTOS_PRIO_DEFAULT
                                 : process_table[current_pid].priority;
    process_table[slot].state = PROC_STATE_READY;
    ready_enqueue(slot);

    // The addresses are the whole point: if two processes run the same module,
    // the code should be at the same place and the data areas at different ones.
    // Where the code and the data landed used to be printed here, and where a
    // process had gone was the only way to see anything at all while the loader
    // was being written. With a shell in front of it, it is two lines of noise
    // after every command; ps says where a process is, when anyone asks.
    return slot;
}

// Called from the trap handler. Saves the interrupted process's stack and
// returns the one to take over -- round robin over everything runnable.
uint32_t myrtos_switch(uint32_t current_sp) {
    process_table[current_pid].saved_sp = current_sp;
    if (process_table[current_pid].state == PROC_STATE_RUNNING) {
        process_table[current_pid].state = PROC_STATE_READY;
        ready_enqueue(current_pid);
    }

    // A process that blocked is not put back: its state is no longer RUNNING, so
    // it simply is not in any queue. That is why no special case is needed for
    // "do not resume the process that just went to sleep" -- and why the idle
    // process, which never blocks, is always there to fall back on.
    int32_t next = ready_take_highest();

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

// Report one slot. The state numbers are the ABI's, not the enum's: the enum is
// the kernel's business and free to change, and a module built against an older
// header should not start naming the wrong states if it does.
int32_t myrtos_process_info(uint32_t slot, myrtos_psinfo_t *out) {
    if (slot >= MAX_PROCESSES) return -1;
    const pcb_t *p = &process_table[slot];
    if (p->state == PROC_STATE_FREE) return -1;

    out->pid = p->pid;
    out->priority = p->priority;
    out->mem_size = p->mem_size;
    switch (p->state) {
        case PROC_STATE_READY:      out->state = MYRTOS_PS_READY; break;
        case PROC_STATE_RUNNING:    out->state = MYRTOS_PS_RUNNING; break;
        case PROC_STATE_WAIT_READ:  out->state = MYRTOS_PS_WAIT_READ; break;
        case PROC_STATE_WAIT_WRITE: out->state = MYRTOS_PS_WAIT_WRITE; break;
        case PROC_STATE_WAIT_CHILD: out->state = MYRTOS_PS_WAIT_CHILD; break;
        case PROC_STATE_SLEEPING:   out->state = MYRTOS_PS_SLEEPING; break;
        case PROC_STATE_WAIT_RECV:  out->state = MYRTOS_PS_WAIT_RECV; break;
        case PROC_STATE_WAIT_REPLY: out->state = MYRTOS_PS_WAIT_REPLY; break;
        case PROC_STATE_ZOMBIE:     out->state = MYRTOS_PS_ZOMBIE; break;
        default:                    out->state = MYRTOS_PS_FREE; break;
    }

    const char *name = "(kernel)";      // a kernel thread has no module
    if (p->module) name = (const char*)((uintptr_t)p->module + p->module->name_offset);
    int i = 0;
    while (i < 11 && name[i]) { out->name[i] = name[i]; i++; }
    while (i < 12) out->name[i++] = 0;
    return 0;
}

uint32_t myrtos_process_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].state != PROC_STATE_FREE) n++;
    return n;
}

// A process has asked to die. Its memory goes back and its slot is freed.
// Insert into the delta list. `remaining` is what is left of the requested time
// after subtracting everyone this process will wake up behind; that difference
// is the delta stored, and the process it lands in front of has its own delta
// reduced by the same amount so the chain still adds up.
static void sleep_insert(int32_t pid, uint32_t ticks) {
    int32_t prev = -1, cur = sleep_head;
    uint32_t remaining = ticks;

    while (cur >= 0 && (uint32_t)process_table[cur].sleep_delta <= remaining) {
        remaining -= (uint32_t)process_table[cur].sleep_delta;
        prev = cur;
        cur = process_table[cur].sleep_next;
    }

    process_table[pid].sleep_delta = (int32_t)remaining;
    process_table[pid].sleep_next = cur;
    if (prev < 0) sleep_head = pid;
    else process_table[prev].sleep_next = pid;
    if (cur >= 0) process_table[cur].sleep_delta -= (int32_t)remaining;
}

// Taking one out has to give its delta to the one behind it, or everything
// after it wakes early. A process that dies while asleep goes through here.
static void sleep_remove(int32_t pid) {
    int32_t prev = -1, cur = sleep_head;
    while (cur >= 0 && cur != pid) { prev = cur; cur = process_table[cur].sleep_next; }
    if (cur < 0) return;

    int32_t next = process_table[cur].sleep_next;
    if (next >= 0) process_table[next].sleep_delta += process_table[cur].sleep_delta;
    if (prev < 0) sleep_head = next;
    else process_table[prev].sleep_next = next;
    process_table[cur].sleep_next = -1;
}

// A process changes its own urgency. Returns what it was, so a utility can put
// it back. Nothing is requeued: the caller is running, hence in no queue, and it
// is enqueued at its new priority the next time it gives up the processor.
uint32_t myrtos_set_priority(uint32_t prio) {
    if (current_pid == KERNEL_PID) return MYRTOS_PRIO_IDLE;   // idle stays idle
    uint32_t was = process_table[current_pid].priority;

    // Zero belongs to the idle process and cannot be taken, which makes it a
    // free sentinel for asking without changing. It also closes the hole where
    // a process could demote itself to the idle level and compete with the loop
    // that keeps USB alive.
    if (prio == MYRTOS_PRIO_IDLE) return was;

    if (prio >= MYRTOS_PRIO_LEVELS) prio = MYRTOS_PRIO_LEVELS - 1;
    process_table[current_pid].priority = prio;
    return was;
}

void myrtos_sleep_begin(uint32_t ticks) {
    process_table[current_pid].state = PROC_STATE_SLEEPING;
    sleep_insert(current_pid, ticks);
}

// One decrement per tick, however many are asleep. Several can come due at
// once: a zero delta means "at the same moment as the one ahead of me".
void myrtos_sleep_tick(void) {
    if (sleep_head < 0) return;
    if (process_table[sleep_head].sleep_delta > 0) process_table[sleep_head].sleep_delta--;

    while (sleep_head >= 0 && process_table[sleep_head].sleep_delta == 0) {
        int32_t pid = sleep_head;
        sleep_head = process_table[pid].sleep_next;
        process_table[pid].sleep_next = -1;
        // A timed receive waits on both lists at once, and this is the losing
        // side of that race: nothing came, so the deadline answers instead. The
        // frame is written the same way a sender would have written it, because
        // as far as the process is concerned receive is simply returning.
        if (process_table[pid].recv_timed) {
            process_table[pid].recv_timed = false;
            process_table[pid].msg_out = 0;
            ((myrtos_frame_t*)(uintptr_t)process_table[pid].saved_sp)->a0 =
                (uint32_t)MYRTOS_RECV_TIMEOUT;
        }
        process_table[pid].state = PROC_STATE_READY;
        ready_enqueue(pid);
    }
}

// --- ALLOCATION ON BEHALF OF A PROCESS ------------------------------------

static void alloc_link(int32_t pid, alloc_hdr_t *h) {
    h->owner = (uint32_t)pid;
    h->next = process_table[pid].allocs;
    process_table[pid].allocs = h;
}

static bool alloc_unlink(int32_t pid, alloc_hdr_t *h) {
    alloc_hdr_t **pp = &process_table[pid].allocs;
    while (*pp) {
        if (*pp == h) { *pp = h->next; h->next = NULL; return true; }
        pp = &(*pp)->next;
    }
    return false;
}

static void *alloc_from(tlsf_pool_t pool, uint32_t size) {
    if (!size || !pool || current_pid == KERNEL_PID) return NULL;
    alloc_hdr_t *h = myrtos_tlsf_malloc(pool, size + sizeof(alloc_hdr_t));
    if (!h) return NULL;
    h->size = size;
    h->magic = ALLOC_MAGIC;
    alloc_link(current_pid, h);
    return (void*)(h + 1);
}

void *myrtos_mem_alloc(uint32_t size) {
    return alloc_from(myrtos_mem_pool, size);
}

// Deliberately a separate call rather than a flag. The choice is not about how
// much memory is wanted but about what it is for: this says "large, and I do
// not mind waiting". Falls back to SRAM when there is no PSRAM, so a module
// asking for it still works on a board without any.
void *myrtos_mem_alloc_bulk(uint32_t size) {
    void *p = alloc_from(myrtos_bulk_pool, size);
    return p ? p : alloc_from(myrtos_mem_pool, size);
}

// Where a module's memory comes from. A real-time module keeps everything in
// SRAM: its stack is touched by every call, and its code -- when it was copied
// from the card rather than left in flash -- executes from wherever it landed.
// Anything else can live in PSRAM, which is plentiful and slow.
tlsf_pool_t myrtos_pool_for(const myrtos_module_header_t *m) {
    bool rt = m && ((m->attr_rev >> 8) & MYRTOS_ATTR_REALTIME);
    return (rt || !myrtos_bulk_pool) ? myrtos_mem_pool : myrtos_bulk_pool;
}

// Which pool a block came from is decided by where it is, so the header does
// not have to carry it -- but by ASKING the pool, not by comparing against the
// memory map.
//
// This read "address >= MYRTOS_PSRAM_BASE" and was wrong for every block it
// ever saw. PSRAM is at 0x11000000 and SRAM at 0x20000000, so every SRAM
// address is above the PSRAM base: each free of SRAM put the block into the
// PSRAM pool's free lists. SRAM never came back, and the bulk pool was left
// holding addresses that are not in it -- which would eventually have been
// handed out.
//
// It hid because of the order at startup. The kernel's own heap self-test runs
// before myrtos_bulk_pool_init, so myrtos_bulk_pool is still null there, the
// guard falls through to the right pool, and the test reports "fully
// reclaimed". Everything after that boot moment was wrong.
tlsf_pool_t myrtos_pool_of_address(void *p) {
    if (myrtos_bulk_pool && myrtos_tlsf_owns(myrtos_bulk_pool, p)) return myrtos_bulk_pool;
    return myrtos_mem_pool;
}
static tlsf_pool_t pool_of(void *p) { return myrtos_pool_of_address(p); }

// Refuses anything this process does not own. Without the check a module could
// free the kernel's own module copies by passing any pointer it liked.
int32_t myrtos_mem_free(void *ptr) {
    if (!ptr) return 0;
    alloc_hdr_t *h = ((alloc_hdr_t*)ptr) - 1;
    if (h->magic != ALLOC_MAGIC) return -1;
    if (h->owner != (uint32_t)current_pid) return -1;
    if (!alloc_unlink(current_pid, h)) return -1;
    h->magic = 0;
    myrtos_tlsf_free(pool_of(h), h);
    return 0;
}

// Grow or shrink. Allocate, copy, release: TLSF could sometimes extend a block
// where the neighbour is free, but ours has no path for it and the shortcut is
// worth nothing until something actually leans on realloc.
void *myrtos_mem_realloc(void *ptr, uint32_t size) {
    if (!ptr) return myrtos_mem_alloc(size);
    alloc_hdr_t *h = ((alloc_hdr_t*)ptr) - 1;
    if (h->magic != ALLOC_MAGIC || h->owner != (uint32_t)current_pid) return NULL;
    if (!size) { myrtos_mem_free(ptr); return NULL; }

    void *fresh = myrtos_mem_alloc(size);
    if (!fresh) return NULL;                    // the old block is left intact
    uint32_t keep = h->size < size ? h->size : size;
    for (uint32_t i = 0; i < keep; i++) ((uint8_t*)fresh)[i] = ((uint8_t*)ptr)[i];
    myrtos_mem_free(ptr);
    return fresh;
}

// Hand a block to another process. Nothing calls this yet; it is the operation
// message passing is built from, and the reason the owner is a field.
int32_t myrtos_mem_hand_over(void *ptr, int32_t to_pid) {
    if (!ptr || to_pid < 0 || to_pid >= MAX_PROCESSES) return -1;
    if (process_table[to_pid].state == PROC_STATE_FREE) return -1;
    alloc_hdr_t *h = ((alloc_hdr_t*)ptr) - 1;
    if (h->magic != ALLOC_MAGIC || h->owner != (uint32_t)current_pid) return -1;
    if (!alloc_unlink(current_pid, h)) return -1;
    alloc_link(to_pid, h);
    return 0;
}

// Where this process may keep state that a static variable cannot hold. Named
// apart from the ABI's inline wrapper, which the kernel also has in scope.
void *myrtos_process_data_area(uint32_t *size_out) {
    if (size_out) *size_out = process_table[current_pid].data_size;
    return process_table[current_pid].data_base;
}

// Block the running process. It is not made ready again here -- something else
// has to notice that what it waits for has happened.
void myrtos_block_on_read(int32_t path) {
    process_table[current_pid].state = PROC_STATE_WAIT_READ;
    process_table[current_pid].wait_path = path;
}

void myrtos_block_on_write(int32_t path) {
    process_table[current_pid].state = PROC_STATE_WAIT_WRITE;
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

#define MYRTOS_MAX_ARMS 8

typedef struct {
    int32_t  pid;
    int32_t  path;
    uint32_t type;
} arm_t;

static arm_t arms[MYRTOS_MAX_ARMS];

static int32_t pulse_deliver(int32_t dest, int32_t from, uint32_t type, uint32_t value);

// Called from the timer tick. A device driver knows whether it has anything
// waiting; asking it once per millisecond costs the kernel a few comparisons and
// costs the blocked process nothing at all.
void myrtos_wake_readers(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_STATE_WAIT_READ) {
            if (!myrtos_io_readable(process_table[i].wait_path, i)) continue;
        } else if (process_table[i].state == PROC_STATE_WAIT_WRITE) {
            if (!myrtos_io_writable(process_table[i].wait_path, i)) continue;
        } else {
            continue;
        }
        process_table[i].state = PROC_STATE_READY;
        ready_enqueue(i);
    }

    // And the watchers, in the same sweep. A pulse rather than a wake-up,
    // because a watcher is not blocked on the descriptor -- it is off doing
    // something else, or waiting in receive for whichever of several things
    // happens first, which is the entire point of arming.
    for (int i = 0; i < MYRTOS_MAX_ARMS; i++) {
        if (!arms[i].pid) continue;
        if (process_table[arms[i].pid].state == PROC_STATE_FREE) {
            arms[i].pid = 0;
            continue;
        }
        if (!myrtos_io_readable(arms[i].path, arms[i].pid)) continue;
        pulse_deliver(arms[i].pid, 0, arms[i].type, (uint32_t)arms[i].path);
        arms[i].pid = 0;                        // one shot; ask again for more
    }
}


// --- MESSAGES -------------------------------------------------------------
// send blocks until reply. That is the entire safety argument for passing a raw
// pointer: the sender is stopped, so its buffer cannot move or be rewritten, and
// the receiver may read it right up until it answers. Nothing is copied but the
// twelve-byte descriptor, and nothing is allocated at all.

static void msg_unlink_all(int32_t pid);
static void reap(uint32_t pid);

// --- PULSES ----------------------------------------------------------------
//
// A pulse is a message small enough to copy: a type and a value, no pointer and
// no reply. Sending one never blocks and never waits for anything, which is the
// whole point -- it is what myrtos_send deliberately is not.
//
// The two exist side by side because they pay for different things. A send
// blocks so that the receiver may read the sender's own memory without copying
// it, and fifteen kernel paths are built on that. A pulse carries no pointer,
// so there is nothing to keep alive and nothing to own: it is copied here and
// the sender walks away. QNX draws the same line and calls them the same thing.
//
// The ring is global rather than per process. Per process would be tidier and
// costs thirty-two times as much SRAM, which this machine does not have; the
// framebuffer is already eighty-five per cent of it. A global ring means one
// process can fill it and starve the rest, which is a real objection -- but a
// pulse may be refused, unlike a message that a blocked sender is waiting on,
// so the failure is visible to whoever caused it rather than silent.
#define MYRTOS_MAX_PULSES 24

typedef struct {
    int32_t  dest;
    int32_t  from;
    uint32_t type;
    uint32_t value;
} pulse_t;

static pulse_t pulses[MYRTOS_MAX_PULSES];
static uint32_t pulse_count;

static void pulse_into(myrtos_msg_t *out, int32_t from, uint32_t type, uint32_t value) {
    out->type   = type;
    out->len    = value;      // the value rides in len; a pulse has no buffer
    out->data   = 0;
    out->sender = from;
}

// Take the oldest pulse addressed to this process, if there is one.
static bool pulse_take(int32_t pid, myrtos_msg_t *out) {
    for (uint32_t i = 0; i < pulse_count; i++) {
        if (pulses[i].dest != pid) continue;
        pulse_into(out, pulses[i].from, pulses[i].type, pulses[i].value);
        for (uint32_t j = i + 1; j < pulse_count; j++) pulses[j - 1] = pulses[j];
        pulse_count--;
        return true;
    }
    return false;
}

// Non-blocking, and it may be refused. -1 means the destination is not there or
// the ring is full; the sender decides whether that matters.
// No interrupt guard, for the same reason myrtos_msg_send has none: this runs
// inside a trap, and a trap runs with mstatus.MIE clear. The day something wants
// to send a pulse from an interrupt handler -- which is exactly what a device
// arming a reader would want -- that stops being true and this needs one.
static int32_t pulse_deliver(int32_t dest, int32_t from, uint32_t type, uint32_t value) {
    if (dest <= 0 || dest >= MAX_PROCESSES) return -1;

    pcb_t *d = &process_table[dest];
    if (d->state == PROC_STATE_FREE) return -1;

    // Waiting for something to arrive: hand it over and wake it, exactly as a
    // message does, including taking a timed receiver off the sleep list so the
    // timer cannot wake it a second time.
    if (d->state == PROC_STATE_WAIT_RECV) {
        if (d->recv_timed) { sleep_remove(dest); d->recv_timed = false; }
        pulse_into(d->msg_out, from, type, value);
        d->msg_out = 0;
        // Zero, not a pid: a pulse is not replied to, and replying to zero
        // fails. QNX says the same thing with the same number.
        ((myrtos_frame_t*)(uintptr_t)d->saved_sp)->a0 = 0;
        d->state = PROC_STATE_READY;
        ready_enqueue(dest);
        return 0;
    }

    if (pulse_count >= MYRTOS_MAX_PULSES) return -1;
    pulses[pulse_count].dest  = dest;
    pulses[pulse_count].from  = from;
    pulses[pulse_count].type  = type;
    pulses[pulse_count].value = value;
    pulse_count++;
    return 0;
}

// --- THE INTERRUPT KEY ----------------------------------------------------
//
// Ctrl-C ends a process, and for nearly everything that is right: cat has
// nothing to say about being stopped. But a program driving a device may have
// something to undo -- the BleuIO scans until it is told to stop, and a scanner
// that is killed leaves the dongle scanning into a machine that is no longer
// listening.
//
// Unix answers this with a signal. QNX answers it with a pulse, and so does
// this: a process that has asked is TOLD, in the same receive it already uses
// for everything else, and ends itself. A program written in the arming style --
// hibouair is -- needs no new wait for this, because the pulse arrives where it
// is already looking.
//
// Three things make it safe. Asking is opt-in, so every existing command still
// dies on the key with no change. The deadline means a process that does not go
// is killed anyway, because Ctrl-C must always work in the end. And a second
// Ctrl-C kills at once, which is what a person does when the first did nothing.
#define MYRTOS_INTR_GRACE_TICKS 500

extern volatile uint32_t myrtos_ticks;
int32_t myrtos_process_kill(int32_t pid);

// Named apart from the ABI's myrtos_catch_intr, which is an inline this file
// also sees through modules.h.
int32_t myrtos_intr_catch(uint32_t type) {
    if (current_pid <= 0) return -1;
    process_table[current_pid].intr_pulse = type;
    return 0;
}

// True when the process was told and should be given its grace. False when it
// must be killed -- because it never asked, or because it was asked already and
// this is the second press.
bool myrtos_intr_request(int32_t pid) {
    if (pid <= 0 || pid >= MAX_PROCESSES) return false;
    pcb_t *p = &process_table[pid];
    if (p->state == PROC_STATE_FREE) return false;
    if (!p->intr_pulse) return false;
    if (p->intr_deadline >= 0) return false;          // asked once already

    // Never zero: zero is what an uninitialised slot holds, and this must not
    // be mistaken for one.
    uint32_t at = myrtos_ticks + MYRTOS_INTR_GRACE_TICKS;
    p->intr_deadline = (int32_t)(at ? at : 1u);
    pulse_deliver(pid, 0, p->intr_pulse, 0);          // from the kernel, as arming does
    return true;
}

// Called from the tick. A process that was asked and stayed is ended.
//
// intr_pulse is tested first and that is not belt and braces. A kernel thread is
// not built by myrtos_process_create, so its fields keep the zeroes the static
// table was born with -- and a deadline of zero is in the past. The first
// version tested the deadline alone and killed the USB thread, the filesystem
// server and everything else on the very first tick, half a second after the
// board came up. Only a process that asked can have a deadline that means
// anything.
void myrtos_intr_tick(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        pcb_t *p = &process_table[i];
        if (p->state == PROC_STATE_FREE) continue;
        if (!p->intr_pulse || p->intr_deadline <= 0) continue;
        if ((int32_t)(myrtos_ticks - (uint32_t)p->intr_deadline) < 0) continue;
        p->intr_deadline = -1;
        myrtos_process_kill(i);
    }
}

int32_t myrtos_pulse_send(int32_t dest, uint32_t type, uint32_t value) {
    return pulse_deliver(dest, (int32_t)current_pid, type, value);
}

// --- ARMING ----------------------------------------------------------------
//
// Ask to be told when a descriptor has something, instead of asking it over and
// over. QNX calls this ionotify and delivers the answer as a pulse, which is
// exactly why pulses came first.
//
// It rides on the loop below, which the timer already runs once a millisecond
// to wake blocked readers. No driver knows anything about this: the same
// myrtos_io_readable that decides whether a sleeping reader may run decides
// whether a watcher gets its pulse.
//
// One shot, as ionotify is. It fires once and disarms, so a device that stays
// readable does not bury its watcher in pulses -- and a program that wants the
// next one says so, which is also the moment it has finished with the last.
//
// The pulse comes from pid 0, the kernel, because that is the truth: no process
// sent it. Its value is the descriptor, so a process watching several knows
// which one woke.
// type 0 cancels. Arming the same descriptor twice replaces the first, so a
// program cannot accumulate watches it has forgotten about.
int32_t myrtos_arm_read(int32_t path, uint32_t type) {
    int32_t free_slot = -1;
    for (int i = 0; i < MYRTOS_MAX_ARMS; i++) {
        if (arms[i].pid == (int32_t)current_pid && arms[i].path == path) {
            if (type == 0) { arms[i].pid = 0; return 0; }
            arms[i].type = type;
            return 0;
        }
        if (!arms[i].pid && free_slot < 0) free_slot = i;
    }
    if (type == 0) return 0;                    // cancelling what was not armed
    if (free_slot < 0) return -1;
    arms[free_slot].pid  = (int32_t)current_pid;
    arms[free_slot].path = path;
    arms[free_slot].type = type;
    return 0;
}

// Every watch this process holds, dropped at once. The pattern it is for is
// arming several sources and caring only about whichever speaks first: the
// moment the first pulse arrives, the rest are of no interest, and each would
// otherwise fire one stray pulse into a receive that is no longer expecting it.
int32_t myrtos_disarm_reads(void) {
    int32_t n = 0;
    for (int i = 0; i < MYRTOS_MAX_ARMS; i++)
        if (arms[i].pid == (int32_t)current_pid) { arms[i].pid = 0; n++; }

    // And the ones that have already gone off. Dropping the watches alone was
    // not enough and the board said so: two descriptors armed, both readable in
    // the same tick, and the sweep fired both before the process ran at all.
    // By the time it had its first pulse and asked to disarm, there was nothing
    // left armed -- the answer was zero -- and the second pulse was already
    // queued and arrived anyway.
    //
    // So this means "I have stopped listening for descriptors", pending ones
    // included. Only pulses from pid 0 go, which today means only the ones a
    // watch produced; a pulse from another process is somebody talking to you
    // and is none of this call's business.
    uint32_t i = 0;
    while (i < pulse_count) {
        if (pulses[i].dest != (int32_t)current_pid || pulses[i].from != 0) { i++; continue; }
        for (uint32_t j = i + 1; j < pulse_count; j++) pulses[j - 1] = pulses[j];
        pulse_count--;
        n++;
    }
    return n;
}

static void arms_drop_for(int32_t pid) {
    for (int i = 0; i < MYRTOS_MAX_ARMS; i++)
        if (arms[i].pid == pid) arms[i].pid = 0;
}

// Drop anything addressed to a process that has gone. Nobody is waiting on
// these -- that is what makes them pulses -- so they simply disappear.
static void pulse_drop_for(int32_t pid) {
    uint32_t i = 0;
    while (i < pulse_count) {
        if (pulses[i].dest != pid) { i++; continue; }
        for (uint32_t j = i + 1; j < pulse_count; j++) pulses[j - 1] = pulses[j];
        pulse_count--;
    }
}

// True when the sender has been queued and must now block.
bool myrtos_msg_send(int32_t dest, const myrtos_msg_t *m) {
    if (dest <= 0 || dest >= MAX_PROCESSES || !m) return false;
    pcb_t *d = &process_table[dest];
    if (d->state == PROC_STATE_FREE) return false;

    pcb_t *me = &process_table[current_pid];
    me->msg = *m;
    me->msg.sender = (int32_t)current_pid;   // so the receiver can put it aside
    me->msg_next = -1;
    me->msg_dest = dest;

    if (d->state == PROC_STATE_WAIT_RECV) {
        // A receiver with a deadline is on the sleep list as well. It is being
        // woken for the better reason, so take it off before the timer can also
        // wake it -- a process on the ready queue twice is a process that
        // returns from receive twice.
        if (d->recv_timed) {
            sleep_remove(dest);
            d->recv_timed = false;
        }
        // Nobody ahead of us: hand it straight over and wake the server.
        *d->msg_out = me->msg;
        d->msg_out = 0;
        d->msg_serving = (int32_t)current_pid;
        ((myrtos_frame_t*)(uintptr_t)d->saved_sp)->a0 = (uint32_t)current_pid;
        d->state = PROC_STATE_READY;
        ready_enqueue(dest);
    } else {
        if (d->msg_tail < 0) d->msg_head = (int32_t)current_pid;
        else process_table[d->msg_tail].msg_next = (int32_t)current_pid;
        d->msg_tail = (int32_t)current_pid;
    }
    me->state = PROC_STATE_WAIT_REPLY;
    return true;
}

// The sender's pid, or -1 when the caller has been put to sleep waiting.
// Wait for a message, for at most `ms` milliseconds. Zero polls and never
// blocks; MYRTOS_TIMEOUT_FOREVER is the old behaviour and is what
// myrtos_msg_receive asks for.
//
// The deadline is the ordinary sleep list -- the same one myrtos_sleep uses --
// so a receiver with a timeout is queued in two places at once and whichever
// happens first cancels the other. reap already unlinks a dying process from
// the sleep list, so nothing more is needed there.
int32_t myrtos_msg_receive_tmo(myrtos_msg_t *out, uint32_t ms) {
    // A second request may be taken before the first is answered: a server that
    // has to wait for something puts the sender aside with myrtos_reply_to and
    // goes on serving. msg_serving is merely the most recent, for the simple
    // servers that answer before they ask again.
    pcb_t *me = &process_table[current_pid];

    // Messages before pulses, because a message has a sender standing blocked
    // behind it and a pulse has nobody. The cost is that a busy server can
    // leave pulses waiting, which is the right way round: the process that is
    // stuck is served first.
    if (me->msg_head >= 0) {
        int32_t from = me->msg_head;
        me->msg_head = process_table[from].msg_next;
        if (me->msg_head < 0) me->msg_tail = -1;
        process_table[from].msg_next = -1;
        *out = process_table[from].msg;
        me->msg_serving = from;
        return from;
    }
    if (pulse_take((int32_t)current_pid, out)) return 0;   // 0: do not reply

    if (ms == 0) return MYRTOS_RECV_TIMEOUT;   // a poll, which never blocks

    me->msg_out = out;
    me->state = PROC_STATE_WAIT_RECV;
    if (ms != MYRTOS_TIMEOUT_FOREVER) {
        sleep_insert((int32_t)current_pid, ms);
        me->recv_timed = true;
    }
    return -1;
}

int32_t myrtos_msg_receive(myrtos_msg_t *out) {
    return myrtos_msg_receive_tmo(out, MYRTOS_TIMEOUT_FOREVER);
}

int32_t myrtos_msg_reply(int32_t status) {
    pcb_t *me = &process_table[current_pid];
    int32_t s = me->msg_serving;
    if (s < 0) return -1;
    me->msg_serving = -1;
    // Killed while we were serving it. The answer has nowhere to go, but this
    // is the moment its memory stops being ours to write to.
    if (process_table[s].state == PROC_STATE_ZOMBIE) { reap((uint32_t)s); return 0; }
    if (process_table[s].state == PROC_STATE_WAIT_REPLY) {
        ((myrtos_frame_t*)(uintptr_t)process_table[s].saved_sp)->a0 = (uint32_t)status;
        process_table[s].state = PROC_STATE_READY;
        ready_enqueue(s);
    }
    return 0;
}

// A process by module name, so a client can name the service it wants without
// anyone having written a pid down.
// Answer a particular sender. Refuses one that is not blocked waiting on this
// process, so a server cannot release somebody else's client.
int32_t myrtos_msg_reply_to(int32_t pid, int32_t status) {
    if (pid <= 0 || pid >= MAX_PROCESSES) return -1;
    pcb_t *p = &process_table[pid];
    if (p->state == PROC_STATE_ZOMBIE) {        // killed while we held it
        if (process_table[current_pid].msg_serving == pid)
            process_table[current_pid].msg_serving = -1;
        reap((uint32_t)pid);
        return 0;
    }
    if (p->state != PROC_STATE_WAIT_REPLY) return -1;
    if (p->msg_dest != (int32_t)current_pid) return -1;

    if (process_table[current_pid].msg_serving == pid)
        process_table[current_pid].msg_serving = -1;
    p->msg_dest = -1;
    ((myrtos_frame_t*)(uintptr_t)p->saved_sp)->a0 = (uint32_t)status;
    p->state = PROC_STATE_READY;
    ready_enqueue(pid);
    return 0;
}

int32_t myrtos_find_pid(const char *name) {
    if (!name) return -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        const pcb_t *p = &process_table[i];
        if (p->state == PROC_STATE_FREE || !p->module) continue;
        const char *n = (const char*)((uintptr_t)p->module + p->module->name_offset);
        int k = 0;
        while (k < 11 && name[k] && n[k] == name[k]) k++;
        if (!name[k] && (n[k] == 0 || n[k] == ' ')) return (int32_t)p->pid;
    }
    return -1;
}

// A server that dies must not take its callers with it. Everyone queued on it,
// and anyone it was serving, is released with a failure rather than left in
// WAIT_REPLY for ever.
static void msg_unlink_all(int32_t pid) {
    pcb_t *p = &process_table[pid];
    int32_t s = p->msg_head;
    while (s >= 0) {
        int32_t next = process_table[s].msg_next;
        process_table[s].msg_next = -1;
        if (process_table[s].state == PROC_STATE_WAIT_REPLY) {
            ((myrtos_frame_t*)(uintptr_t)process_table[s].saved_sp)->a0 = (uint32_t)-1;
            process_table[s].state = PROC_STATE_READY;
            ready_enqueue(s);
        }
        s = next;
    }
    p->msg_head = p->msg_tail = -1;

    // Anyone put aside for a later answer is waiting on us too, and is not in
    // the queue any more. They would wait for ever otherwise.
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_STATE_WAIT_REPLY) continue;
        if (process_table[i].msg_dest != pid) continue;
        process_table[i].msg_dest = -1;
        ((myrtos_frame_t*)(uintptr_t)process_table[i].saved_sp)->a0 = (uint32_t)-1;
        process_table[i].state = PROC_STATE_READY;
        ready_enqueue(i);
    }

    if (p->msg_serving >= 0) {
        int32_t v = p->msg_serving;
        p->msg_serving = -1;
        if (process_table[v].state == PROC_STATE_WAIT_REPLY) {
            ((myrtos_frame_t*)(uintptr_t)process_table[v].saved_sp)->a0 = (uint32_t)-1;
            process_table[v].state = PROC_STATE_READY;
            ready_enqueue(v);
        }
    }
}

// The current directory, and how a child comes to share its parent's.
const char *myrtos_cwd_get(void) { return process_table[current_pid].cwd; }

// The same, for another process. The filesystem server resolves a client's
// relative path and so needs the client's directory, not its own.
const char *myrtos_cwd_of(int32_t pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return "/";
    return process_table[pid].cwd;
}

bool myrtos_cwd_set_of(int32_t pid, const char *abs) {
    if (pid < 0 || pid >= MAX_PROCESSES) return false;
    char *dst = process_table[pid].cwd;
    uint32_t max = sizeof(process_table[0].cwd), i = 0;
    while (abs[i] && i < max - 1) { dst[i] = abs[i]; i++; }
    if (abs[i]) return false;
    dst[i] = 0;
    return true;
}

void myrtos_cwd_inherit(int32_t parent, int32_t child) {
    if (parent < 0 || parent >= MAX_PROCESSES) return;
    if (child  < 0 || child  >= MAX_PROCESSES) return;
    uint32_t i = 0;
    while (i < sizeof(process_table[0].cwd) - 1 && process_table[parent].cwd[i]) {
        process_table[child].cwd[i] = process_table[parent].cwd[i];
        i++;
    }
    process_table[child].cwd[i] = 0;
}

bool myrtos_cwd_set(const char *abs) {
    char *dst = process_table[current_pid].cwd;
    uint32_t max = sizeof(process_table[0].cwd);
    uint32_t i = 0;
    while (abs[i] && i < max - 1) { dst[i] = abs[i]; i++; }
    if (abs[i]) return false;                   // would not fit; leave it alone
    dst[i] = 0;
    return true;
}

// Whoever was waiting for this one can run again. Exact, unlike the read
// wake-up: the event is this call, and nothing has to be polled to see it.
static void wake_waiters(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_STATE_WAIT_CHILD) continue;
        if (process_table[i].wait_pid != (int32_t)pid) continue;
        process_table[i].state = PROC_STATE_READY;
        ready_enqueue(i);
    }
}

// Take a process apart. Written for the one ending itself, and now also used by
// the one being killed and by the reply that releases a zombie, so it names the
// process it is working on rather than assuming it is the current one.
static void reap(uint32_t pid) {
    sleep_remove((int32_t)pid);                 // harmless if it was not asleep
    msg_unlink_all((int32_t)pid);               // release anyone waiting on us
    pulse_drop_for((int32_t)pid);               // nobody is waiting on these
    arms_drop_for((int32_t)pid);                // and no one to tell any more
    myrtos_io_close_all(pid);
    // And on the coprocessor, where a listening socket keeps its port. Marks
    // only -- see the note in wifilink.c for why it cannot close them here.
    { extern void myrtos_wifi_forget_pid(int32_t pid); myrtos_wifi_forget_pid((int32_t)pid); }
#if MYRTOS_LWIP
    { extern void myrtos_lwip_forget_pid(int32_t pid); myrtos_lwip_forget_pid((int32_t)pid); }
#endif
    if (process_table[pid].module) {            // a kernel thread has none
        myrtos_moddir_unlink(process_table[pid].module);
    }
    // Everything this process was given goes back, whether it freed it or not.
    alloc_hdr_t *h = process_table[pid].allocs;
    while (h) { alloc_hdr_t *next = h->next; h->magic = 0;
                myrtos_tlsf_free(pool_of(h), h); h = next; }
    process_table[pid].allocs = NULL;

    myrtos_tlsf_free(pool_of(process_table[pid].mem_base),
                     process_table[pid].mem_base);
    if (process_table[pid].code_base) {
        myrtos_tlsf_free(pool_of(process_table[pid].code_base),
                         process_table[pid].code_base);
        process_table[pid].code_base = NULL;
    }
    process_table[pid].state = PROC_STATE_FREE;
    process_table[pid].mem_base = NULL;

    wake_waiters(pid);
}

void myrtos_process_exit(void) {
    if (current_pid == KERNEL_PID) return;      // the kernel is never terminated
    reap(current_pid);
}

// End somebody else.
//
// The hard case is a process blocked on a server. Its message carries a pointer
// to a buffer of its own -- the wifi request is a local of the system call, so
// it is on that process's stack -- and the server is holding it. Freeing the
// block now would leave the server writing its answer into memory that has been
// given to someone else, seconds later and with nothing to connect the two.
//
// So it stops being a process immediately, which is what the person who typed
// ctrl-C asked for, and is not taken apart until the reply arrives. Everything
// that would look at it -- the scheduler, wait, a later reply -- can tell.
int32_t myrtos_process_kill(int32_t pid) {
    if (pid <= 0 || pid >= MAX_PROCESSES) return -1;
    pcb_t *p = &process_table[pid];
    if (p->state == PROC_STATE_FREE || p->state == PROC_STATE_ZOMBIE) return -1;

    // A kernel thread is the console, the filesystem or the radio. The machine
    // needs all three, and none of them was started by anyone who could be
    // asked whether they meant it.
    if (!p->module) return -1;

    if (p->state == PROC_STATE_WAIT_REPLY) {
        sleep_remove(pid);
        p->state = PROC_STATE_ZOMBIE;
        wake_waiters((uint32_t)pid);
        return 0;
    }

    if (p->state == PROC_STATE_READY) ready_remove(pid);
    reap((uint32_t)pid);
    return 0;
}

// --- THE MACHINE TIMER ----------------------------------------------------
//
// Only on this machine. Cortex-M33 has SysTick and a separate free-running
// counter, and kernel/arm/timer.c answers the same three questions with them --
// so this whole section is the RISC-V half of a pair, not a general facility.
#ifdef __riscv

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

#endif   // __riscv

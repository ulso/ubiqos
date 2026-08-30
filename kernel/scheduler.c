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
    PROC_STATE_WAIT_REPLY       // a sender whose message has not been answered
} proc_state_t;

typedef struct {
    uint32_t pid;
    proc_state_t state;
    uintptr_t entry_point;
    const myrtos_module_header_t *module;   // shared code, one copy for all
    void* mem_base;           // bottom of the allocation, private per process
    void* data_base;          // where the module's own state may start
    uint32_t data_size;       // how far it reaches before the stack comes down
    uint32_t mem_size;        // data + stack, as the module header asked for
    uint32_t saved_sp;        // the trap frame, hence the entire context
    const char *args;         // points into the process's OWN memory, not here
    int32_t  wait_path;       // WAIT_READ: the path being waited on
    int32_t  wait_pid;        // WAIT_CHILD: the process being waited for
    int32_t  sleep_delta;     // SLEEPING: ticks after the process ahead of it
    int32_t  sleep_next;      // SLEEPING: next in the delta list, -1 at the end
    uint32_t priority;        // 0 is the idle process, 31 the most urgent
    int32_t  next_ready;      // READY: next in this priority's queue, -1 at the end
    struct alloc_hdr *allocs; // everything this process has been given

    // Messages. A rendezvous queues senders, not messages: the message stays in
    // the sender's memory, which cannot change because the sender is stopped.
    // So this is one more list through the process table, like the two above.
    int32_t  msg_next;        // WAIT_REPLY: next sender queued on the same server
    int32_t  msg_head;        // senders waiting for ME, -1 when none
    int32_t  msg_tail;
    int32_t  msg_serving;     // the sender I am serving, -1 when none
    myrtos_msg_t msg;         // WAIT_REPLY: what this sender is offering
    myrtos_msg_t *msg_out;    // WAIT_RECV: where the message is to be delivered

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
        process_table[i].sleep_next = -1;
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
    __asm__ volatile("mv %0, gp" : "=r"(kernel_gp));
    __asm__ volatile("mv %0, tp" : "=r"(kernel_tp));
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
    frame->mepc = (uint32_t)(uintptr_t)entry;
    frame->ra   = (uint32_t)(uintptr_t)myrtos_process_return;
    frame->gp   = kernel_gp;
    frame->tp   = kernel_tp;      // a kernel thread has no data area to point at

    process_table[slot].entry_point = frame->mepc;
    process_table[slot].module   = NULL;      // nothing to unlink when it ends
    process_table[slot].mem_base = mem;
    process_table[slot].data_base = NULL;     // a kernel thread keeps its state
    process_table[slot].data_size = 0;        // in the kernel's own variables
    process_table[slot].mem_size = stack_bytes;
    process_table[slot].saved_sp = (uint32_t)(uintptr_t)frame;
    process_table[slot].msg_next = process_table[slot].msg_head = -1;
    process_table[slot].msg_tail = process_table[slot].msg_serving = -1;
    process_table[slot].msg_out = 0;
    process_table[slot].cwd[0] = '/';
    process_table[slot].cwd[1] = 0;
    process_table[slot].args     = NULL;
    process_table[slot].sleep_next = -1;
    process_table[slot].allocs = NULL;
    process_table[slot].priority = priority;
    process_table[slot].state    = PROC_STATE_READY;
    ready_enqueue(slot);
    return slot;
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
    void *mem = myrtos_tlsf_malloc(myrtos_pool_for(module_ptr), bytes);
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
    // The thread-local block goes first, because tp points at it and the linker
    // assigned every variable an offset from zero. The module's initial values
    // are copied in and the rest zeroed -- the same job OS-9's loader did with
    // a module's data section. Whatever follows is the area myrtos_data_area
    // hands out for raw use.
    uintptr_t tls_base = ((uintptr_t)&argv[argc + 1] + 3) & ~(uintptr_t)3;

    const uint8_t *tls_src = module_ptr->tls_init
        ? (const uint8_t*)module_ptr + module_ptr->tls_offset : 0;
    for (uint32_t i = 0; i < module_ptr->tls_total; i++) {
        ((uint8_t*)tls_base)[i] = (tls_src && i < module_ptr->tls_init) ? tls_src[i] : 0;
    }

    uintptr_t data_base = (tls_base + module_ptr->tls_total + 3) & ~(uintptr_t)3;

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

    // The thread pointer carries the data area. That is what tp is for: this
    // process's own storage, which is thread-local storage with our layout
    // rather than the compiler's. Nothing else uses it -- .tdata and .tbss are
    // both empty -- and the trap frame already saves and restores it per
    // process, so a module reads its own state with one instruction instead of
    // a system call. It is OS-9's U register, in the register meant for it.
    // tp is the thread-local block, not the raw area: the offsets the linker
    // baked into the code all count from here.
    frame->tp   = (uint32_t)tls_base;

    process_table[slot].entry_point = frame->mepc;
    process_table[slot].module = module_ptr;
    process_table[slot].mem_base = mem;
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
    process_table[slot].msg_out = 0;
    process_table[slot].cwd[0] = '/';
    process_table[slot].cwd[1] = 0;
    process_table[slot].args = (const char*)mem;
    process_table[slot].sleep_next = -1;

    // Priority is inherited, as paths are: that is what lets `nice` work
    // without the started program knowing anything about priorities. The
    // exception is the kernel, which creates the first process from the idle
    // level -- inheriting that would leave the shell below everything.
    process_table[slot].allocs = NULL;
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
// not have to carry it.
tlsf_pool_t myrtos_pool_of_address(void *p) {
    return ((uintptr_t)p >= MYRTOS_PSRAM_BASE && myrtos_bulk_pool)
         ? myrtos_bulk_pool : myrtos_mem_pool;
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
}


// --- MESSAGES -------------------------------------------------------------
// send blocks until reply. That is the entire safety argument for passing a raw
// pointer: the sender is stopped, so its buffer cannot move or be rewritten, and
// the receiver may read it right up until it answers. Nothing is copied but the
// twelve-byte descriptor, and nothing is allocated at all.

static void msg_unlink_all(int32_t pid);

// True when the sender has been queued and must now block.
bool myrtos_msg_send(int32_t dest, const myrtos_msg_t *m) {
    if (dest <= 0 || dest >= MAX_PROCESSES || !m) return false;
    pcb_t *d = &process_table[dest];
    if (d->state == PROC_STATE_FREE) return false;

    pcb_t *me = &process_table[current_pid];
    me->msg = *m;
    me->msg_next = -1;

    if (d->state == PROC_STATE_WAIT_RECV) {
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
int32_t myrtos_msg_receive(myrtos_msg_t *out) {
    pcb_t *me = &process_table[current_pid];
    if (me->msg_serving >= 0) return -2;        // answer the last one first
    if (me->msg_head >= 0) {
        int32_t from = me->msg_head;
        me->msg_head = process_table[from].msg_next;
        if (me->msg_head < 0) me->msg_tail = -1;
        process_table[from].msg_next = -1;
        *out = process_table[from].msg;
        me->msg_serving = from;
        return from;
    }
    me->msg_out = out;
    me->state = PROC_STATE_WAIT_RECV;
    return -1;
}

int32_t myrtos_msg_reply(int32_t status) {
    pcb_t *me = &process_table[current_pid];
    int32_t s = me->msg_serving;
    if (s < 0) return -1;
    me->msg_serving = -1;
    if (process_table[s].state == PROC_STATE_WAIT_REPLY) {
        ((myrtos_frame_t*)(uintptr_t)process_table[s].saved_sp)->a0 = (uint32_t)status;
        process_table[s].state = PROC_STATE_READY;
        ready_enqueue(s);
    }
    return 0;
}

// A process by module name, so a client can name the service it wants without
// anyone having written a pid down.
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

void myrtos_process_exit(void) {
    if (current_pid == KERNEL_PID) return;      // the kernel is never terminated
    sleep_remove(current_pid);                  // harmless if it was not asleep
    msg_unlink_all((int32_t)current_pid);       // release anyone waiting on us
    myrtos_io_close_all(current_pid);
    if (process_table[current_pid].module) {    // a kernel thread has none
        myrtos_moddir_unlink(process_table[current_pid].module);
    }
    // Everything this process was given goes back, whether it freed it or not.
    alloc_hdr_t *h = process_table[current_pid].allocs;
    while (h) { alloc_hdr_t *next = h->next; h->magic = 0;
                myrtos_tlsf_free(pool_of(h), h); h = next; }
    process_table[current_pid].allocs = NULL;

    myrtos_tlsf_free(pool_of(process_table[current_pid].mem_base),
                     process_table[current_pid].mem_base);
    process_table[current_pid].state = PROC_STATE_FREE;
    process_table[current_pid].mem_base = NULL;

    // Whoever was waiting for this one can run again. Exact, unlike the read
    // wake-up: the event is this line, and nothing has to be polled to see it.
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_STATE_WAIT_CHILD) continue;
        if (process_table[i].wait_pid != current_pid) continue;
        process_table[i].state = PROC_STATE_READY;
        ready_enqueue(i);
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

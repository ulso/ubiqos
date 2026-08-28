#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tlsf.h"
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "moddir.h"

#define MAX_PROCESSES 8
#define KERNEL_PID    0     // Kärnan är själv en process, alltid körbar.

typedef enum {
    PROC_STATE_FREE,
    PROC_STATE_READY,
    PROC_STATE_RUNNING
} proc_state_t;

typedef struct {
    uint32_t pid;
    proc_state_t state;
    uintptr_t entry_point;
    const myrtos_module_header_t *module;   // delad kod, en kopia för alla
    void* mem_base;           // Dataområdets botten, privat per process
    uint32_t mem_size;        // Data + stack, som modulhuvudet begärde
    uint32_t saved_sp;        // Trap-ramen, alltså hela sammanhanget
    const char *args;         // pekar in i processens EGET minne, inte hit
} pcb_t;

static pcb_t process_table[MAX_PROCESSES];
static int32_t current_pid = KERNEL_PID;

// Kärnans globalpekare och trådpekare. crt0 sätter gp till __global_pointer$,
// och kärnans C-kod adresserar sina små globaler relativt den. En ny process
// måste ärva dem: annars återställer trap-vektorn processens nollade gp när
// den trappar in i kärnan, och hanteraren skriver vilt i minnet.
static uint32_t kernel_gp, kernel_tp;

extern tlsf_pool_t myrtos_mem_pool;
void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

#define SYS_EXIT 2u

// Dit en process returnerar när dess module_main är klar. Den kan inte
// returnera till kärnan -- den har ingen sådan anropskedja -- så den ber om
// att bli avslutad i stället.
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
    // Kärnan är pid 0. Dess sammanhang fylls i vid första trappen, eftersom
    // den redan kör på sin egen stack.
    process_table[KERNEL_PID].state = PROC_STATE_RUNNING;
    current_pid = KERNEL_PID;
    __asm__ volatile("mv %0, gp" : "=r"(kernel_gp));
    __asm__ volatile("mv %0, tp" : "=r"(kernel_tp));
    myrtos_print("Real-time process scheduler initialized.\n");
}

int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr,
                              const char *args) {
    int32_t slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {      // 0 är kärnan
        if (process_table[i].state == PROC_STATE_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        myrtos_print("Error: process table full.\n");
        return -1;
    }

    // Ett sammanhängande område: data nedtill, stack uppifrån och nedåt.
    uint32_t bytes = module_ptr->mem_size;
    void *mem = myrtos_tlsf_malloc(myrtos_mem_pool, bytes);
    if (!mem) {
        myrtos_print("Error: failed to allocate process memory.\n");
        return -1;
    }

    // Kommandoraden längst ned i processens eget område, följd av en argv-
    // vektor. Båda frigörs med resten när processen dör, så varken egen
    // allokering eller egen frigöring behövs -- och enda gränsen är mem_size.
    //
    // Ordningen är Unix ordning: strängen delas på plats med nolltecken, och
    // pekarna läggs efter den. Modulen får argc i a0 och argv i a1, alltså
    // exakt vad main(int, char**) förväntar sig.
    char *dst = (char*)mem;
    uint32_t n = 0;
    if (args) while (args[n] && n < bytes / 2) { dst[n] = args[n]; n++; }
    dst[n] = 0;

    char **argv = (char**)(((uintptr_t)mem + n + 1 + 3) & ~(uintptr_t)3);
    int argc = 0;

    // argv[0] är modulens eget namn, som i alla system sedan Unix.
    argv[argc++] = (char*)((uintptr_t)module_ptr + module_ptr->name_offset);

    // Delningen förstår citattecken: ett citerat stycke är ETT argument, och
    // citattecknen själva försvinner. Eftersom bara tecken tas bort, aldrig
    // läggs till, kan resultatet komprimeras i samma buffert -- skrivpekaren
    // ligger alltid bakom läspekaren.
    char *p = dst;      // läser
    char *w = dst;      // skriver
    while (*p && argc < 16) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = w;
        while (*p && *p != ' ') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) *w++ = *p++;
                if (*p) p++;          // hoppa över avslutande citattecken
            } else {
                *w++ = *p++;
            }
        }
        // Avgränsaren måste konsumeras INNAN ordet termineras. Utan citattecken
        // går pekarna i takt, och nolltecknet skulle annars skriva över just
        // det blanksteg som läspekaren står på -- delaren såg då strängslut och
        // tappade allt efter första ordet.
        while (*p == ' ') p++;
        *w++ = 0;
    }
    argv[argc] = 0;

    uintptr_t data_base = ((uintptr_t)&argv[argc + 1] + 3) & ~(uintptr_t)3;
    (void)data_base;   // reserverat åt modulens eget dataområde

    uintptr_t stack_top = ((uintptr_t)mem + bytes) & ~(uintptr_t)15;
    myrtos_frame_t *frame = (myrtos_frame_t*)(stack_top - sizeof(myrtos_frame_t));
    for (uint32_t i = 0; i < sizeof(myrtos_frame_t) / 4; i++) {
        ((uint32_t*)frame)[i] = 0;
    }
    // När schemaläggaren väljer processen återställer vektorn de här värdena
    // och mret hoppar till mepc. Det är så en process startar: som om den
    // just blivit avbruten precis före sin första instruktion.
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

    // Adresserna är hela poängen: kör två processer samma modul ska koden
    // ligga på samma ställe och dataområdena på olika.
    myrtos_print("  pid ");
    myrtos_print_u32(slot);
    myrtos_print(": code at 0x");
    myrtos_print_hex((uint32_t)(uintptr_t)module_ptr);
    myrtos_print(", data at 0x");
    myrtos_print_hex((uint32_t)(uintptr_t)mem);
    myrtos_print("\n");
    return slot;
}

// Anropas ur trap-hanteraren. Sparar den avbrutna processens stack och
// returnerar den som ska tas vid -- rundgång över allt som är körbart.
uint32_t myrtos_switch(uint32_t current_sp) {
    process_table[current_pid].saved_sp = current_sp;
    if (process_table[current_pid].state == PROC_STATE_RUNNING) {
        process_table[current_pid].state = PROC_STATE_READY;
    }

    int32_t next = current_pid;
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

// En process har bett om att få dö. Minnet lämnas tillbaka och platsen frigörs.
void myrtos_process_exit(void) {
    if (current_pid == KERNEL_PID) return;      // kärnan avslutas inte
    myrtos_print("Process ");
    myrtos_print_u32(current_pid);
    myrtos_print(" exited.\n");
    myrtos_io_close_all(current_pid);
    myrtos_moddir_unlink(process_table[current_pid].module);
    myrtos_tlsf_free(myrtos_mem_pool, process_table[current_pid].mem_base);
    process_table[current_pid].state = PROC_STATE_FREE;
    process_table[current_pid].mem_base = NULL;
}

// --- MASKINTIMERN ---------------------------------------------------------
// Hazard3 har en standardiserad RISC-V-maskintimer i SIO. mtime räknar från
// tick-generatorn som runtime_init sätter till en puls per mikrosekund, och
// ett avbrott utlöses när mtime når mtimecmp.

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

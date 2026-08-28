#ifndef MYRTOS_ABI_H
#define MYRTOS_ABI_H

#include <stdint.h>

// The interface between the kernel and the modules. Everything both sides must
// agree on lives here and nowhere else -- the module format, the system call
// numbers and the calling convention.
//
// The numbers used to live in the kernel AND in every module, written out by
// hand in three places. A change in the kernel then produced no build error but
// an "unknown system call" at runtime, which is exactly the kind of silent
// drift an ABI exists to prevent.
//
// Once this stops moving, this file is what a myrtos SDK consists of: module
// development needs it, not the kernel sources.

// --- FORMAT VERSION -------------------------------------------------------
// Lives in the low byte of attr_rev. The kernel rejects modules built against
// another version rather than running them and failing somewhere obscure.
#define MYRTOS_ABI_VERSION 1

// --- MODULE HEADER --------------------------------------------------------
#define MYRTOS_SYNC_CODE 0x0509000B

// Type, in the high byte of type_lang.
#define MYRTOS_TYPE_PROGRAM 1
#define MYRTOS_TYPE_DRIVER  2
#define MYRTOS_TYPE_DATA    3   // no code, no entry point

// --- DEVICE DESCRIPTORS ---------------------------------------------------
// A data module describing a device, in the OS-9 sense. It states what the
// device is called, which driver module handles it, and carries a tail that
// only that driver understands.
//
// The point is that a device can be added by dropping a file on the card
// rather than rebuilding the kernel. To change UART or baud rate you change
// the descriptor.

#define MYRTOS_CLASS_CHAR   1   // character stream: terminal, serial port
#define MYRTOS_CLASS_BLOCK  2   // block oriented: SD, disk

typedef struct __attribute__((packed, aligned(4))) {
    char     device_name[12];   // what a process opens: "term"
    char     driver_name[12];   // the module handling it: "UART    MOD"
    uint16_t device_class;      // MYRTOS_CLASS_*
    uint16_t reserved;
    uint32_t config_offset;     // from the start of the descriptor to the tail
    uint32_t config_size;       // size of the tail, zero if none
} myrtos_descriptor_t;

// The tail for the UART driver. Its layout is the driver's business alone; the
// I/O manager passes it on without interpreting it.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t uart_base;         // 0x40070000 for UART0 on the RP2350
    uint32_t tx_pin;
    uint32_t rx_pin;            // 0xffffffff if send-only
    uint32_t baud_rate;
} myrtos_uart_config_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t sync_code;      // MYRTOS_SYNC_CODE
    uint32_t module_size;    // the whole module, header included
    uint32_t name_offset;    // to the name string
    uint16_t type_lang;      // type (program, driver) and language
    uint16_t attr_rev;       // attributes and ABI version
    uint32_t exec_offset;    // to the entry point
    uint32_t mem_size;       // RAM per process: data at the bottom, stack from the top
    uint32_t header_crc;     // complement of the sum of the first six words
} myrtos_module_header_t;

// --- SYSTEM CALLS ---------------------------------------------------------
// a7 carries the number, a0-a2 the arguments, a0 comes back with the result.
#define SYS_NULL      0u   // does nothing; exists to exercise the trap path
#define SYS_IO_PUTC   1u   // a0 = character
#define SYS_EXIT      2u   // ends the process, never returns
#define SYS_OPEN      3u   // a0 = device name       -> a0 = path number
#define SYS_WRITE     4u   // a0 = path, a1 = buffer, a2 = length
#define SYS_CLOSE     5u   // a0 = path
#define SYS_MODDIR    6u   // a0 = index, a1 = buffer(12) -> a0 = links, -1 = end
#define SYS_MEMINFO   7u   // a0 = 0 largest free block, 1 processes -> a0 = value
#define SYS_READ      8u   // a0 = path, a1 = buf, a2 = length -> a0 = read, 0 = nothing
#define SYS_EXEC      9u   // a0 = module name, a1 = argument string -> a0 = pid
#define SYS_ARGS     10u   // a0 = buffer, a1 = length -> a0 = characters copied

#define MYRTOS_MEM_LARGEST_FREE 0u
#define MYRTOS_MEM_PROCESSES    1u

// The call itself. It is identical in every module, so it belongs here.
static inline int32_t myrtos_syscall(uint32_t id, uint32_t a, uint32_t b, uint32_t c) {
    register uint32_t r_id __asm__("a7") = id;
    register uint32_t r_a0 __asm__("a0") = a;
    register uint32_t r_a1 __asm__("a1") = b;
    register uint32_t r_a2 __asm__("a2") = c;

    __asm__ volatile (
        "ecall"
        : "+r"(r_a0)
        : "r"(r_id), "r"(r_a1), "r"(r_a2)
        : "memory"
    );
    return (int32_t)r_a0;
}

static inline int32_t myrtos_open(const char *device) {
    return myrtos_syscall(SYS_OPEN, (uint32_t)(uintptr_t)device, 0, 0);
}

static inline int32_t myrtos_write(int32_t path, const void *buf, uint32_t len) {
    return myrtos_syscall(SYS_WRITE, (uint32_t)path, (uint32_t)(uintptr_t)buf, len);
}

// Reads do not block: zero means nothing was there just now. The kernel has no
// way yet to sleep a process on a device, so whoever waits for input must ask
// again -- the scheduler reclaims the time regardless.
// The standard paths, the same convention as OS-9 and Unix. The kernel sets
// them up for the first process and every child inherits them, so a utility
// neither opens nor closes anything: it reads path 0 and writes path 1.
#define MYRTOS_STDIN  0
#define MYRTOS_STDOUT 1
#define MYRTOS_STDERR 2

// Inherited output if there is any, otherwise a console of our own. The
// fallback is only needed for a module started without a parent.
static inline int32_t myrtos_console(void) {
    if (myrtos_write(MYRTOS_STDOUT, "", 0) >= 0) return MYRTOS_STDOUT;
    int32_t p = myrtos_open("usb");
    if (p < 0) p = myrtos_open("term");
    return p;
}

static inline int32_t myrtos_read(int32_t path, void *buf, uint32_t len) {
    return myrtos_syscall(SYS_READ, (uint32_t)path, (uint32_t)(uintptr_t)buf, len);
}

// Start a module by name, with a command line. OS-9's F$Link then F$Fork.
static inline int32_t myrtos_exec(const char *module_name, const char *args) {
    return myrtos_syscall(SYS_EXEC, (uint32_t)(uintptr_t)module_name,
                          (uint32_t)(uintptr_t)args, 0);
}

// A module starts like main: argc in a0, argv in a1, and argv[0] is the
// module's own name. The kernel builds the vector in the process's memory
// before starting it.
//
//     void module_main(int argc, char **argv) { ... }
//
// A module that does not care declares module_main(void) as before.

// Fetch the raw command line. Kept for anyone who would rather parse it
// themselves than go through argv.
static inline int32_t myrtos_args(char *buf, uint32_t len) {
    return myrtos_syscall(SYS_ARGS, (uint32_t)(uintptr_t)buf, len, 0);
}

static inline int32_t myrtos_close(int32_t path) {
    return myrtos_syscall(SYS_CLOSE, (uint32_t)path, 0, 0);
}

static inline void myrtos_exit(void) {
    myrtos_syscall(SYS_EXIT, 0, 0, 0);
}

// Convenience: write a NUL-terminated string in ONE call. Sending the whole
// string rather than a character at a time is what makes the output atomic
// against other processes.
// --- FUNCTION TABLES IN RELOCATABLE CODE ----------------------------------
// An ordinary table of function pointers carries absolute addresses written in
// by the linker, and breaks position independence. Store the DISTANCE from the
// table to the function instead, and add the table's address at runtime, and
// the table becomes relocatable -- and can sit const in .rodata, hence shared
// between processes. A table built on the stack at runtime works too, but then
// every process gets its own copy.
//
// The difference has to be computed by the assembler: C rejects it as an
// initialiser, because the difference between two addresses is not a constant
// in the language's sense. The result is ADD32/SUB32 relocations, which are
// link-time constants carrying no absolute address.
//
// The functions must carry __attribute__((used)): they are referenced only
// from assembly, which the compiler cannot see, and are otherwise optimised
// away as unused.
//
//     __attribute__((used)) static void do_read(int a) { ... }
//     MYRTOS_RELTAB_BEGIN(ops);
//     MYRTOS_RELTAB_ENTRY(ops, do_read);
//     MYRTOS_RELTAB_ENTRY(ops, do_write);
//     MYRTOS_RELTAB_END();
//     ...
//     MYRTOS_RELTAB_CALL(ops, i, void (*)(int))(arg);

#define MYRTOS_RELTAB_BEGIN(name)                                  \
    extern const intptr_t name[];                                  \
    __asm__(".pushsection .rodata." #name ",\"a\"\n"               \
            ".balign 4\n.globl " #name "\n" #name ":")

#define MYRTOS_RELTAB_ENTRY(name, fn)                              \
    __asm__(".word " #fn " - " #name)

// .popsection is not optional: without it the section switch stays in effect
// and all following code lands in the table's section instead of .text.
#define MYRTOS_RELTAB_END()  __asm__(".popsection")

#define MYRTOS_RELTAB_CALL(name, index, type)                      \
    ((type)((intptr_t)(name) + (name)[index]))

// A write is atomic, but a LINE only is if it goes out in one call. If a
// utility builds its line from several writes, other processes get in between,
// and the output is unreadable as soon as more than one process speaks. Hence
// this: gather the line, send it once.
typedef struct {
    char buf[96];
    uint32_t len;
} myrtos_line_t;

static inline void myrtos_line_reset(myrtos_line_t *l) { l->len = 0; }

static inline void myrtos_line_str(myrtos_line_t *l, const char *s) {
    while (*s && l->len < sizeof(l->buf) - 1) l->buf[l->len++] = *s++;
}

static inline void myrtos_line_chars(myrtos_line_t *l, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n && l->len < sizeof(l->buf) - 1; i++) l->buf[l->len++] = s[i];
}

static inline void myrtos_line_u32(myrtos_line_t *l, uint32_t v) {
    char tmp[11];
    int i = 10;
    tmp[i] = 0;
    if (!v) tmp[--i] = '0';
    while (v) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    myrtos_line_str(l, &tmp[i]);
}

// Utilities need to print numbers, and a module has no printf. Ten lines here
// saves them in every utility.
static inline int32_t myrtos_write_u32(int32_t path, uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    return myrtos_write(path, &buf[i], (uint32_t)(10 - i));
}

static inline int32_t myrtos_moddir_get(uint32_t index, char *name_out) {
    return myrtos_syscall(SYS_MODDIR, index, (uint32_t)(uintptr_t)name_out, 0);
}

static inline int32_t myrtos_meminfo(uint32_t what) {
    return myrtos_syscall(SYS_MEMINFO, what, 0, 0);
}

static inline int32_t myrtos_line_flush(int32_t path, myrtos_line_t *l) {
    int32_t r = myrtos_write(path, l->buf, l->len);
    l->len = 0;
    return r;
}

static inline int32_t myrtos_write_str(int32_t path, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return myrtos_write(path, s, n);
}

#endif

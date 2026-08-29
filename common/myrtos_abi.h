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
#define SYS_FSDIR    11u   // a0 = index, a1 = name(12), a2 = &size -> a0 = attr, -1 = end
#define SYS_FSREAD   12u   // a0 = &myrtos_fs_io_t -> a0 = bytes read, 0 = eof
#define SYS_FSWRITE  13u   // a0 = &myrtos_fs_io_t -> a0 = bytes written
#define SYS_FSREMOVE 14u   // a0 = name -> a0 = 0 ok, -1 failed
#define SYS_WAIT     15u   // a0 = pid; returns when that process has exited
#define SYS_SLEEP    16u   // a0 = milliseconds; returns when they have passed
#define SYS_SETPRIO  17u   // a0 = new priority -> a0 = the old one
#define SYS_TICKS    18u   // -> a0 = milliseconds since the timer started
#define SYS_PSINFO   19u   // a0 = slot, a1 = &myrtos_psinfo_t -> a0 = 0, -1 empty
#define SYS_BOOTSEL  20u   // reboots into the bootloader; never returns
#define SYS_ALLOC    21u   // a0 = bytes -> a0 = pointer, 0 on failure
#define SYS_FREE     22u   // a0 = pointer -> a0 = 0, -1 if not ours
#define SYS_REALLOC  23u   // a0 = pointer, a1 = bytes -> a0 = pointer

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

// Writes take what the device can hold and report how much that was, as write
// does everywhere. The kernel blocks rather than returning zero, so this loop
// waits rather than spins.
static inline int32_t myrtos_write(int32_t path, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < len) {
        int32_t n = myrtos_syscall(SYS_WRITE, (uint32_t)path,
                                   (uint32_t)(uintptr_t)(p + done), len - done);
        if (n < 0) return n;
        done += (uint32_t)n;
    }
    return (int32_t)done;
}

// Reads block. A process waiting for input is taken off the run queue until the
// device has something, so waiting costs nothing rather than costing every
// quantum the scheduler will give it. Zero still comes back from a device that
// cannot say whether it has anything -- the send-only UART, for instance --
// because blocking on one of those would never end.
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

// --- FILESYSTEM -----------------------------------------------------------
// Read-only for now: the card can be listed and read, not written. Names are
// given as a person types them ("readme.txt"); the kernel pads them into the
// 8.3 form the directory stores.

#define MYRTOS_ATTR_DIRECTORY 0x10

// Enumerate the root directory. index starts at zero; -1 means no more.
static inline int32_t myrtos_fs_dir(uint32_t index, char *name_out, uint32_t *size_out) {
    return myrtos_syscall(SYS_FSDIR, index, (uint32_t)(uintptr_t)name_out,
                          (uint32_t)(uintptr_t)size_out);
}

// Four arguments do not fit in a0-a2, so the request travels as a struct. Reads
// and writes take the same one -- they differ in direction, not in shape -- and
// it leaves room to grow without disturbing the calling convention.
typedef struct {
    const char *name;
    uint32_t    offset;
    uint8_t    *buf;
    uint32_t    len;
} myrtos_fs_io_t;

// Read a slice of a file. Returns bytes read, 0 at end of file, -1 if missing.
static inline int32_t myrtos_fs_read(const char *name, uint32_t offset,
                                     void *buf, uint32_t len) {
    myrtos_fs_io_t r = { name, offset, (uint8_t*)buf, len };
    return myrtos_syscall(SYS_FSREAD, (uint32_t)(uintptr_t)&r, 0, 0);
}

// Write a slice, creating and extending the file as needed. Returns bytes
// written, or -1. There is no truncate: writing over a longer file leaves the
// tail behind, so a utility that replaces a file removes it first.
static inline int32_t myrtos_fs_write(const char *name, uint32_t offset,
                                      const void *buf, uint32_t len) {
    myrtos_fs_io_t r = { name, offset, (uint8_t*)(uintptr_t)buf, len };
    return myrtos_syscall(SYS_FSWRITE, (uint32_t)(uintptr_t)&r, 0, 0);
}

// Delete a file. Returns 0, or -1 if it is missing or is a directory.
static inline int32_t myrtos_fs_remove(const char *name) {
    return myrtos_syscall(SYS_FSREMOVE, (uint32_t)(uintptr_t)name, 0, 0);
}

// Wait for a process to exit. Returns at once if it already has, so there is no
// race between starting something and waiting for it.
static inline int32_t myrtos_wait(int32_t pid) {
    return myrtos_syscall(SYS_WAIT, (uint32_t)pid, 0, 0);
}

// Sleep for a length of time. The tick is a millisecond, so that is the unit.
// Zero yields: the process stays runnable but lets the next one go first.
static inline int32_t myrtos_sleep(uint32_t ms) {
    return myrtos_syscall(SYS_SLEEP, ms, 0, 0);
}

// Thirty-two levels. 0 belongs to the idle process and cannot be taken; 16 is
// what a process starts with. Strict priority: nothing below the highest ready
// level runs at all, so a process that neither blocks nor sleeps starves
// everything under it for as long as it holds the processor.
#define MYRTOS_PRIO_MAX     31
#define MYRTOS_PRIO_DEFAULT 16

// Set this process's priority, returning the previous one. Zero asks without
// changing anything: it is the idle process's level and cannot be taken, so it
// is free to mean something else.
static inline int32_t myrtos_setprio(uint32_t prio) {
    return myrtos_syscall(SYS_SETPRIO, prio, 0, 0);
}

static inline int32_t myrtos_getprio(void) {
    return myrtos_syscall(SYS_SETPRIO, 0, 0, 0);
}

// Milliseconds since the timer started. Wraps after 49 days; compare
// differences rather than absolute values and the wrap takes care of itself.
static inline uint32_t myrtos_ticks_now(void) {
    return (uint32_t)myrtos_syscall(SYS_TICKS, 0, 0, 0);
}

// What a process is doing. The states a reader cares about are the ones it can
// be stuck in, so they are named rather than numbered in any output.
#define MYRTOS_PS_FREE        0
#define MYRTOS_PS_READY       1
#define MYRTOS_PS_RUNNING     2
#define MYRTOS_PS_WAIT_READ   3
#define MYRTOS_PS_WAIT_WRITE  6
#define MYRTOS_PS_WAIT_CHILD  4
#define MYRTOS_PS_SLEEPING    5

typedef struct {
    uint32_t pid;
    uint32_t state;        // MYRTOS_PS_*
    uint32_t priority;
    uint32_t mem_size;     // data and stack together, as the header asked
    char     name[12];     // the module's, or a kernel thread's stand-in
} myrtos_psinfo_t;

// How many processes can exist, kernel included. This lived in three places --
// the scheduler's table, the I/O manager's path table, and here -- with nothing
// keeping them equal. Raising only the scheduler's would have given high pids no
// I/O at all: path_of would refuse every path number they asked for, and every
// read and write would fail without saying why.
//
// The cost is 88 bytes of kernel table per process, so the limit is set by what
// is useful rather than by what fits.
#define MYRTOS_MAX_PROCESSES 32

// Ask about one slot. Slots are not compacted, so walk from 0 to the limit and
// skip the ones that answer -1 rather than stopping at the first.
#define MYRTOS_PS_SLOTS MYRTOS_MAX_PROCESSES
static inline int32_t myrtos_psinfo(uint32_t slot, myrtos_psinfo_t *out) {
    return myrtos_syscall(SYS_PSINFO, slot, (uint32_t)(uintptr_t)out, 0);
}

// Reboot into the bootloader, so new firmware can be loaded without reaching
// for the board. Does not return.
static inline void myrtos_bootsel(void) {
    myrtos_syscall(SYS_BOOTSEL, 0, 0, 0);
}

// --- MEMORY ---------------------------------------------------------------
// Memory beyond the block the module header asked for. The kernel remembers
// which process each block belongs to, so nothing is lost when a process dies
// -- including one that dies without tidying up.
//
// Where the pointer lives matters. A module may not have writable statics, so
// `static void *buf;` is refused by the build. Keep it on the stack, or in the
// data area the module header reserved.
static inline void *myrtos_alloc(uint32_t size) {
    return (void*)(uintptr_t)myrtos_syscall(SYS_ALLOC, size, 0, 0);
}

// Returns 0, or -1 for a pointer this process was not given.
static inline int32_t myrtos_free(void *ptr) {
    return myrtos_syscall(SYS_FREE, (uint32_t)(uintptr_t)ptr, 0, 0);
}

// NULL leaves the old block untouched, so the caller has not lost it.
static inline void *myrtos_realloc(void *ptr, uint32_t size) {
    return (void*)(uintptr_t)myrtos_syscall(SYS_REALLOC,
                                            (uint32_t)(uintptr_t)ptr, size, 0);
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

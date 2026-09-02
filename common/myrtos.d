// The myrtos system interface, for modules written in D.
//
// D is here for one property: every module-level and static variable goes into
// thread-local storage unless it is marked __gshared. That is what a shareable
// module needs, and in D it is the default rather than something each
// declaration has to remember -- the C side gets there with __thread on every
// variable and check_module.py to catch the ones that forgot.
//
// This is the counterpart of common/myrtos_abi.h and follows it. The numbers
// are that file's; if one changes there it must change here, and there is
// nothing but this comment to enforce that.
//
// One deliberate difference: the C side calls a descriptor a `path`, which is
// what OS-9 called it -- a path number is exactly what Unix calls a file
// descriptor, and the kernel's tables are named for it throughout. Here it is
// `fd`, because `int path` reads as a string until you check the type, and
// myrtos_posix.h already uses `fd` for the same thing. They are the same
// number; only the spelling changes with which world you are standing in.
module myrtos;

import ldc.llvmasm;

@nogc: nothrow:

// The trap. a7 carries the call, a0 to a2 the arguments, a0 the result -- the
// same registers the C inline function uses, written in the constraint syntax
// LLVM wants. Verified by disassembly rather than by reading: li a7,N; ecall.
pragma(inline, true)
int syscall(uint id, uint a = 0, uint b = 0, uint c = 0) {
    return __asm!int("ecall", "={a0},{a7},{a0},{a1},{a2},~{memory}", id, a, b, c);
}

enum : uint {
    SYS_NULL = 0, SYS_IO_PUTC = 1, SYS_EXIT = 2, SYS_OPEN = 3, SYS_WRITE = 4,
    SYS_CLOSE = 5, SYS_MODDIR = 6, SYS_MEMINFO = 7, SYS_READ = 8, SYS_EXEC = 9,
    SYS_ARGS = 10, SYS_FSDIR = 11, SYS_FSREAD = 12, SYS_FSWRITE = 13,
    SYS_FSREMOVE = 14, SYS_WAIT = 15, SYS_SLEEP = 16, SYS_SETPRIO = 17,
    SYS_TICKS = 18, SYS_PSINFO = 19, SYS_BOOTSEL = 20, SYS_ALLOC = 21,
    SYS_FREE = 22, SYS_REALLOC = 23, SYS_READABLE = 39
}

enum int STDIN = 0, STDOUT = 1, STDERR = 2;

enum : uint { O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 4, O_TRUNC = 8, O_APPEND = 16 }

// Writes take what the device can hold and say how much that was, so this
// loops rather than assuming. The kernel blocks instead of returning zero, so
// the loop waits rather than spins. Same as myrtos_write in the C header.
int write(int fd, const(void)* buf, uint len) {
    auto p = cast(const(ubyte)*) buf;
    uint done = 0;
    while (done < len) {
        int n = syscall(SYS_WRITE, fd, cast(uint)(p + done), len - done);
        if (n < 0) return n;
        done += n;
    }
    return cast(int) done;
}

// A slice knows its own length, which is the one place D saves the caller from
// getting it wrong.
int write(int fd, const(char)[] s)    { return write(fd, s.ptr, cast(uint) s.length); }

int read(int fd, void* buf, uint len) { return syscall(SYS_READ, fd, cast(uint) buf, len); }
int read(int fd, ubyte[] buf)         { return read(fd, buf.ptr, cast(uint) buf.length); }

// Is there anything to read? read() blocks when there is not -- the caller goes
// on WAIT_READ and its ecall is re-executed when a byte turns up, which is what
// a loop with nothing else to do wants and exactly what a loop waiting for an
// answer that may never come does not. Ask first and a program can give up.
//
// Returns the bytes waiting, 0 for none, -1 for a descriptor that is not open.
int readable(int fd)                  { return syscall(SYS_READABLE, fd); }

// Names reach the kernel as C strings, so a D literal needs its terminator.
// String literals in D are NUL-terminated when they are used as const(char)*,
// which is why open takes the pointer rather than a slice.
int open(const(char)* name, uint flags = O_RDONLY) {
    return syscall(SYS_OPEN, cast(uint) name, flags);
}
int close(int fd)      { return syscall(SYS_CLOSE, fd); }
void exit(int code = 0)  { syscall(SYS_EXIT, code); }
int sleep(uint ms)       { return syscall(SYS_SLEEP, ms); }
uint ticks()             { return cast(uint) syscall(SYS_TICKS); }
void* alloc(uint bytes)  { return cast(void*) syscall(SYS_ALLOC, bytes); }
int free(void* p)        { return syscall(SYS_FREE, cast(uint) p); }

// The command line -- but only its first word. By the time a module runs, the
// kernel has split the stored argument string in place to build argv, and this
// copies up to the first NUL. Take argc and argv in module_main instead; this
// is here because the system call is.
int args(char[] buf) { return syscall(SYS_ARGS, cast(uint) buf.ptr, cast(uint) buf.length); }

// --- What a freestanding D module owes the compiler ------------------------
//
// These are not part of the system interface; they are the handful of symbols
// LDC emits calls to and expects someone else to define. On a hosted target
// they come from the C library, and there is not one here.

// Array bounds are still checked under -betterC, and a failed check calls this.
// Keeping it -- rather than building with --boundscheck=off -- means an
// out-of-range slice says so on the console and ends the process, instead of
// reading whatever was next in memory. That is worth more than the branch.
extern(C) void __assert(const(char)* msg, const(char)* file, int line) {
    write(STDERR, "D assertion failed: ");
    if (msg)  write(STDERR, msg, cast(uint) strlen(msg));
    write(STDERR, " in ");
    if (file) write(STDERR, file, cast(uint) strlen(file));
    write(STDERR, "\r\n");
    exit(1);
}

// Written out by hand and then called by name, because the compiler recognises
// the loop and emits a call to strlen whichever way it is spelled. Better to
// name it than to be surprised by it.
extern(C) size_t strlen(const(char)* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

// D zero-initialises a local array unless it is declared `= void`, and the
// compiler turns that into a memset call rather than a loop.
extern(C) void* memset(void* dst, int c, size_t n) {
    auto p = cast(ubyte*) dst;
    foreach (i; 0 .. n) p[i] = cast(ubyte) c;
    return dst;
}

extern(C) void* memcpy(void* dst, const(void)* src, size_t n) {
    auto d = cast(ubyte*) dst;
    auto s = cast(const(ubyte)*) src;
    foreach (i; 0 .. n) d[i] = s[i];
    return dst;
}

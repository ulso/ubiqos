//! The myrtos system call interface for Zig, the same one common/myrtos.d gives
//! D and common/myrtos_abi.h gives C.
//!
//! Why Zig is here at all: a shareable module may have no writable statics, and
//! Zig's `threadlocal var` gives exactly the relocations that needs --
//! TPREL_HI20, TPREL_ADD, TPREL_LO12_I -- with no flag to remember. Its default
//! for a freestanding target is already local exec, where LDC needs
//! --fthread-model=local-exec and clang needs __thread on each declaration.
//!
//! What Zig does NOT have is a way to put every variable there. A plain `var`
//! at module scope lands in .data and is shared by every process running the
//! module, which is the thing check_module.py refuses. D is still the only
//! language here whose default is what this system wants; Zig is C's discipline
//! with better habits, since a module-scope `var` stands out in Zig where a
//! static does not in C.

const builtin = @import("builtin");

/// The trap. The number carries the call, three registers the arguments, and
/// the first of them comes back holding the result.
///
/// Two machines, one shape. RISC-V puts the number in a7 and the arguments in
/// a0 to a2; ARM puts it in r7 and the arguments in r0 to r2, which is where
/// Linux's ARM EABI has always put them. The condition is comptime-known, so
/// only the branch for the machine being built is ever analysed -- the other
/// one's register names would not even parse here.
///
/// r7 rather than r12 on ARM: r12 belongs to the linker's veneers, and r7 is
/// the frame pointer only when there is one, which at ReleaseSmall there is not.
pub fn syscall(id: u32, a0: u32, a1: u32, a2: u32) i32 {
    if (builtin.cpu.arch.isArm() or builtin.cpu.arch.isThumb()) {
        return asm volatile ("svc 0"
            : [ret] "={r0}" (-> i32),
            : [id] "{r7}" (id),
              [arg0] "{r0}" (a0),
              [arg1] "{r1}" (a1),
              [arg2] "{r2}" (a2),
            : .{ .memory = true });
    } else {
        return asm volatile ("ecall"
            : [ret] "={a0}" (-> i32),
            : [id] "{a7}" (id),
              [arg0] "{a0}" (a0),
              [arg1] "{a1}" (a1),
              [arg2] "{a2}" (a2),
            : .{ .memory = true });
    }
}

pub const SYS_IO_PUTC: u32 = 1;
pub const SYS_EXIT: u32 = 2;
pub const SYS_OPEN: u32 = 3;
pub const SYS_WRITE: u32 = 4;
pub const SYS_READ: u32 = 5;
pub const SYS_CLOSE: u32 = 6;
pub const SYS_ARGS: u32 = 10;
pub const SYS_RANDOM: u32 = 56;

pub const STDIN: i32 = 0;
pub const STDOUT: i32 = 1;
pub const STDERR: i32 = 2;

/// A short write can happen, so this loops rather than trusting one call.
pub fn write(fd: i32, buf: []const u8) i32 {
    var done: usize = 0;
    while (done < buf.len) {
        const n = syscall(SYS_WRITE, @bitCast(fd), @intFromPtr(buf.ptr) + done, @intCast(buf.len - done));
        if (n <= 0) return if (done == 0) n else @intCast(done);
        done += @intCast(n);
    }
    return @intCast(done);
}

pub fn writeStr(fd: i32, s: []const u8) void {
    _ = write(fd, s);
}

pub fn read(fd: i32, buf: []u8) i32 {
    return syscall(SYS_READ, @bitCast(fd), @intFromPtr(buf.ptr), @intCast(buf.len));
}

pub fn random(buf: []u8) i32 {
    return syscall(SYS_RANDOM, @intFromPtr(buf.ptr), @intCast(buf.len), 0);
}

pub fn exit(code: i32) noreturn {
    _ = syscall(SYS_EXIT, @bitCast(code), 0, 0);
    unreachable;
}

/// Decimal, without a libc. Returns the slice of `out` that was used.
pub fn u32ToDec(v: u32, out: []u8) []const u8 {
    if (v == 0) { out[0] = '0'; return out[0..1]; }
    var tmp: [10]u8 = undefined;
    var n: usize = 0;
    var x = v;
    while (x != 0) : (x /= 10) { tmp[n] = '0' + @as(u8, @intCast(x % 10)); n += 1; }
    var i: usize = 0;
    while (i < n) : (i += 1) out[i] = tmp[n - 1 - i];
    return out[0..n];
}

// --- WHAT THE COMPILER ASSUMES IS THERE -----------------------------------
// Zig recognises hand-written loops and replaces them with calls to the C
// library: the first build of zhello failed on an undefined `strlen`, from a
// loop that never mentioned it. clang does the same, which is what -fno-builtin
// is for in the wasm examples -- but a freestanding module has no libc to fall
// back on, so the names are simply provided here. common/myrtos.d carries the
// same three for LDC.
export fn strlen(s: [*:0]const u8) usize {
    var n: usize = 0;
    while (s[n] != 0) n += 1;
    return n;
}

export fn memcpy(dst: [*]u8, src: [*]const u8, n: usize) [*]u8 {
    var i: usize = 0;
    while (i < n) : (i += 1) dst[i] = src[i];
    return dst;
}

export fn memset(dst: [*]u8, c: c_int, n: usize) [*]u8 {
    var i: usize = 0;
    while (i < n) : (i += 1) dst[i] = @intCast(c & 0xff);
    return dst;
}

export fn memmove(dst: [*]u8, src: [*]const u8, n: usize) [*]u8 {
    if (@intFromPtr(dst) < @intFromPtr(src)) {
        var i: usize = 0;
        while (i < n) : (i += 1) dst[i] = src[i];
    } else {
        var i: usize = n;
        while (i > 0) { i -= 1; dst[i] = src[i]; }
    }
    return dst;
}

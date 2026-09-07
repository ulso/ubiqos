// A myrtos module in Rust, natively.
//
// Rust reaches this machine three ways now: as wasm through the wasm host, as
// a native module like this one, and not at all through Embassy -- see the
// note at the bottom for why that last one is a category difference rather
// than a missing feature.
//
// What a native Rust module has to satisfy is what every module satisfies, and
// two of the three are flags:
//
//   -C relocation-model=static      no PIC; the loader relocates instead
//   -C target-feature=+no-movt      addresses as literal pool words, which the
//                                   loader can fix -- rustc is LLVM and would
//                                   otherwise split them across movw/movt, the
//                                   same wall clang hit on ARM
//   -C target-feature=+reserve-r9   r9 carries the thread pointer, which this
//                                   machine has nowhere else
//
// The third is below: the syscall cannot be written as inline asm.
#![no_std]
#![no_main]

use core::arch::global_asm;

// The syscall, and it has to be assembly rather than inline asm.
//
// myrtos puts the call number in r7 -- where Linux's ARM EABI has always put it
// -- and rustc refuses r7 as an inline-asm operand: it reserves it as the frame
// pointer on thumb targets and says so outright. That is not a flag away, so
// the stub is written out, which is what common/arm/tp.S already does for the
// thread pointer in C modules.
//
// AAPCS puts the four arguments in r0 to r3; myrtos wants the number in r7 and
// the arguments in r0 to r2, so they shuffle down by one. r7 is saved and
// restored because it belongs to the caller.
// One per machine, because the stub is the one part of this that cannot be
// written once. Both put the call number where myrtos wants it and shuffle the
// arguments down by one, which is the whole of the difference between the C
// calling convention and this system call.
#[cfg(target_arch = "arm")]
global_asm!(
    ".global myrtos_syscall",
    ".thumb_func",
    "myrtos_syscall:",
    "   push {{r7, lr}}",
    "   mov r7, r0",
    "   mov r0, r1",
    "   mov r1, r2",
    "   mov r2, r3",
    "   svc 0",
    "   pop {{r7, pc}}",
);

// RISC-V needs no saving: a7 and a0 to a3 are all caller-saved here, and the
// number goes in a7 the way Linux has always put it.
#[cfg(target_arch = "riscv32")]
global_asm!(
    ".global myrtos_syscall",
    "myrtos_syscall:",
    "   mv a7, a0",
    "   mv a0, a1",
    "   mv a1, a2",
    "   mv a2, a3",
    "   ecall",
    "   ret",
);

unsafe extern "C" {
    fn myrtos_syscall(id: u32, a: u32, b: u32, c: u32) -> i32;
}

unsafe fn syscall(id: u32, a: u32, b: u32, c: u32) -> i32 {
    unsafe { myrtos_syscall(id, a, b, c) }
}

const SYS_WRITE: u32 = 4;
const STDOUT: u32 = 1;

fn write_str(s: &str) {
    unsafe { syscall(SYS_WRITE, STDOUT, s.as_ptr() as u32, s.len() as u32) };
}

#[unsafe(no_mangle)]
pub extern "C" fn module_main(_argc: i32, _argv: *const *const u8) {
    write_str("hello from Rust, natively\r\n");
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

// --- WHY NOT EMBASSY -------------------------------------------------------
// Embassy is not a library a module can call; it is a runtime that owns the
// machine. embassy-rp takes the RP2350's peripherals and configures clocks,
// DMA and interrupts; embassy-time binds a hardware timer; embassy-executor
// wakes tasks from interrupt context; embassy-net is a TCP/IP stack.
//
// A myrtos module has none of that and is not supposed to: it has no
// peripheral access, no interrupts of its own, and no need of a TCP/IP stack
// because the ESP32-C6 carries one. So the pico-io-bridge web server's 1300
// lines of http.rs cannot come across -- every I/O call in it is an
// embassy_net::tcp::TcpSocket.
//
// What CAN come across is the part that has nothing to do with Embassy: the
// pages themselves, which are ordinary HTML and are already separate files.

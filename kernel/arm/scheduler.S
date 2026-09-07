// Trap vector for myrtos on Cortex-M33, the ARM half of kernel/scheduler.S.
//
// The contract is the same one and is worth stating before the differences:
// the whole context is saved on the interrupted process's own stack, the
// handler is given the address of that frame, and it answers with the stack
// pointer to resume from. The same pointer means the same process continues; a
// different one means the next process takes over. That is the entire context
// switch, here as there.
//
// What ARM does differently is that half the frame is not ours to save. On
// exception entry the core has already pushed r0-r3, r12, lr, pc and xPSR onto
// the stack that was in use, and it will pop them again on return. So the
// software frame is r4-r11 and nothing else -- pushed immediately below the
// hardware frame, so that the two together are one contiguous structure and a
// frame pointer means what it means on the other side.
//
// Two more things have no RISC-V counterpart:
//
//   EXC_RETURN. On entry lr does not hold a return address but a code saying
//   which stack to return to and whether floating-point state was stacked. It
//   is destroyed by the call to the handler, so it is saved in the frame like
//   any other register and put back before the return.
//
//   The cause. There is no mcause; the exception number is in IPSR, and the
//   detail is in CFSR and its friends, which the C side reads directly. IPSR
//   is stored where mcause sits on the other side so that the handler has one
//   question to ask.
//
// We do not own the vector table here either. The SDK's crt0 declares every
// entry weak, and these definitions replace the four that matter.

.syntax unified
.cpu cortex-m33
// Named for the assembler's sake. .cpu alone leaves it without an FPU and the
// d8-d15 save below will not assemble. FPv5-SP-D16 is what a Cortex-M33 has:
// single precision, sixteen double-width registers -- and d8 to d15 are s16 to
// s31 under another name, which is the half of the register file nothing else
// saves.
.fpu fpv5-sp-d16
.thumb

.section .text
.global myrtos_trap_vector
.global isr_svcall
.global isr_pendsv
.global isr_systick
.global isr_hardfault
.align 2

// One body for all of them: IPSR says which arrived, and it is decoded in the
// handler, exactly as mcause is on RISC-V.
//
// One .thumb_func per label, and it is not decoration. The directive marks the
// NEXT symbol only, so a single one in front of five aliases set the Thumb bit
// on isr_svcall and left the other four even. The vector table is where that
// shows: an even entry means ARM state, ARMv8-M has none, so the core takes an
// INVSTATE UsageFault -- which escalates to HardFault, whose vector was equally
// even. The first SysTick tick would have locked the machine up with nothing
// printed and nothing to read. Caught by dumping the table out of the binary
// before ever flashing it.
.thumb_func
isr_svcall:
.thumb_func
isr_pendsv:
.thumb_func
isr_systick:
.thumb_func
isr_hardfault:
.thumb_func
myrtos_trap_vector:
    // The process stack, which is where the hardware frame already is. A
    // process always runs on PSP; only these handlers run on MSP.
    mrs     r0, psp

    // IPSR into a register that is already saved by the hardware, so nothing
    // is lost by using it as a scratch.
    mrs     r1, ipsr

    // Descending, and in register order: r1 lands lowest, lr highest. The
    // frame therefore reads cause, r4..r11, exc_return, and then the hardware
    // frame the core pushed.
    stmdb   r0!, {r1, r4-r11, lr}

    // --- THE OTHER HALF OF THE CONTEXT ------------------------------------
    // s16 to s31, which nothing else saves.
    //
    // The FPU is on -- the SDK enables CP10 unconditionally and this port does
    // not opt out -- and s0 to s15 are handled by the core, which pushes them
    // in an extended exception frame and pops them again. That works here only
    // because EXC_RETURN is preserved per process above; it names the frame's
    // shape, and getting it right for the stack pointer got it right for the
    // floating-point state as well, by luck rather than by design.
    //
    // s16 to s31 are callee-saved and never automatically stacked. AAPCS gets
    // them across a CALL: a function that uses d8 pushes it on entry. It does
    // not get them across a PREEMPTION -- interrupt a process between its vpush
    // and its vpop and the next process's use of d8 overwrites a live value.
    //
    // And they are used, by code that has nothing to do with arithmetic: gcc
    // moves eight-byte blocks through d8 when an FPU is available, so
    // sd_init, wifi_thread and fat32lib all have a vpush {d8} in them. Two of
    // those are different processes that preempt each other.
    //
    // FType is bit 4 of EXC_RETURN and is CLEAR when the core pushed an
    // extended frame, which is exactly when this process has live
    // floating-point context. So a process that has never touched the FPU pays
    // nothing, which is nearly all of them.
    //
    // Written 64 bytes BELOW the frame rather than pushed, so that
    // myrtos_frame_t stays one contiguous structure with the hardware's half
    // and r0 keeps pointing at it. That space is below the process's live
    // stack and nothing else can reach it: handlers run on MSP, and the
    // process is not running. It does mean a frame's worst case is 136 bytes
    // rather than 72, which the smallest stack here -- the console thread's
    // 2 kB -- has room for many times over.
    tst     lr, #0x10
    bne     1f
    sub     r1, r0, #64
    vstmia  r1, {d8-d15}
1:

    // The frame is the argument, and the answer is the frame to resume.
    bl      myrtos_trap_handler

    // r0 now points at the frame to return through -- the same one, or another
    // process's. Its own EXC_RETURN says whether it has a block to restore,
    // and it is read out of the frame rather than out of lr: lr belongs to
    // whoever we are leaving, and this belongs to whoever we are going to.
    ldr     r1, [r0, #36]
    tst     r1, #0x10
    bne     2f
    sub     r1, r0, #64
    vldmia  r1, {d8-d15}
2:

    // Undo the push in the same order.
    ldmia   r0!, {r1, r4-r11, lr}
    msr     psp, r0

    // Back to whichever stack and mode EXC_RETURN names. The core pops the
    // hardware frame on the way.
    bx      lr

myrtos_trap_vector_end:

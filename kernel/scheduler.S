// Trap vector for UbiqOS on RISC-V (Hazard3, machine mode).
//
// We do NOT own mtvec. The Pico SDK's crt0 installs a vector table whose
// entries are weak symbols, meant to be replaced -- and we replace two of
// them: the exception vector and the machine timer. External interrupts are
// left to the SDK, which already decodes MEINEXT and dispatches through its
// soft vector table. That table is where TinyUSB registers itself, with
// irq_add_shared_handler.
//
// Taking over mtvec instead worked for our own timer but made the SDK's
// interrupt registration impossible, and TinyUSB asserted in dcd_init.
//
// The whole register file is saved on the interrupted process's own stack.
// The frame IS the saved context: the handler is given its address
// and answers with the stack pointer to resume. If it answers with a different
// one we have switched process, and the restore below picks up that process's
// register values instead. That is the entire context switch.

.section .text
.global ubiqos_trap_vector
.global isr_riscv_machine_exception
.global isr_riscv_machine_timer
.align 4

#define FRAME_SIZE 144

// Same body for both: mcause says what happened, and it is decoded in
// hanteraren.
isr_riscv_machine_exception:
isr_riscv_machine_timer:
ubiqos_trap_vector:
    addi    sp, sp, -FRAME_SIZE

    sw      ra,   0(sp)
    sw      gp,   4(sp)
    sw      tp,   8(sp)
    sw      t0,  12(sp)
    sw      t1,  16(sp)
    sw      t2,  20(sp)
    sw      s0,  24(sp)
    sw      s1,  28(sp)
    sw      a0,  32(sp)
    sw      a1,  36(sp)
    sw      a2,  40(sp)
    sw      a3,  44(sp)
    sw      a4,  48(sp)
    sw      a5,  52(sp)
    sw      a6,  56(sp)
    sw      a7,  60(sp)
    sw      s2,  64(sp)
    sw      s3,  68(sp)
    sw      s4,  72(sp)
    sw      s5,  76(sp)
    sw      s6,  80(sp)
    sw      s7,  84(sp)
    sw      s8,  88(sp)
    sw      s9,  92(sp)
    sw      s10, 96(sp)
    sw      s11,100(sp)
    sw      t3, 104(sp)
    sw      t4, 108(sp)
    sw      t5, 112(sp)
    sw      t6, 116(sp)

    csrr    t0, mepc
    sw      t0, 120(sp)
    csrr    t0, mcause
    sw      t0, 124(sp)
    csrr    t0, mtval
    sw      t0, 128(sp)

    mv      a0, sp
    call    ubiqos_trap_handler
// The return value is the stack to resume. The same pointer means we continue
// in the same process; a different one means the next process takes over.
    mv      sp, a0

    lw      t0, 120(sp)
    csrw    mepc, t0

    lw      ra,   0(sp)
    lw      gp,   4(sp)
    lw      tp,   8(sp)
    lw      t0,  12(sp)
    lw      t1,  16(sp)
    lw      t2,  20(sp)
    lw      s0,  24(sp)
    lw      s1,  28(sp)
    lw      a0,  32(sp)
    lw      a1,  36(sp)
    lw      a2,  40(sp)
    lw      a3,  44(sp)
    lw      a4,  48(sp)
    lw      a5,  52(sp)
    lw      a6,  56(sp)
    lw      a7,  60(sp)
    lw      s2,  64(sp)
    lw      s3,  68(sp)
    lw      s4,  72(sp)
    lw      s5,  76(sp)
    lw      s6,  80(sp)
    lw      s7,  84(sp)
    lw      s8,  88(sp)
    lw      s9,  92(sp)
    lw      s10, 96(sp)
    lw      s11,100(sp)
    lw      t3, 104(sp)
    lw      t4, 108(sp)
    lw      t5, 112(sp)
    lw      t6, 116(sp)

    addi    sp, sp, FRAME_SIZE
    mret

ubiqos_trap_vector_end:

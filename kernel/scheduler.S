// Trap-vektor för myrtos på RISC-V (Hazard3, machine mode).
//
// Vi äger INTE mtvec. Pico SDK:ns crt0 lägger dit en vektortabell vars poster
// är svaga symboler, avsedda att ersättas -- och vi ersätter två av dem:
// undantagsvektorn och maskintimern. Externa avbrott lämnas åt SDK:n, som
// redan avkodar MEINEXT och dispatchar genom sin mjuka vektortabell. Det är
// den tabellen TinyUSB registrerar sig i med irq_add_shared_handler.
//
// Att i stället ta över mtvec fungerade för vår egen timer men gjorde SDK:ns
// avbrottsregistrering omöjlig, och TinyUSB assertade i dcd_init.
//
// Hela registerfilen sparas på den avbrutna processens egen stack. Ramen ÄR
// det sparade sammanhanget: hanteraren får dess adress och svarar med den
// stackpekare som ska återupptas. Svarar den med en annan har vi bytt process,
// och återställningen nedan plockar upp den processens registervärden i
// stället. Det är hela kontextväxlingen.

.section .text
.global myrtos_trap_vector
.global isr_riscv_machine_exception
.global isr_riscv_machine_timer
.align 4

#define FRAME_SIZE 144

// Samma kropp för båda: mcause avgör vad som hänt, och det avkodas ändå i
// hanteraren.
isr_riscv_machine_exception:
isr_riscv_machine_timer:
myrtos_trap_vector:
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
    call    myrtos_trap_handler
    // Returvärdet är den stack som ska återupptas. Samma pekare betyder att vi
    // fortsätter i samma process; en annan betyder att nästa process tar vid.
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

myrtos_trap_vector_end:

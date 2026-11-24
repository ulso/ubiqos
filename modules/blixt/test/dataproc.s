@ Data processing: every form where a 16-bit or a 32-bit encoding is chosen.
start:
    movs r0, #1
    movs r7, #255
    mov r0, #1
    mov r8, #1
    mov r0, #0x1234
    mov r0, #0xff00
    mov r1, #0x00ab00ab
    mov r2, #0xabababab
    mov r3, #0xab00ab00
    mov r4, #-1
    mov r5, #0xfffffffe
    movs r0, #256
    mov r0, r1
    mov r8, r9
    mov sp, r0
    movs r0, r1
    movs r8, r1
    mov.w r0, r1
    mov r0, r1, lsl #2
    movs r0, r1, lsl #2
    mov r0, r1, asr #32
    mov r0, r0, lsl r1
    movs r0, r0, lsl r1
    mvn r0, #0
    mvns r0, r1
    mvn r0, r1
    mvn r0, r1, lsl #3
    movw r0, #0xffff
    movt r0, #0x1234
    adds r0, r1, r2
    add r0, r1, r2
    add r0, r0, r1
    add r1, r0, r1
    add r0, r1
    add r8, r1
    add r0, r8
    adds r8, r8, r1
    add sp, r0
    adds r0, r1, #1
    adds r0, r0, #1
    adds r0, #200
    add r0, r1, #1
    add r0, r0, #1
    adds r0, r1, #8
    add r0, r1, #255
    add r0, r1, #256
    add r0, r1, #4095
    add r0, r1, #-4
    adds r0, r1, #-4
    add sp, sp, #8
    add sp, #508
    add sp, sp, #512
    sub sp, sp, #8
    sub sp, #16
    add r0, sp, #4
    add r0, sp, #1020
    add r0, sp, #1024
    add r8, sp, #4
    add r0, r1, r2, lsl #3
    adds.w r0, r1, r2
    subs r0, r1, r2
    sub r0, r1, r2
    sub r0, r0, r1
    subs r0, r1, #7
    subs r0, #255
    sub r0, r1, #1
    sub r0, r1, #0x1000
    subw r0, r1, #0x123
    addw r0, r1, #0x123
    adcs r0, r1
    adcs r0, r0, r1
    adcs r0, r1, r0
    adc r0, r1
    adc r0, r1, #1
    adc r0, r1, #-2
    sbcs r0, r1
    sbc r0, r1, r2
    sbc r0, r1, #0xff
    rsbs r0, r1, #0
    rsb r0, r1, #0
    rsb r0, r1, #10
    rsb r0, r1, r2
    rsb r0, r1, r2, lsl #1
    negs r0, r1
    neg r0, r1
    ands r0, r1
    ands r0, r1, r0
    and r0, r1
    and r0, r1, r2
    and r0, r1, #0xff
    and r0, r1, #0xffffff00
    orrs r0, r1
    orr r0, r1, #0x100
    orr r0, r1, #0xffffff00
    eors r3, r4
    eor r0, r1, r2, ror #4
    eor r0, r1, #0x80000000
    bics r0, r1
    bic r0, r1, #3
    bic r0, r1, #0xfffffffc
    orn r0, r1, r2
    orn r0, r1, #1
    cmp r0, #0
    cmp r0, #255
    cmp r0, #256
    cmp r8, #1
    cmp r0, #-1
    cmp r0, r1
    cmp r8, r1
    cmp r0, r8
    cmp r0, r1, lsl #2
    cmp.w r0, r1
    cmn r0, r1
    cmn r0, #1
    cmn r0, #-1
    tst r0, r1
    tst r0, #1
    tst r8, r9
    teq r0, r1
    teq r0, #0x100
    lsls r0, r1, #1
    lsls r0, r1, #31
    lsrs r0, r1, #32
    asrs r0, r1, #1
    lsl r0, r1, #1
    lsr r8, r1, #1
    asr r0, r1, #32
    rors r0, r1, #3
    ror r0, r1, #31
    lsls r0, r1
    lsls r0, r0, r1
    lsl r0, r0, r1
    lsl r0, r1, r2
    asrs r2, r3
    rors r2, r3
    ror r0, r1, r2
    rrx r0, r1
    rrxs r0, r1
    muls r0, r1, r0
    muls r0, r0, r1
    muls r0, r1
    mul r0, r1, r2
    mul r0, r1, r0
    mla r0, r1, r2, r3
    mls r0, r1, r2, r3
    sdiv r0, r1, r2
    udiv r0, r1, r2
    udiv r0, r1
    umull r0, r1, r2, r3
    smull r0, r1, r2, r3
    umlal r0, r1, r2, r3
    smlal r0, r1, r2, r3
    uxtb r0, r1
    uxth r0, r1
    sxtb r0, r1
    sxth r0, r1
    uxtb r8, r1
    uxtb r0, r1, ror #8
    sxth r0, r1, ror #16
    rev r0, r1
    rev16 r0, r1
    revsh r0, r1
    rev r8, r9
    rbit r0, r1
    clz r0, r1
    bfi r0, r1, #4, #8
    bfc r0, #0, #16
    ubfx r0, r1, #3, #5
    sbfx r0, r1, #0, #32
    nop
    nop.w
    wfi
    wfe
    sev
    yield
    dsb
    dmb sy
    isb
    svc #0
    svc 255
    bkpt #3
    udf #0
    cpsid i
    cpsie i
    mrs r0, primask
    msr basepri, r1
    mrs r0, apsr

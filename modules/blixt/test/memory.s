@ Loads, stores, multiple registers.
    ldr r0, [r1]
    ldr r0, [r1, #4]
    ldr r0, [r1, #124]
    ldr r0, [r1, #128]
    ldr r0, [r1, #4095]
    ldr r0, [r1, #-4]
    ldr r0, [r1, #-255]
    ldr r0, [r1, #4]!
    ldr r0, [r1, #-4]!
    ldr r0, [r1], #4
    ldr r0, [r1], #-4
    ldr r8, [r1]
    ldr r0, [r8, #4]
    ldr r0, [sp]
    ldr r0, [sp, #1020]
    ldr r0, [sp, #1024]
    ldr r0, [r1, r2]
    ldr r0, [r1, r2, lsl #2]
    ldr r8, [r1, r2]
    str r0, [r1]
    str r0, [r1, #8]
    str r0, [sp, #8]
    str r0, [r1, #-8]
    str r0, [r1, #8]!
    str r0, [r1], #8
    str r0, [r1, r2]
    str r0, [r1, r2, lsl #1]
    ldrb r0, [r1]
    ldrb r0, [r1, #31]
    ldrb r0, [r1, #32]
    ldrb r0, [r1, r2]
    ldrb r0, [r1, #-1]
    strb r0, [r1, #3]
    strb r0, [r1, r2]
    strb r0, [r1], #1
    ldrh r0, [r1, #2]
    ldrh r0, [r1, #62]
    ldrh r0, [r1, #64]
    ldrh r0, [r1, #1]
    ldrh r0, [r1, r2]
    strh r0, [r1, #2]
    strh r0, [r1, r2]
    ldrsb r0, [r1, r2]
    ldrsb r0, [r1, #1]
    ldrsh r0, [r1, r2]
    ldrsh r0, [r1, #2]
    ldrsh r0, [r1, #-2]
    ldrd r0, r1, [r2]
    ldrd r0, r1, [r2, #8]
    ldrd r0, r1, [r2, #-8]
    ldrd r0, r1, [r2, #8]!
    ldrd r0, r1, [r2], #8
    strd r0, r1, [sp, #16]
    ldrex r0, [r1]
    ldrex r0, [r1, #4]
    strex r2, r0, [r1]
    push {r0}
    push {r4-r7, lr}
    push {r4, r8}
    push {r8}
    push {lr}
    pop {r0}
    pop {r4-r7, pc}
    pop {r4, r8}
    pop {r8}
    pop {pc}
    ldmia r0!, {r1, r2}
    ldmia r0, {r0, r1}
    ldm r0!, {r1-r3}
    ldmia r0, {r1, r2}
    ldmia r8!, {r1, r2}
    ldmia sp!, {r4, pc}
    ldmdb r0, {r1, r2}
    ldmdb r0!, {r1, r2}
    stmia r0!, {r1, r2}
    stm r0!, {r1}
    stmia r0, {r1, r2}
    stmia r0!, {r1, r8}
    stmdb sp!, {r4, lr}
    stmdb r0!, {r1, r2}
    stmfd sp!, {r8, r9}

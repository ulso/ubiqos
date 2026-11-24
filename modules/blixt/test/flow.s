@ Branches, IT blocks, literal pools, labels and data.
    .equ COUNT, 10
    .equ BIG, 0x12345678
SIZE = 8
top:
    b top
    b near
    beq near
    bne top
    bl func
    bx lr
    blx r3
    cbz r0, near
    cbnz r7, near
    adr r0, near
    adr r1, top
near:
    nop
    it eq
    moveq r0, #1
    ite ne
    movne r0, #2
    moveq r0, #3
    itt gt
    addgt r0, r0, r1
    addsgt r0, r1, r2
    itete lt
    movlt r0, #1
    movge r0, #2
    movlt r0, #3
    movge r0, #4
    it hi
    bhi top
    it ls
    ldrls r0, [r1]
    itt cs
    movcs r0, r1
    lslcs r0, r0, #2
    it lo
    movslo r0, #1
    it pl
    addpl r8, r8, r1
    ldr r0, =BIG
    ldr r1, =0x87654321
    ldr r2, =BIG
    ldr r3, =1
    ldr r4, =300
    ldr r5, =0xff000000
    ldr r6, =0xffffff00
    ldr r7, =0xabcd
    ldr r8, =COUNT
    ldr r0, word1
    ldr r9, word1
    mov r0, #COUNT * 2 + 1
    mov r1, #(1 << 8) | 3
    mov r2, #SIZE
    mov r3, #'A'
    mov r4, #end - top
    tbb [r0, r1]
    tbh [r0, r1, lsl #1]
func:
    push {r4, lr}
    movs r4, #0
1:
    adds r4, #1
    cmp r4, #COUNT
    blt 1b
    bgt 2f
2:
    pop {r4, pc}
    .ltorg
    .align 2
word1:
    .word 0x11223344, COUNT, end - top
    .hword 0x5566, -1
    .byte 1, 2, 3, 0xff, -1
    .ascii "Hi\n"
    .asciz "Blixt\t\"q\"\x41\101"
    .align 1
    .byte 9
    .align 2
    .space 6
    .space 3, 0xee
    .balign 4
    .align 3, 0
far:
    .space 300
    b.w top
    beq.w top
    beq far
    bl far2
    adr.w r0, far
    ldr r0, =0x99999999
    adds r0, r1
    .ltorg
    .space 2100
far2:
    b far
    beq top
    ldr r0, =0xaaaaaaaa
end:

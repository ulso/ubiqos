@ .include, with a file that includes another, labels across the boundary,
@ and the same file twice in a row would define its labels twice, so once.
        .include "inc/defs.s"
main:   push    {lr}
        adr     r1, msg
        movs    r2, #4
        bl      putstr
        bl      helper
        beq     putstr
        movs    r7, #SYS_EXIT
        pop     {pc}
        .align  2
msg:    .ascii  "Hej\n"
        ldr     r0, =0x12345678

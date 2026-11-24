@ Included by include.s: constants, a routine, and a nested include.
        .equ    SYS_WRITE, 4
        .equ    STDOUT, 1
        .include "inc/more.s"
putstr:                         @ r1 = string, r2 = length
        push    {r7, lr}
        movs    r0, #STDOUT
        movs    r7, #SYS_WRITE
        svc     0
        pop     {r7, pc}
1:      b       1b

// The guest provides 256 bytes. Preserve ARMv7 AAPCS integer and VFP callee-saved registers.
.syntax unified
.arch armv7-a
.fpu neon
.arm
.text
.align 2
.global oy_setjmp
.type oy_setjmp, %function
oy_setjmp:
    stmia r0!, {r4-r11, sp, lr}
    vstmia r0, {d8-d15}
    mov r0, #0
    bx lr
.size oy_setjmp, .-oy_setjmp

.global oy_longjmp
.type oy_longjmp, %function
oy_longjmp:
    ldmia r0!, {r4-r11, sp, lr}
    vldmia r0, {d8-d15}
    cmp r1, #0
    moveq r1, #1
    mov r0, r1
    bx lr
.size oy_longjmp, .-oy_longjmp
.section .note.GNU-stack,"",%progbits

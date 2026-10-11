/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* RAM blocks that reach the end of a used 64 KiB code chunk. */

.syntax unified
.cpu arm7tdmi
.section .text
.arm
.global _start

.equ BIG_OPS, 230
.equ FILL_OPS, 20
.equ FILL_COUNT, 7
.equ FILL_STRIDE, 0x400
.equ THUMB_OPS, 128
.equ THUMB_COUNT, 96
.equ THUMB_STRIDE, 0x400
.equ BIG_AT, 0x02000000
.equ FILL_AT, 0x02008000
.equ THUMB_AT, 0x02010000
.equ DATA_AT, 0x02030000

_start:
    b main
    .space 0xbc
main:
    ldr sp, =0x03007f00
    ldr r1, =0x04000000
    ldr r2, =0x00000403
    strh r2, [r1]

    /* Every load reads 1. */
    ldr r4, =DATA_AT
    mov r0, #1
    mov r1, #16
fill_data:
    str r0, [r4], #4
    subs r1, r1, #1
    bne fill_data

    ldr r4, =BIG_AT
    ldr r5, =arm_big
    ldr r6, =arm_big_end
    bl copy
    mov r7, #0
copy_fill:
    ldr r4, =FILL_AT
    add r4, r4, r7, lsl #10
    ldr r5, =arm_fill
    ldr r6, =arm_fill_end
    bl copy
    add r7, r7, #1
    cmp r7, #FILL_COUNT
    blo copy_fill
    mov r7, #0
copy_thumb:
    ldr r4, =THUMB_AT
    add r4, r4, r7, lsl #10
    ldr r5, =thumb_load
    bic r5, r5, #1
    ldr r6, =thumb_load_end
    bl copy
    add r7, r7, #1
    cmp r7, #THUMB_COUNT
    blo copy_thumb

    /* Fill the first chunk, then translate the large ARM block. */
    mov r7, #0
call_fill:
    ldr r4, =FILL_AT
    add r4, r4, r7, lsl #10
    bl call_r4
    add r7, r7, #1
    cmp r7, #FILL_COUNT
    blo call_fill
    bl run_big
    ldr r1, =0x06000000
    bl show

    /* Thumb copies end chunks at many fill levels. */
    mov r7, #0
    mov r8, #1
call_thumb:
    bl run_thumb
    movne r8, #0
    add r7, r7, #1
    cmp r7, #THUMB_COUNT
    blo call_thumb
    cmp r8, #1
    ldr r1, =0x06000002
    bl show

loop:
    bl run_big
    ldr r1, =0x06000000
    bl show
    mov r7, #0
    bl run_thumb
    ldr r1, =0x06000002
    bl show
    b loop

/* Sets the flags for r0 == expected result. */
run_big:
    push {lr}
    ldr r4, =BIG_AT
    bl call_r4
    ldr r1, =BIG_OPS
    cmp r0, r1
    pop {lr}
    bx lr

run_thumb:
    push {lr}
    ldr r4, =THUMB_AT + 1
    add r4, r4, r7, lsl #10
    bl call_r4
    cmp r0, #THUMB_OPS
    pop {lr}
    bx lr

/* The blocks load into r1 and r3-r11. */
call_r4:
    push {r5-r11, lr}
    ldr r2, =DATA_AT
    mov r0, #0
    mov lr, pc
    bx r4
    pop {r5-r11, lr}
    bx lr

/* Green when Z is set, red otherwise. Red stays. */
show:
    movne r2, #0x001f
    bne show_store
    ldrh r2, [r1]
    cmp r2, #0x001f
    bxeq lr
    mov r2, #0x03e0
show_store:
    strh r2, [r1]
    bx lr

copy:
    cmp r5, r6
    ldrlo r0, [r5], #4
    strlo r0, [r4], #4
    blo copy
    bx lr
.pool

.align 2
arm_big:
    .rept BIG_OPS
    ldmia r2, {r1, r3-r11}
    add r0, r0, r1
    .endr
    bx lr
arm_big_end:

arm_fill:
    .rept FILL_OPS
    ldmia r2, {r1, r3-r11}
    add r0, r0, r1
    .endr
    bx lr
arm_fill_end:

.align 2
.thumb
.thumb_func
thumb_load:
    .rept THUMB_OPS
    ldr r1, [r2]
    adds r0, r0, r1
    .endr
    bx lr
.align 2
thumb_load_end:

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* 32-bit forms of compressed.S. Branch and jump offsets are the byte
 * distances encoded there. */
    .option norvc
    .text
    addi a0, sp, 8
    lw a1, 124(a2)
    sw a3, 4(a4)
    lbu a5, 3(s0)
    lhu s1, 2(a0)
    lh a1, 0(a2)
    sb a3, 1(a4)
    sh a5, 2(s0)
    addi t0, t0, -32
    jal ra, .+2046
    addi t1, zero, 31
    addi sp, sp, -512
    lui t2, 1
    lui t3, 0xfffe0
    srli a0, a0, 31
    srai a1, a1, 1
    andi a2, a2, -1
    sub a3, a3, a4
    xor a5, a5, s0
    or s1, s1, a0
    and a1, a1, a2
    mul a3, a3, a4
    andi a5, a5, 255
    sext.b s0, s0
    zext.h s1, s1
    sext.h a0, a0
    xori a1, a1, -1
    jal zero, .-2048
    beq a2, zero, .+254
    bne a3, zero, .-256
    slli t4, t4, 31
    lw t5, 252(sp)
    add t6, zero, a0
    jalr zero, 0(ra)
    jalr ra, 0(t0)
    add t1, t1, t2
    sw t3, 4(sp)

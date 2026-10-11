/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Assembled independently from rv32_codegen.c. Keep this sequence in sync
 * with emit_reference_sequence() in test_codegen.c. */

    .option norvc
    .option norelax
    .text
    .p2align 2
    .global _start
_start:
    add x1, x2, x3
    sub x4, x5, x6
    sll x7, x8, x9
    slt x10, x11, x12
    sltu x13, x14, x15
    xor x16, x17, x18
    srl x19, x20, x21
    sra x22, x23, x24
    or x25, x26, x27
    and x28, x29, x30
    mul x1, x2, x3
    mulh x4, x5, x6
    mulhsu x7, x8, x9
    mulhu x10, x11, x12
    addi x5, x6, -2048
    slti x7, x8, 2047
    sltiu x9, x10, -1
    xori x11, x12, -1
    ori x13, x14, 511
    andi x15, x16, -17
    slli x17, x18, 31
    srli x19, x20, 5
    srai x21, x22, 31
    lui x23, 0xabcde
    auipc x24, 0xfffff
    andn x25, x26, x27
    ror x28, x29, x30
    rori x31, x1, 31
    xnor x2, x3, x4
    sh1add x5, x6, x7
    sh2add x8, x9, x10
    sh3add x11, x12, x13
    bext x14, x15, x16
    bexti x17, x18, 0
    bexti x19, x20, 31
    lb x1, -2048(x2)
    lh x3, 2047(x4)
    lw x5, -9(x6)
    lbu x7, 3(x8)
    lhu x9, 0(x10)
    sb x11, -2048(x12)
    sh x13, 2047(x14)
    sw x15, -9(x16)
    beq x1, x2, 1f
    addi x0, x0, 0
1:
    bne x3, x4, 2f
    addi x0, x0, 0
2:
    blt x5, x6, 3f
    addi x0, x0, 0
3:
    bge x7, x8, 4f
    addi x0, x0, 0
4:
    bltu x9, x10, 5f
    addi x0, x0, 0
5:
    bgeu x11, x12, 6f
    addi x0, x0, 0
6:
    jal x1, 7f
    addi x0, x0, 0
7:
    jalr x2, -2048(x3)
8:
    addi x0, x0, 0
    bne x5, x6, 8b
    beq x1, x2, 9f
    addi x0, x0, 0
9:
    jal x3, 10f
    addi x0, x0, 0
10:
    addi x31, x0, 1

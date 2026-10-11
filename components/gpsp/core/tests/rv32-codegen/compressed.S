/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The assembler selects compressed forms here. Keep this sequence in sync
 * with emit_compressed_sequence() in test_codegen.c. */

    .option rvc
    .option norelax
    .text
    .p2align 2
    .global _start
_start:
    addi x0, x0, 0
    addi a0, x0, -32
    addi a0, x0, 31
    addi a0, x0, 32
    addi a0, a0, 1
    addi a0, a0, -32
    addi a0, a0, 32
    addi s2, s2, 5
    add a0, a0, a1
    add a0, x0, a1
    add a0, a1, a2
    slli a0, a0, 3
    slli t6, t6, 31
    slli a0, a1, 3
    lui a0, 1
    lui a0, 0xfffff
    lui a0, 32
    jr ra
    jalr t1
    jalr t0, 0(t1)
    srli a0, a0, 1
    srai s0, s0, 31
    srli t6, t6, 1
    srai a7, a7, 2
    andi a5, a5, -32
    andi a5, a5, 31
    andi a5, a5, 32
    andi a6, a6, 1
    andi s1, s0, 1
    sub a0, a0, a1
    sub a0, a1, a0
    sub t1, t1, a0
    xor a0, a0, a5
    or s1, s1, s0
    and a2, a2, a3
    and a6, a6, a3
    and a2, a2, a7
    lw a0, 0(s0)
    lw a5, 124(a1)
    lw a5, 128(a1)
    lw a5, 2(a1)
    lw a5, -4(a1)
    lw t5, 0(s0)
    lw a0, 0(t0)
    sw a0, 0(s0)
    sw a5, 124(a1)
    sw a5, 128(a1)
    sw t6, 4(s0)
    sw a0, 4(t0)

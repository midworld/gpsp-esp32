/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The assembler selects Zcb forms here; it does not select c.not or a
 * swapped c.mul, so those are explicit. Keep this sequence in sync with
 * emit_zcb_sequence() in test_codegen.c. */

    .option rvc
    .option norelax
    .text
    .p2align 2
    .global _start
_start:
    lbu a0, 0(a1)
    lbu s0, 3(a5)
    lbu a0, 4(a1)
    lbu t0, 0(a1)
    lhu a0, 0(a1)
    lhu s1, 2(a5)
    lhu a0, 1(a1)
    lh a0, 0(a1)
    lh a4, 2(s0)
    lh a0, 4(a1)
    sb a0, 0(a1)
    sb s0, 3(a5)
    sb a0, -1(a1)
    sh a0, 0(a1)
    sh a5, 2(s1)
    sh a0, 3(a1)
    andi a0, a0, 255
    andi s1, s1, 255
    andi a0, a1, 255
    c.not a5
    xori a5, a5, 1
    mul a0, a0, a1
    c.mul a0, a1
    mul a0, a1, a2
    mul t0, t0, a0
    lbu a1, 1(a2)

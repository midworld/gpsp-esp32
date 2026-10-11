/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Short branches at their reach limits. Keep this sequence in sync with
 * test_short_branch_reference() in test_codegen.c. */

    .option rvc
    .option norelax
    .text
    .p2align 2
    .global _start
_start:
    c.beqz a0, .+254
    c.beqz a0, .-256
    c.bnez s1, .+2
    c.bnez a5, .-2
    c.beqz s0, .
    c.bnez a4, .+128
    c.j .+2046
    c.j .-2048
    c.j .
    c.j .+86

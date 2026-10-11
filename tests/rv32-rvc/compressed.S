/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Explicit RVC and Zcb forms; expanded.S holds the same instructions in
 * 32-bit form, in the same order. */
    .option rvc
    .text
    c.addi4spn a0, sp, 8
    c.lw a1, 124(a2)
    c.sw a3, 4(a4)
    c.lbu a5, 3(s0)
    c.lhu s1, 2(a0)
    c.lh a1, 0(a2)
    c.sb a3, 1(a4)
    c.sh a5, 2(s0)
    c.addi t0, -32
    c.jal .+2046
    c.li t1, 31
    c.addi16sp sp, -512
    c.lui t2, 1
    c.lui t3, 0xfffe0
    c.srli a0, 31
    c.srai a1, 1
    c.andi a2, -1
    c.sub a3, a4
    c.xor a5, s0
    c.or s1, a0
    c.and a1, a2
    c.mul a3, a4
    c.zext.b a5
    c.sext.b s0
    c.zext.h s1
    c.sext.h a0
    c.not a1
    c.j .-2048
    c.beqz a2, .+254
    c.bnez a3, .-256
    c.slli t4, 31
    c.lwsp t5, 252(sp)
    c.mv t6, a0
    c.jr ra
    c.jalr t0
    c.add t1, t2
    c.swsp t3, 4(sp)

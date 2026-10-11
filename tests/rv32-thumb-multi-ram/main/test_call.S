/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_register_map.h"

    .text
    .option push
    .option norvc

    /* x\num keeps \value unless the resolver uses it as its link or scratch. */
    .macro check_saved num, value
    .if \num != GPSP_RV32_REG_TMP0 && \num != GPSP_RV32_REG_TMP1
    li t0, \value
    bne x\num, t0, .Lfail
    .endif
    .endm

    .align 2
    .globl test_multi_ram_call
    .type test_multi_ram_call, @function
test_multi_ram_call:
    addi sp, sp, -80
    sw ra, 0(sp)
    sw s0, 4(sp)
    sw s1, 8(sp)
    sw s2, 12(sp)
    sw s3, 16(sp)
    sw s4, 20(sp)
    sw s5, 24(sp)
    sw s6, 28(sp)
    sw s7, 32(sp)
    sw s8, 36(sp)
    sw s9, 40(sp)
    sw s10, 44(sp)
    sw s11, 48(sp)
    sw a2, 52(sp)
    sw a3, 56(sp)
    sw a1, 60(sp)

    mv a2, a1
    mv a1, a0
    li s0, 0x101
    li s1, 0x102
    li s2, 0x103
    li s3, 0x104
    li s4, 0x105
    li s5, 0x106
    li s6, 0x107
    li s7, 0x108
    li s8, 0x109
    li s9, 0x10a
    li s10, 0x10b
    li s11, 0x10c

    lla RV_TMP0, gpsp_rv32_thumb_multi_ram
    jalr RV_TMP1, 0(RV_TMP0)
.Lback:
    lw RV_TMP0, 52(sp)
    sw a0, 0(RV_TMP0)
    lw RV_TMP0, 56(sp)
    sw a1, 0(RV_TMP0)
    lw RV_TMP0, 60(sp)
    bne a2, RV_TMP0, .Lfail
    lla RV_TMP0, .Lback
    bne RV_TMP1, RV_TMP0, .Lfail
    lw RV_TMP0, 0(sp)
    bne ra, RV_TMP0, .Lfail
    /* s0-s11 are x8, x9 and x18-x27. */
    check_saved 8, 0x101
    check_saved 9, 0x102
    check_saved 18, 0x103
    check_saved 19, 0x104
    check_saved 20, 0x105
    check_saved 21, 0x106
    check_saved 22, 0x107
    check_saved 23, 0x108
    check_saved 24, 0x109
    check_saved 25, 0x10a
    check_saved 26, 0x10b
    check_saved 27, 0x10c
    li a0, 1
    j .Lrestore
.Lfail:
    li a0, 0
.Lrestore:
    lw ra, 0(sp)
    lw s0, 4(sp)
    lw s1, 8(sp)
    lw s2, 12(sp)
    lw s3, 16(sp)
    lw s4, 20(sp)
    lw s5, 24(sp)
    lw s6, 28(sp)
    lw s7, 32(sp)
    lw s8, 36(sp)
    lw s9, 40(sp)
    lw s10, 44(sp)
    lw s11, 48(sp)
    addi sp, sp, 80
    ret
    .size test_multi_ram_call, .-test_multi_ram_call
    .option pop

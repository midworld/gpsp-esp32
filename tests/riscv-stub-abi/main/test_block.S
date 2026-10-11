/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The static block exercises the production register map without depending
 * on executable RAM or any ARM instruction decoder. */
#include "rv32_register_map.h"

/* Guest register n in generated code. */
#define G(n) GPSP_RV32_X(GPSP_RV32_REG_GUEST_R##n)
#define RV_GUEST_SP G(13)
#define RV_GUEST_LR G(14)

    .text
    .option push
    .option norvc
    .align 2
    .globl gpsp_rv32_abi_test_block
    .type gpsp_rv32_abi_test_block, @function
gpsp_rv32_abi_test_block:
    addi G(0), G(0), 5             /* guest r0 */
    add G(4), G(4), G(0)           /* guest r4 */
    addi G(8), G(8), 7             /* guest r8 */
    addi G(9), G(9), 9             /* guest r9 */
    addi G(12), G(12), 11          /* guest r12; RV_STATE stays the base */
    addi RV_GUEST_SP, RV_GUEST_SP, 4 /* guest SP */
    addi RV_GUEST_LR, RV_GUEST_LR, -2 /* guest LR */
    addi RV_BUDGET, RV_BUDGET, -3  /* remaining cycles */
    addi RV_PC, RV_PC, 4           /* next guest PC */
    xori RV_FLAG_N, RV_FLAG_N, 1   /* N */
    xori RV_FLAG_Z, RV_FLAG_Z, 1   /* Z */
    xori RV_FLAG_C, RV_FLAG_C, 1   /* C */
    xori RV_FLAG_V, RV_FLAG_V, 1   /* V */
    ret
    .size gpsp_rv32_abi_test_block, .-gpsp_rv32_abi_test_block

/* Copies the unpacked N, Z, C, V to guest r0-r3; r4-r7 become NZCV. */
    .align 2
    .globl gpsp_rv32_abi_flags_block
    .type gpsp_rv32_abi_flags_block, @function
gpsp_rv32_abi_flags_block:
    mv G(0), RV_FLAG_N
    mv G(1), RV_FLAG_Z
    mv G(2), RV_FLAG_C
    mv G(3), RV_FLAG_V
    mv RV_FLAG_N, G(4)
    mv RV_FLAG_Z, G(5)
    mv RV_FLAG_C, G(6)
    mv RV_FLAG_V, G(7)
    ret
    .size gpsp_rv32_abi_flags_block, .-gpsp_rv32_abi_flags_block

/* Leaves ADD r4 + r5 pending: operands in RV_FLAG_N/Z, negative V. */
    .align 2
    .globl gpsp_rv32_abi_lazy_add_block
    .type gpsp_rv32_abi_lazy_add_block, @function
gpsp_rv32_abi_lazy_add_block:
    mv RV_FLAG_N, G(4)
    mv RV_FLAG_Z, G(5)
    li RV_FLAG_V, -1
    ret
    .size gpsp_rv32_abi_lazy_add_block, .-gpsp_rv32_abi_lazy_add_block

/* Change live NZCV before the budget guard stores them. */
    .align 2
    .globl gpsp_rv32_abi_flags_guard_block
    .type gpsp_rv32_abi_flags_guard_block, @function
gpsp_rv32_abi_flags_guard_block:
    xori RV_FLAG_N, RV_FLAG_N, 1
    xori RV_FLAG_Z, RV_FLAG_Z, 1
    xori RV_FLAG_C, RV_FLAG_C, 1
    xori RV_FLAG_V, RV_FLAG_V, 1
    j gpsp_rv32_abi_guard_block
    .size gpsp_rv32_abi_flags_guard_block, .-gpsp_rv32_abi_flags_guard_block

/* Leave ADD r4 + r5 pending before the budget guard. */
    .align 2
    .globl gpsp_rv32_abi_lazy_guard_block
    .type gpsp_rv32_abi_lazy_guard_block, @function
gpsp_rv32_abi_lazy_guard_block:
    mv RV_FLAG_N, G(4)
    mv RV_FLAG_Z, G(5)
    li RV_FLAG_V, -1
    j gpsp_rv32_abi_guard_block
    .size gpsp_rv32_abi_lazy_guard_block, .-gpsp_rv32_abi_lazy_guard_block

/* A budget guard returns the following native instruction in a1. */
    .align 2
    .globl gpsp_rv32_abi_guard_block
    .type gpsp_rv32_abi_guard_block, @function
gpsp_rv32_abi_guard_block:
    blt zero, RV_BUDGET, gpsp_rv32_abi_guard_body
    jalr a1, ra, 4
    .globl gpsp_rv32_abi_guard_body
gpsp_rv32_abi_guard_body:
    addi G(0), G(0), 1
    addi RV_PC, RV_PC, 2
    addi RV_BUDGET, RV_BUDGET, -1
    ret
    .size gpsp_rv32_abi_guard_block, .-gpsp_rv32_abi_guard_block

    .align 2
    .globl gpsp_rv32_abi_bl_cache_block
    .type gpsp_rv32_abi_bl_cache_block, @function
gpsp_rv32_abi_bl_cache_block:
    mv a0, RV_GUEST_LR
    j gpsp_rv32_thumb_bl_exit_shared
    .size gpsp_rv32_abi_bl_cache_block, .-gpsp_rv32_abi_bl_cache_block

    .align 2
    .globl gpsp_rv32_abi_arm_cache_block
    .type gpsp_rv32_abi_arm_cache_block, @function
gpsp_rv32_abi_arm_cache_block:
    mv a0, RV_GUEST_LR
    li a1, 0
    j gpsp_rv32_arm_bx_exit_shared
    .size gpsp_rv32_abi_arm_cache_block, .-gpsp_rv32_abi_arm_cache_block

    .align 2
    .globl gpsp_rv32_abi_arm_pc_exit_block
    .type gpsp_rv32_abi_arm_pc_exit_block, @function
gpsp_rv32_abi_arm_pc_exit_block:
    mv a0, RV_GUEST_LR
    j gpsp_rv32_arm_pc_exit_shared
    .size gpsp_rv32_abi_arm_pc_exit_block, .-gpsp_rv32_abi_arm_pc_exit_block

    .align 2
    .globl gpsp_rv32_abi_system_exit_block
    .type gpsp_rv32_abi_system_exit_block, @function
gpsp_rv32_abi_system_exit_block:
    mv RV_PC, RV_GUEST_LR
    j gpsp_rv32_system_exit_shared
    .size gpsp_rv32_abi_system_exit_block, .-gpsp_rv32_abi_system_exit_block

    .align 2
    .globl gpsp_rv32_abi_cached_block
    .type gpsp_rv32_abi_cached_block, @function
gpsp_rv32_abi_cached_block:
    mv G(0), RV_PC
    ret
    .size gpsp_rv32_abi_cached_block, .-gpsp_rv32_abi_cached_block

/* Seed every callee-saved register, invoke the stub, then verify that the
 * caller's values survived. Return -1 if the host ABI was violated. */
    .align 2
    .globl gpsp_rv32_abi_check_saved
    .type gpsp_rv32_abi_check_saved, @function
gpsp_rv32_abi_check_saved:
    addi sp, sp, -64
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

    li s0, 0x11
    li s1, 0x12
    li s2, 0x13
    li s3, 0x14
    li s4, 0x15
    li s5, 0x16
    li s6, 0x17
    li s7, 0x18
    li s8, 0x19
    li s9, 0x1A
    li s10, 0x1B
    li s11, 0x1C
    call gpsp_rv32_run_block
    mv t5, a0

    li t6, 0x11
    bne s0, t6, .Lbad_saved
    li t6, 0x12
    bne s1, t6, .Lbad_saved
    li t6, 0x13
    bne s2, t6, .Lbad_saved
    li t6, 0x14
    bne s3, t6, .Lbad_saved
    li t6, 0x15
    bne s4, t6, .Lbad_saved
    li t6, 0x16
    bne s5, t6, .Lbad_saved
    li t6, 0x17
    bne s6, t6, .Lbad_saved
    li t6, 0x18
    bne s7, t6, .Lbad_saved
    li t6, 0x19
    bne s8, t6, .Lbad_saved
    li t6, 0x1A
    bne s9, t6, .Lbad_saved
    li t6, 0x1B
    bne s10, t6, .Lbad_saved
    li t6, 0x1C
    bne s11, t6, .Lbad_saved
    mv a0, t5
    j .Lrestore_saved

.Lbad_saved:
    li a0, -1
.Lrestore_saved:
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
    addi sp, sp, 64
    ret
    .size gpsp_rv32_abi_check_saved, .-gpsp_rv32_abi_check_saved
    .option pop

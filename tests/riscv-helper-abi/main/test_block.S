/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_register_map.h"

/* Guest register n in generated code. */
#define G(n) GPSP_RV32_X(GPSP_RV32_REG_GUEST_R##n)
#define RV_GUEST_SP G(13)
#define RV_GUEST_LR G(14)

    .text
    .option push
    .option norvc
    .align 2

    .globl gpsp_rv32_test_continue_block
    .type gpsp_rv32_test_continue_block, @function
gpsp_rv32_test_continue_block:
    la a0, gpsp_rv32_test_helper_continue
    li a1, 0x123
    li a2, 0x456
    call gpsp_rv32_call_helper
    bnez a1, .Lunexpected_exit
    mv G(2), a0                    /* result becomes guest r2 */
    addi G(0), G(0), 2             /* callback-updated guest r0 */
    addi G(9), G(9), 1             /* callback-updated guest r9 */
    addi RV_BUDGET, RV_BUDGET, -2
    addi RV_PC, RV_PC, 4
    ret
.Lunexpected_exit:
    li G(2), -1
    ret
    .size gpsp_rv32_test_continue_block, .-gpsp_rv32_test_continue_block

    .align 2
    .globl gpsp_rv32_test_mode_block
    .type gpsp_rv32_test_mode_block, @function
gpsp_rv32_test_mode_block:
    la a0, gpsp_rv32_test_helper_mode
    li a1, 0x789
    li a2, 0xABC
    call gpsp_rv32_call_helper
    beqz a1, .Lunexpected_continue
    ret
.Lunexpected_continue:
    addi G(0), G(0), 99
    ret
    .size gpsp_rv32_test_mode_block, .-gpsp_rv32_test_mode_block

    .align 2
    .globl gpsp_rv32_test_force_block
    .type gpsp_rv32_test_force_block, @function
gpsp_rv32_test_force_block:
    la a0, gpsp_rv32_test_helper_force
    li a1, 0x135
    li a2, 0x246
    call gpsp_rv32_call_helper
    beqz a1, .Lunexpected_continue
    ret
    .size gpsp_rv32_test_force_block, .-gpsp_rv32_test_force_block
    .option pop

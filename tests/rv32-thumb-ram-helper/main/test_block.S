/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_register_map.h"

/* Guest register n in generated code. */
#define G(n) GPSP_RV32_X(GPSP_RV32_REG_GUEST_R##n)

    .text
    .option push
    .option norvc

    .macro test_block name, load_helper
    .align 2
    .globl \name
    .type \name, @function
\name:
    mv a1, G(0)                  /* guest r0 contains the test address */
    lla RV_TMP1, \load_helper
    jalr RV_TMP0, 0(RV_TMP1)
    addi RV_BUDGET, RV_BUDGET, -2 /* captured fetch wait */
    mv G(1), a0                  /* guest r1 receives the load */
    ret
    .size \name, .-\name
    .endm

    test_block gpsp_rv32_test_ram_u8,  gpsp_rv32_ram_load_u8
    test_block gpsp_rv32_test_ram_u16, gpsp_rv32_ram_load_u16
    test_block gpsp_rv32_test_ram_u32, gpsp_rv32_ram_load_u32
    test_block gpsp_rv32_test_ram_s8,  gpsp_rv32_ram_load_s8
    test_block gpsp_rv32_test_ram_s16, gpsp_rv32_ram_load_s16

    .macro test_store_block name, store_helper
    .align 2
    .globl \name
    .type \name, @function
\name:
    mv a1, G(0)                  /* guest r0: address */
    mv a2, G(1)                  /* guest r1: value */
    lla RV_TMP1, \store_helper
    jalr RV_TMP0, 0(RV_TMP1)
    addi RV_BUDGET, RV_BUDGET, -2 /* captured fetch wait */
    ret
    .size \name, .-\name
    .endm

    test_store_block gpsp_rv32_test_ram_store_u8,  gpsp_rv32_ram_store_u8
    test_store_block gpsp_rv32_test_ram_store_u16, gpsp_rv32_ram_store_u16
    test_store_block gpsp_rv32_test_ram_store_u32, gpsp_rv32_ram_store_u32
    .option pop

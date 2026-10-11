/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"
#include "../gpsp_config.h"

  .text
  .option push
  .option norvc
#if GPSP_RV32_USE_ZBA
  /* Follows the JIT extension flags, not the assembler ISA. */
  .option arch, +zba
#endif
#if GPSP_RV32_USE_ZBS
  .option arch, +zbs
#endif

  /* a1 = CPSR.T from reg[REG_CPSR]. */
  .macro load_thumb_state
  lw a1, 64(RV_STATE)
#if GPSP_RV32_USE_ZBS
  bexti a1, a1, 5
#else
  srli a1, a1, 5
  andi a1, a1, 1
#endif
  .endm
  .align 2
  .globl gpsp_rv32_thumb_bx_exit_shared
  .type gpsp_rv32_thumb_bx_exit_shared, @function
gpsp_rv32_thumb_bx_exit_shared:
  /* A0 is the aligned target; A1 is the new Thumb state. */
  mv RV_PC, a0
  lw RV_TMP0, 64(RV_STATE)
  andi RV_TMP0, RV_TMP0, -33
  slli RV_TMP1, a1, 5
  or RV_TMP0, RV_TMP0, RV_TMP1
  sw RV_TMP0, 64(RV_STATE)
  /* The existing flow exit skips fetch and idle checks for ARM targets. */
  beqz a1, .Lreturn
  srli RV_TMP0, RV_PC, 24
  andi RV_TMP0, RV_TMP0, 15
  slli RV_TMP0, RV_TMP0, 1
  j .Lthumb_seq
  .size gpsp_rv32_thumb_bx_exit_shared, .-gpsp_rv32_thumb_bx_exit_shared

  .align 2
  .globl gpsp_rv32_arm_bx_exit_shared
  .type gpsp_rv32_arm_bx_exit_shared, @function
gpsp_rv32_arm_bx_exit_shared:
  /* A0 is the aligned target; A1 is the new Thumb state. */
  mv RV_PC, a0
  lw RV_TMP0, 64(RV_STATE)
  andi RV_TMP0, RV_TMP0, -33
  slli RV_TMP1, a1, 5
  or RV_TMP0, RV_TMP0, RV_TMP1
  sw RV_TMP0, 64(RV_STATE)
  /* The existing ARM exit skips fetch and idle checks for Thumb targets. */
  bnez a1, .Lreturn
  srli RV_TMP0, RV_PC, 24
  andi RV_TMP0, RV_TMP0, 15
  slli RV_TMP0, RV_TMP0, 1
  lla RV_TMP1, ws_cyc_nseq
  add RV_TMP1, RV_TMP1, RV_TMP0
  lbu RV_TMP1, 1(RV_TMP1)
  sub RV_BUDGET, RV_BUDGET, RV_TMP1
  j .Larm_seq
  .size gpsp_rv32_arm_bx_exit_shared, .-gpsp_rv32_arm_bx_exit_shared

  .align 2
  .globl gpsp_rv32_arm_pc_exit_shared
  .type gpsp_rv32_arm_pc_exit_shared, @function
gpsp_rv32_arm_pc_exit_shared:
  /* A0 is the new PC. A helper may have changed the CPSR state. */
  mv RV_PC, a0
  load_thumb_state
  srli RV_TMP0, RV_PC, 24
  andi RV_TMP0, RV_TMP0, 15
  slli RV_TMP0, RV_TMP0, 1
.Larm_seq:
  lla RV_TMP1, ws_cyc_seq
  add RV_TMP1, RV_TMP1, RV_TMP0
  lbu RV_TMP1, 1(RV_TMP1)
  sub RV_BUDGET, RV_BUDGET, RV_TMP1
  lla RV_TMP0, idle_loop_target_pc
  lw RV_TMP0, 0(RV_TMP0)
  bne RV_PC, RV_TMP0, .Lreturn
  bge zero, RV_BUDGET, .Lreturn
  li RV_BUDGET, 0
  j .Lreturn
  .size gpsp_rv32_arm_pc_exit_shared, .-gpsp_rv32_arm_pc_exit_shared

  .align 2
  .globl gpsp_rv32_system_exit_shared
  .type gpsp_rv32_system_exit_shared, @function
gpsp_rv32_system_exit_shared:
  /* After an SWI, MSR or exception return helper. The helper bridge
     reloaded RV_PC and the guest state; the helper charged any fetch. */
  load_thumb_state
  j .Lreturn
  .size gpsp_rv32_system_exit_shared, .-gpsp_rv32_system_exit_shared

  .align 2
  .globl gpsp_rv32_thumb_pop_exit_shared
  .type gpsp_rv32_thumb_pop_exit_shared, @function
gpsp_rv32_thumb_pop_exit_shared:
  /* RV_PC is the aligned Thumb target loaded by POP {pc}. */
  li a1, 1
  srli RV_TMP0, RV_PC, 24
  andi RV_TMP0, RV_TMP0, 15
  slli RV_TMP0, RV_TMP0, 1
  j .Lthumb_seq
  .size gpsp_rv32_thumb_pop_exit_shared, .-gpsp_rv32_thumb_pop_exit_shared

  .align 2
  .globl gpsp_rv32_thumb_bl_exit_shared
  .type gpsp_rv32_thumb_bl_exit_shared, @function
gpsp_rv32_thumb_bl_exit_shared:
  /* BL charges the nonsequential and sequential target fetches. */
  mv RV_PC, a0
  li a1, 1
  srli RV_TMP0, RV_PC, 24
  andi RV_TMP0, RV_TMP0, 15
  slli RV_TMP0, RV_TMP0, 1
  lla RV_TMP1, ws_cyc_nseq
  add RV_TMP1, RV_TMP1, RV_TMP0
  lbu RV_TMP1, 0(RV_TMP1)
  sub RV_BUDGET, RV_BUDGET, RV_TMP1

.Lthumb_seq:
  lla RV_TMP1, ws_cyc_seq
  add RV_TMP1, RV_TMP1, RV_TMP0
  lbu RV_TMP1, 0(RV_TMP1)
  sub RV_BUDGET, RV_BUDGET, RV_TMP1
  lla RV_TMP0, idle_loop_target_pc
  lw RV_TMP0, 0(RV_TMP0)
  bne RV_PC, RV_TMP0, .Lreturn
  bge zero, RV_BUDGET, .Lreturn
  li RV_BUDGET, 0
.Lreturn:
  /* With budget left, a cached target runs like a linked branch. */
  bge zero, RV_BUDGET, gpsp_rv32_branch_cache_miss
  or a0, RV_PC, a1
  srli RV_TMP0, RV_PC, 1
  andi RV_TMP0, RV_TMP0, GPSP_RV32_BRANCH_CACHE_SIZE - 1
#if GPSP_RV32_USE_ZBA
  lla RV_TMP1, gpsp_rv32_branch_cache
  sh3add RV_TMP0, RV_TMP0, RV_TMP1
#else
  slli RV_TMP0, RV_TMP0, 3
  lla RV_TMP1, gpsp_rv32_branch_cache
  add RV_TMP0, RV_TMP0, RV_TMP1
#endif
  lw RV_TMP1, 0(RV_TMP0)
  bne RV_TMP1, a0, gpsp_rv32_branch_cache_miss
  lw RV_TMP1, 4(RV_TMP0)
  /* A0 is PC | Thumb; subtract Thumb to align the next fetch. */
  sub RV_PC, a0, a1
  jr RV_TMP1
  /* Empty entries point here, so a stray key match still returns. */
  .globl gpsp_rv32_branch_cache_miss
gpsp_rv32_branch_cache_miss:
  ret
  .size gpsp_rv32_thumb_bl_exit_shared, .-gpsp_rv32_thumb_bl_exit_shared
  .option pop

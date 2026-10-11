/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

  .text
  .option push
  .option norvc
#if GPSP_RV32_USE_ZBA
  /* Follows the JIT extension flag, not the assembler ISA. */
  .option arch, +zba
#endif

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  .macro count_multi name
  addi sp, sp, -16
  sw RV_TMP0, 0(sp)
  sw RV_TMP1, 4(sp)
  lla RV_TMP0, \name
  lw RV_TMP1, 0(RV_TMP0)
  addi RV_TMP1, RV_TMP1, 1
  sw RV_TMP1, 0(RV_TMP0)
  bnez RV_TMP1, .Lcount_done\@
  lw RV_TMP1, 4(RV_TMP0)
  addi RV_TMP1, RV_TMP1, 1
  sw RV_TMP1, 4(RV_TMP0)
.Lcount_done\@:
  lw RV_TMP0, 0(sp)
  lw RV_TMP1, 4(sp)
  addi sp, sp, 16
  .endm
#else
  .macro count_multi name
  .endm
#endif

  .macro multi_resolver name, stores
  .align 2
  .globl \name
  .type \name, @function
\name:
  srli a0, a1, 24
  addi a0, a0, -2
  sltiu RV_TMP0, a0, 2
  beqz RV_TMP0, .Lmiss\@

  andi RV_TMP0, a1, 3
  bnez RV_TMP0, .Lmiss\@
  beqz a2, .Lmiss\@
  sltiu RV_TMP0, a2, 10
  beqz RV_TMP0, .Lmiss\@
  bnez a0, .Liwram\@

  slli RV_TMP0, a1, 14
  srli RV_TMP0, RV_TMP0, 14
#if GPSP_RV32_USE_ZBA
  sh2add a1, a2, RV_TMP0
#else
  slli a1, a2, 2
  add a1, a1, RV_TMP0
#endif
  li a0, 0x40000
  bltu a0, a1, .Lmiss\@
  lla a0, ewram
  add a0, a0, RV_TMP0
#if GPSP_RV32_USE_ZBA
  sh1add a1, a2, a2
#else
  slli a1, a2, 1
  add a1, a1, a2
#endif
  slli a1, a1, 1
  .if \stores
    addi sp, sp, -16
    sw a1, 0(sp)
    sw a2, 4(sp)
    li a1, 0x40000
    add RV_TMP0, a0, a1
.Lewram_tag\@:
    lw a1, 0(RV_TMP0)
    bnez a1, .Lmiss_stack\@
    addi RV_TMP0, RV_TMP0, 4
    addi a2, a2, -1
    bnez a2, .Lewram_tag\@
    lw a1, 0(sp)
    lw a2, 4(sp)
    addi sp, sp, 16
  .endif
  count_multi gpsp_rv32_jit_multi_fast
  jr RV_TMP1

.Liwram\@:
  slli RV_TMP0, a1, 17
  srli RV_TMP0, RV_TMP0, 17
#if GPSP_RV32_USE_ZBA
  sh2add a1, a2, RV_TMP0
#else
  slli a1, a2, 2
  add a1, a1, RV_TMP0
#endif
  li a0, 0x8000
  bltu a0, a1, .Lmiss\@
  lla a0, iwram+0x8000
  add a0, a0, RV_TMP0
  mv a1, a2
  .if \stores
    addi sp, sp, -16
    sw a1, 0(sp)
    sw a2, 4(sp)
    li a1, 0x8000
    sub RV_TMP0, a0, a1
.Liwram_tag\@:
    lw a1, 0(RV_TMP0)
    bnez a1, .Lmiss_stack\@
    addi RV_TMP0, RV_TMP0, 4
    addi a2, a2, -1
    bnez a2, .Liwram_tag\@
    lw a1, 0(sp)
    lw a2, 4(sp)
    addi sp, sp, 16
  .endif
  count_multi gpsp_rv32_jit_multi_fast
  jr RV_TMP1

.Lmiss_stack\@:
  .if \stores
    addi sp, sp, 16
  .endif
.Lmiss\@:
  count_multi gpsp_rv32_jit_multi_slow
  li a0, 0
  li a1, 0
  jr RV_TMP1
  .size \name, .-\name
  .endm

  multi_resolver gpsp_rv32_thumb_multi_ram, 0
  multi_resolver gpsp_rv32_thumb_multi_ram_store, 1
  .option pop

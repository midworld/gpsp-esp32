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

  /* Loads a0 from ROM and charges the wait, or branches to \miss. Uses a2,
     RV_TMP1 and \tmp; \tmp may be a0. */
  .macro rom_read load_op, align_mask, word, miss, tmp
  srli a2, a1, 24
  addi \tmp, a2, -8
  sltiu \tmp, \tmp, 5
  beqz \tmp, \miss

  .if \align_mask
    andi \tmp, a1, \align_mask
    bnez \tmp, \miss
  .endif

  slli \tmp, a1, 7
  srli \tmp, \tmp, 7
  lla RV_TMP1, gamepak_size
  lw RV_TMP1, 0(RV_TMP1)
  bgeu \tmp, RV_TMP1, \miss

  srli \tmp, a1, 15
#if GPSP_RV32_USE_ZBA
  lla RV_TMP1, memory_map_read
  sh2add \tmp, \tmp, RV_TMP1
#else
  slli \tmp, \tmp, 2
  lla RV_TMP1, memory_map_read
  add \tmp, \tmp, RV_TMP1
#endif
  lw RV_TMP1, 0(\tmp)
  beqz RV_TMP1, \miss

  slli \tmp, a1, 17
  srli \tmp, \tmp, 17
  add RV_TMP1, RV_TMP1, \tmp
  \load_op a0, 0(RV_TMP1)

#if GPSP_RV32_USE_ZBA
  lla RV_TMP1, ws_cyc_nseq+\word
  sh1add RV_TMP1, a2, RV_TMP1
#else
  slli a2, a2, 1
  lla RV_TMP1, ws_cyc_nseq+\word
  add RV_TMP1, RV_TMP1, a2
#endif
  lbu RV_TMP1, 0(RV_TMP1)
  sub RV_BUDGET, RV_BUDGET, RV_TMP1
  .endm

  /* a0/a1 retain the C callback and guest address; reads leave a2 free. */
  .macro rom_load name, load_op, align_mask, word
  .align 2
  .globl \name
  .type \name, @function
\name:
  rom_read \load_op, \align_mask, \word, .Lslow\@, RV_TMP0
  li a1, 0
  mv RV_TMP0, ra
  lla ra, gpsp_rv32_block_return
  jr RV_TMP0

.Lslow\@:
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  lla RV_TMP1, gpsp_rv32_call_memory_read_helper
#else
  lla RV_TMP1, gpsp_rv32_call_helper
#endif
  jr RV_TMP1
  .size \name, .-\name
  .endm

  /* Called with RV_TMP0 as the link; ra and a1 stay intact. A hit returns to
     RV_TMP0; a miss returns to RV_TMP0 + 4, the site's slow path. */
  .macro rom_probe name, load_op, align_mask, word
  .align 2
  .globl \name
  .type \name, @function
\name:
  rom_read \load_op, \align_mask, \word, .Lmiss\@, a0
  jr RV_TMP0

.Lmiss\@:
  jalr zero, RV_TMP0, 4
  .size \name, .-\name
  .endm

  rom_load gpsp_rv32_rom_load_u8,  lbu, 0, 0
  rom_load gpsp_rv32_rom_load_u16, lhu, 1, 0
  rom_load gpsp_rv32_rom_load_u32, lw,  3, 1
  rom_load gpsp_rv32_rom_load_s8,  lb,  0, 0
  rom_load gpsp_rv32_rom_load_s16, lh,  1, 0

  rom_probe gpsp_rv32_rom_probe_u8,  lbu, 0, 0
  rom_probe gpsp_rv32_rom_probe_u16, lhu, 1, 0
  rom_probe gpsp_rv32_rom_probe_u32, lw,  3, 1
  rom_probe gpsp_rv32_rom_probe_s8,  lb,  0, 0
  rom_probe gpsp_rv32_rom_probe_s16, lh,  1, 0

  .option pop

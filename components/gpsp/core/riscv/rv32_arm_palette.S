/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

#if defined(USE_XBGR1555_FORMAT)
#define PALETTE_EXPAND_MASK 0x03E07C1F
#else
#define PALETTE_EXPAND_MASK 0x07E0F81F
#endif

  .text
  .option push
  .option norvc
#if GPSP_RV32_USE_ZBB
  /* Follows the JIT extension flag, not the assembler ISA. */
  .option arch, +zbb
#endif
  /* Writes both colors of a converted word (a1) to palette_ram_expanded.
     RV_TMP0 points at the word in palette_ram_converted; a2 is free here. */
  .macro store_expanded
  lla RV_TMP1, palette_ram_converted
  sub RV_TMP0, RV_TMP0, RV_TMP1
  slli RV_TMP0, RV_TMP0, 1
  lla RV_TMP1, palette_ram_expanded
  add RV_TMP0, RV_TMP0, RV_TMP1
#if GPSP_RV32_USE_ZBB
  zext.h a2, a1
#else
  slli a2, a1, 16
  srli a2, a2, 16
#endif
  slli RV_TMP1, a2, 16
  or a2, a2, RV_TMP1
  li RV_TMP1, PALETTE_EXPAND_MASK
  and a2, a2, RV_TMP1
  sw a2, 0(RV_TMP0)
  srli a2, a1, 16
  slli RV_TMP1, a2, 16
  or a2, a2, RV_TMP1
  li RV_TMP1, PALETTE_EXPAND_MASK
  and a2, a2, RV_TMP1
  sw a2, 4(RV_TMP0)
  .endm
  .align 2
  .globl gpsp_rv32_arm_palette_store32
  .type gpsp_rv32_arm_palette_store32, @function
gpsp_rv32_arm_palette_store32:
  /* a0 is the JIT continuation; ra remains the block exit. */
  andi RV_TMP1, a1, 0x3ff
  lla RV_TMP0, palette_ram
  add RV_TMP0, RV_TMP0, RV_TMP1
  sw a2, 0(RV_TMP0)
  lla RV_TMP0, palette_ram_converted
  add RV_TMP0, RV_TMP0, RV_TMP1
#if defined(USE_XBGR1555_FORMAT)
  li RV_TMP1, 0x7fff7fff
  and a1, a2, RV_TMP1
#else
  li RV_TMP1, 0x001f001f
  and a1, a2, RV_TMP1
  slli a1, a1, 11
  slli RV_TMP1, RV_TMP1, 5
  and RV_TMP1, a2, RV_TMP1
  slli RV_TMP1, RV_TMP1, 1
  or a1, a1, RV_TMP1
  li RV_TMP1, 0x7c007c00
  and RV_TMP1, a2, RV_TMP1
  srli RV_TMP1, RV_TMP1, 10
  or a1, a1, RV_TMP1
#endif
  sw a1, 0(RV_TMP0)
  store_expanded
  addi RV_BUDGET, RV_BUDGET, -2
  jr a0
  .size gpsp_rv32_arm_palette_store32, .-gpsp_rv32_arm_palette_store32

  .align 2
  .globl gpsp_rv32_arm_palette_store32_compact
  .type gpsp_rv32_arm_palette_store32_compact, @function
gpsp_rv32_arm_palette_store32_compact:
  /* The emitter requires raw RAM to follow converted RAM by 0x400 bytes. */
  andi RV_TMP1, a1, 0x3ff
  lla RV_TMP0, palette_ram_converted
  add RV_TMP0, RV_TMP0, RV_TMP1
  sw a2, 0x400(RV_TMP0)
#if defined(USE_XBGR1555_FORMAT)
  li RV_TMP1, 0x7fff7fff
  and a1, a2, RV_TMP1
#else
  li RV_TMP1, 0x001f001f
  srli a1, a2, 10
  and a1, a1, RV_TMP1
  and RV_TMP1, a2, RV_TMP1
  slli RV_TMP1, RV_TMP1, 11
  or a1, a1, RV_TMP1
  li RV_TMP1, 0x03e003e0
  and RV_TMP1, a2, RV_TMP1
  slli RV_TMP1, RV_TMP1, 1
  or a1, a1, RV_TMP1
#endif
  sw a1, 0(RV_TMP0)
  store_expanded
  addi RV_BUDGET, RV_BUDGET, -2
  jr a0
  .size gpsp_rv32_arm_palette_store32_compact, .-gpsp_rv32_arm_palette_store32_compact
  .option pop

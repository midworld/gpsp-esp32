/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

  .text
  .option push
  .option norvc
#if GPSP_RV32_USE_ZBB
  /* Follows the JIT extension flag, not the assembler ISA. */
  .option arch, +zbb
#endif

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  .macro count_ram_load name
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
  .macro count_ram_load name
  .endm
#endif

  /* Return to RV_TMP0 after a native access at the next Thumb PC. */
  .macro ram_return
  addi RV_PC, RV_PC, 2
  jalr zero, 0(RV_TMP0)
  .endm

  /* RV_TMP0 points to the site's fetch charge. ra exits the block. */
  .macro ram_load name, load_op, kind, align_mask, ew_cycles
  .align 2
  .globl \name
  .type \name, @function
\name:
  blt zero, RV_BUDGET, .Lready\@
  ret
  .globl \name\()_flow
  .type \name\()_flow, @function
\name\()_flow:
  blt zero, RV_BUDGET, .Lready\@
  /* RV_TMP0 is the return address, 12 bytes after the native load site. */
  addi a1, RV_TMP0, -12
  jalr zero, 4(ra)
.Lready\@:
  .if \align_mask
    andi RV_TMP1, a1, \align_mask
    bnez RV_TMP1, .Lslow\@
  .endif
  srli RV_TMP1, a1, 24
  addi RV_TMP1, RV_TMP1, -3
  bnez RV_TMP1, .Lewram\@
  slli RV_TMP1, a1, 17
  srli RV_TMP1, RV_TMP1, 17
  lui a0, %hi(iwram + 0x8000)
  add a0, a0, RV_TMP1
  count_ram_load gpsp_rv32_jit_ram_load_fast
  \load_op a0, %lo(iwram + 0x8000)(a0)
  addi RV_BUDGET, RV_BUDGET, -1
  ram_return

.Lewram\@:
  addi RV_TMP1, RV_TMP1, 1
  bnez RV_TMP1, .Lio\@
  slli RV_TMP1, a1, 14
  srli RV_TMP1, RV_TMP1, 14
  lui a0, %hi(ewram)
  add a0, a0, RV_TMP1
  count_ram_load gpsp_rv32_jit_ram_load_fast
  \load_op a0, %lo(ewram)(a0)
  addi RV_BUDGET, RV_BUDGET, -\ew_cycles
  ram_return

.Lio\@:
  /* I/O registers without read side effects: masked backing store. */
  addi RV_TMP1, RV_TMP1, -2
  bnez RV_TMP1, .Lslow\@
  slli RV_TMP1, a1, 8
  .if \kind == 2
    srli RV_TMP1, RV_TMP1, 10
    sltiu a0, RV_TMP1, 256
    beqz a0, .Lslow\@
    slli RV_TMP1, RV_TMP1, 2
    lui a2, %hi(gpsp_rv32_io_read_mask32)
    add a2, a2, RV_TMP1
    lw a2, %lo(gpsp_rv32_io_read_mask32)(a2)
  .else
    srli RV_TMP1, RV_TMP1, 9
    sltiu a0, RV_TMP1, 512
    beqz a0, .Lslow\@
    slli a0, RV_TMP1, 2
    slli RV_TMP1, RV_TMP1, 1
    lui a2, %hi(gpsp_rv32_io_read_mask16)
    add a2, a2, a0
    lw a2, %lo(gpsp_rv32_io_read_mask16)(a2)
  .endif
  beqz a2, .Lslow\@
  lui a0, %hi(io_registers)
  add a0, a0, RV_TMP1
  .if \kind == 2
    lw a0, %lo(io_registers)(a0)
  .else
    lhu a0, %lo(io_registers)(a0)
  .endif
  and a0, a0, a2
  .if \kind == 0 || \kind == 3
    andi RV_TMP1, a1, 1
    slli RV_TMP1, RV_TMP1, 3
    srl a0, a0, RV_TMP1
    .if \kind == 0
      andi a0, a0, 255
    .else
#if GPSP_RV32_USE_ZBB
      sext.b a0, a0
#else
      slli a0, a0, 24
      srai a0, a0, 24
#endif
    .endif
  .elseif \kind == 4
#if GPSP_RV32_USE_ZBB
    sext.h a0, a0
#else
    slli a0, a0, 16
    srai a0, a0, 16
#endif
  .endif
  addi RV_BUDGET, RV_BUDGET, -1
  ram_return

.Lslow\@:
  count_ram_load gpsp_rv32_jit_ram_load_slow
  mv a2, RV_TMP0
  li a0, \kind
  j gpsp_rv32_ram_load_slow
  .size \name, .-\name
  .size \name\()_flow, .-\name\()_flow
  .endm

  ram_load gpsp_rv32_ram_load_u8,  lbu, 0, 0, 3
  ram_load gpsp_rv32_ram_load_u16, lhu, 1, 1, 3
  ram_load gpsp_rv32_ram_load_u32, lw,  2, 3, 6
  ram_load gpsp_rv32_ram_load_s8,  lb,  3, 0, 3
  ram_load gpsp_rv32_ram_load_s16, lh,  4, 1, 3

  .align 2
  .type gpsp_rv32_ram_load_slow, @function
gpsp_rv32_ram_load_slow:
  /* The ROM loader or C bridge returns to the existing fetch path. */
  addi sp, sp, -16
  sw a2, 0(sp)
  slli RV_TMP1, a0, 2
  lla a2, .Lrom_load_targets
  add a2, a2, RV_TMP1
  lw a2, 0(a2)
  lla a0, gpsp_rv32_thumb_memory_helpers
  add RV_TMP1, RV_TMP1, a0
  lw a0, 0(RV_TMP1)
  jalr ra, 0(a2)
  /* The bridge restores ra to its block-exit continuation. */
  lw RV_TMP0, 0(sp)
  addi sp, sp, 16
  bnez a1, .Lexit_load_slow
  addi RV_PC, RV_PC, 2
  jalr zero, 0(RV_TMP0)
.Lexit_load_slow:
  ret
  .size gpsp_rv32_ram_load_slow, .-gpsp_rv32_ram_load_slow

  .pushsection .rodata
  .align 2
.Lrom_load_targets:
  .word gpsp_rv32_rom_load_u8
  .word gpsp_rv32_rom_load_u16
  .word gpsp_rv32_rom_load_u32
  .word gpsp_rv32_rom_load_s8
  .word gpsp_rv32_rom_load_s16
  .popsection
  .option pop

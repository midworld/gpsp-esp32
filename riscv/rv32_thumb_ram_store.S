/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

  .text
  .option push
  .option norvc

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  .macro count_ram_store name
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
  .macro count_ram_store name
  .endm
#endif

  /* Return to RV_TMP0 after a native access at the next Thumb PC. */
  .macro ram_return
  addi RV_PC, RV_PC, 2
  jalr zero, 0(RV_TMP0)
  .endm

  /* RV_TMP1 holds the RAM offset. Check its code tag, then store the data.
     A byte store tests its aligned halfword; a word store both halves. */
  .macro ram_tag_store store_op, kind, tags, data_high, slow, counter
  lui a0, %hi(\tags)
  add RV_TMP1, RV_TMP1, a0
  .if \kind == 5
    addi a0, RV_TMP1, %lo(\tags)
    andi a0, a0, -2
    lhu a0, 0(a0)
  .elseif \kind == 6
    lhu a0, %lo(\tags)(RV_TMP1)
  .else
    lw a0, %lo(\tags)(RV_TMP1)
  .endif
  bnez a0, \slow
  count_ram_store \counter
  lui a0, \data_high
  add RV_TMP1, RV_TMP1, a0
  \store_op a2, %lo(\tags)(RV_TMP1)
  .endm

  /* After an IE or IME write (a0 = io_registers): a deliverable IRQ goes
     to the C path, which repeats the write and raises it. */
  .macro io_irq_check offset, slow
  lw RV_TMP1, 64(RV_STATE)      /* reg[REG_CPSR] */
  andi RV_TMP1, RV_TMP1, 0x80
  bnez RV_TMP1, .Lirq_none\@
  lhu RV_TMP1, 0x208(a0)
  beqz RV_TMP1, .Lirq_none\@
  lw RV_TMP1, 0x200(a0)
  srli a2, RV_TMP1, 16
  and RV_TMP1, RV_TMP1, a2
  slli RV_TMP1, RV_TMP1, 16
  beqz RV_TMP1, .Lirq_none\@
  lhu a2, \offset(a0)
  j \slow
.Lirq_none\@:
  .endm

  /* RV_TMP0 points after the call: at the wait pointer literal, or at the
     site's fetch charge with static waits. ra exits the block. */
  .macro ram_store name, store_op, kind, align_mask, ew_cycles
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
  /* RV_TMP0 is the return address, 16 bytes after the native store site. */
  addi a1, RV_TMP0, -16
  jalr zero, 4(ra)
.Lready\@:
  srli RV_TMP1, a1, 24
  addi RV_TMP1, RV_TMP1, -3
  bnez RV_TMP1, .Lewram\@
  /* Slow paths receive the unaligned address. */
  .if \align_mask
    andi a1, a1, \align_mask
  .endif
  slli RV_TMP1, a1, 17
  srli RV_TMP1, RV_TMP1, 17
  /* IWRAM tags sit 0x8000 below their data. */
  ram_tag_store \store_op, \kind, iwram, 0x8, .Lslow\@, \
      gpsp_rv32_jit_ram_store_fast
  addi RV_BUDGET, RV_BUDGET, -1
  ram_return

.Lewram\@:
  addi RV_TMP1, RV_TMP1, 1
  bnez RV_TMP1, .Lio\@
  .if \align_mask
    andi a1, a1, \align_mask
  .endif
  slli RV_TMP1, a1, 14
  srli RV_TMP1, RV_TMP1, 14
  /* EWRAM tags sit 0x40000 above their data. */
  ram_tag_store \store_op, \kind, ewram + 0x40000, 0xfffc0, \
      .Lslow\@, gpsp_rv32_jit_ram_store_fast
  addi RV_BUDGET, RV_BUDGET, -\ew_cycles
  ram_return

.Lio\@:
  /* I/O registers that only store masked data. Byte stores merge and
     may halt, so they keep the C path. */
  .if \kind == 5
    j .Lslow\@
  .else
    addi RV_TMP1, RV_TMP1, -2
    bnez RV_TMP1, .Lslow\@
    slli RV_TMP1, a1, 8
    .if \kind == 7
      srli RV_TMP1, RV_TMP1, 10
      sltiu a0, RV_TMP1, 256
      beqz a0, .Lslow\@
      slli RV_TMP1, RV_TMP1, 2
      lui a0, %hi(gpsp_rv32_io_write_mask32)
      add a0, a0, RV_TMP1
      lw a0, %lo(gpsp_rv32_io_write_mask32)(a0)
    .else
      srli RV_TMP1, RV_TMP1, 9
      sltiu a0, RV_TMP1, 512
      beqz a0, .Lslow\@
      .if \kind == 6
        addi a0, RV_TMP1, -0x101
        beqz a0, .Lio_if\@
        addi a0, RV_TMP1, -0x100
        beqz a0, .Lio_ie\@
        addi a0, RV_TMP1, -0x104
        beqz a0, .Lio_ime\@
      .endif
      slli RV_TMP1, RV_TMP1, 2
      lui a0, %hi(gpsp_rv32_io_write_mask16)
      add a0, a0, RV_TMP1
      lw a0, %lo(gpsp_rv32_io_write_mask16)(a0)
    .endif
    beqz a0, .Lslow\@
    and a2, a2, a0
    .if \kind == 6
      srli RV_TMP1, RV_TMP1, 1
    .endif
    lui a0, %hi(io_registers)
    add a0, a0, RV_TMP1
    \store_op a2, %lo(io_registers)(a0)
    addi RV_BUDGET, RV_BUDGET, -1
    ram_return
  .endif
  .if \kind == 6
.Lio_if\@:
    /* Writing ones acknowledges; it cannot raise an IRQ. */
    lui a0, %hi(io_registers)
    addi a0, a0, %lo(io_registers)
    lhu RV_TMP1, 0x202(a0)
    not a2, a2
    and RV_TMP1, RV_TMP1, a2
    sh RV_TMP1, 0x202(a0)
    addi RV_BUDGET, RV_BUDGET, -1
    ram_return
.Lio_ie\@:
    lui a0, %hi(io_registers)
    addi a0, a0, %lo(io_registers)
    sh a2, 0x200(a0)
    io_irq_check 0x200, .Lslow\@
    addi RV_BUDGET, RV_BUDGET, -1
    ram_return
.Lio_ime\@:
    lui a0, %hi(io_registers)
    addi a0, a0, %lo(io_registers)
    sh a2, 0x208(a0)
    io_irq_check 0x208, .Lslow\@
    addi RV_BUDGET, RV_BUDGET, -1
    ram_return
  .endif

.Lslow\@:
  count_ram_store gpsp_rv32_jit_ram_store_slow
  mv a0, RV_TMP0
  li RV_TMP0, \kind
  j gpsp_rv32_ram_store_slow
  .size \name, .-\name
  .size \name\()_flow, .-\name\()_flow
  .endm

  ram_store gpsp_rv32_ram_store_u8,  sb, 5,  0, 3
  ram_store gpsp_rv32_ram_store_u16, sh, 6, -2, 3
  ram_store gpsp_rv32_ram_store_u32, sw, 7, -4, 6

  .align 2
  .type gpsp_rv32_ram_store_slow, @function
gpsp_rv32_ram_store_slow:
  /* The C bridge restores ra to the block-exit continuation. */
  addi sp, sp, -16
  sw a0, 0(sp)
  slli RV_TMP1, RV_TMP0, 2
  lla a0, gpsp_rv32_thumb_memory_helpers
  add RV_TMP1, RV_TMP1, a0
  lw a0, 0(RV_TMP1)
  lla RV_TMP1, gpsp_rv32_call_helper
  jalr ra, 0(RV_TMP1)
  lw RV_TMP0, 0(sp)
  addi sp, sp, 16
  bnez a1, .Lexit_store_slow
  addi RV_PC, RV_PC, 2
  jalr zero, 0(RV_TMP0)
.Lexit_store_slow:
  ret
  .size gpsp_rv32_ram_store_slow, .-gpsp_rv32_ram_store_slow
  .option pop

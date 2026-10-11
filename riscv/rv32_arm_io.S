/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

/* ARM I/O fast paths. a1 is the address, a2 the stored value and a0 the
   return address: a0 + 0 jumps to the C path, a0 + 4 continues. Stores
   arrive aligned; unaligned loads rotate, so they take the C path. Only
   RV_TMP0, RV_TMP1 and (for loads) a2 are scratch. */

#define CPSR_OFFSET 64          /* reg[REG_CPSR] */
#define IO_WAIT (ws_cyc_nseq + 4 * 2)

  .text
  .option push
  .option norvc

  /* RV_TMP1 = halfword (shift 9) or word (shift 10) index in the 1 KiB window;
     branches to \miss outside it. */
  .macro io_index shift, limit, miss
  slli RV_TMP1, a1, 8
  srli RV_TMP1, RV_TMP1, \shift
  sltiu RV_TMP0, RV_TMP1, \limit
  beqz RV_TMP0, \miss
  .endm

  .macro io_hit word
  lla RV_TMP0, IO_WAIT + \word
  lbu RV_TMP0, 0(RV_TMP0)
  sub RV_BUDGET, RV_BUDGET, RV_TMP0
  jalr zero, 4(a0)
  .endm

  .align 2
  .globl gpsp_rv32_arm_io_load16
  .type gpsp_rv32_arm_io_load16, @function
gpsp_rv32_arm_io_load16:
  andi RV_TMP0, a1, 1
  bnez RV_TMP0, .Lload16_miss
  io_index 9, 512, .Lload16_miss
  slli RV_TMP0, RV_TMP1, 2
  lui a2, %hi(gpsp_rv32_io_read_mask16)
  add a2, a2, RV_TMP0
  lw a2, %lo(gpsp_rv32_io_read_mask16)(a2)
  beqz a2, .Lload16_miss
  slli RV_TMP1, RV_TMP1, 1
  lui RV_TMP0, %hi(io_registers)
  add RV_TMP0, RV_TMP0, RV_TMP1
  lhu RV_TMP0, %lo(io_registers)(RV_TMP0)
  and a2, a2, RV_TMP0
  lla RV_TMP0, IO_WAIT
  lbu RV_TMP0, 0(RV_TMP0)
  sub RV_BUDGET, RV_BUDGET, RV_TMP0
  mv RV_TMP1, a0
  mv a0, a2
  jalr zero, 4(RV_TMP1)
.Lload16_miss:
  jr a0
  .size gpsp_rv32_arm_io_load16, .-gpsp_rv32_arm_io_load16

  .align 2
  .globl gpsp_rv32_arm_io_load32
  .type gpsp_rv32_arm_io_load32, @function
gpsp_rv32_arm_io_load32:
  andi RV_TMP0, a1, 3
  bnez RV_TMP0, .Lload32_miss
  io_index 10, 256, .Lload32_miss
  slli RV_TMP1, RV_TMP1, 2
  lui a2, %hi(gpsp_rv32_io_read_mask32)
  add a2, a2, RV_TMP1
  lw a2, %lo(gpsp_rv32_io_read_mask32)(a2)
  beqz a2, .Lload32_miss
  lui RV_TMP0, %hi(io_registers)
  add RV_TMP0, RV_TMP0, RV_TMP1
  lw RV_TMP0, %lo(io_registers)(RV_TMP0)
  and a2, a2, RV_TMP0
  lla RV_TMP0, IO_WAIT + 1
  lbu RV_TMP0, 0(RV_TMP0)
  sub RV_BUDGET, RV_BUDGET, RV_TMP0
  mv RV_TMP1, a0
  mv a0, a2
  jalr zero, 4(RV_TMP1)
.Lload32_miss:
  jr a0
  .size gpsp_rv32_arm_io_load32, .-gpsp_rv32_arm_io_load32

  /* After an IE or IME write: leave a deliverable IRQ to the C path, which
     repeats the write and raises it. a2 is reloaded with the stored value. */
  .macro irq_check offset
  lw RV_TMP1, CPSR_OFFSET(RV_STATE)
  andi RV_TMP1, RV_TMP1, 0x80
  bnez RV_TMP1, .Lstore16_hit
  lhu RV_TMP1, 0x208(RV_TMP0)
  beqz RV_TMP1, .Lstore16_hit
  lw RV_TMP1, 0x200(RV_TMP0)
  srli a2, RV_TMP1, 16
  and RV_TMP1, RV_TMP1, a2
  slli RV_TMP1, RV_TMP1, 16
  beqz RV_TMP1, .Lstore16_hit
  lhu a2, \offset(RV_TMP0)
  jr a0
  .endm

  .align 2
  .globl gpsp_rv32_arm_io_store16
  .type gpsp_rv32_arm_io_store16, @function
gpsp_rv32_arm_io_store16:
  io_index 9, 512, .Lstore16_miss
  addi RV_TMP0, RV_TMP1, -0x100
  beqz RV_TMP0, .Lstore16_ie
  addi RV_TMP0, RV_TMP1, -0x101
  beqz RV_TMP0, .Lstore16_if
  addi RV_TMP0, RV_TMP1, -0x104
  beqz RV_TMP0, .Lstore16_ime
  /* Registers that only keep masked data, as in the Thumb path. a2 must
     survive a miss, so the index is rebuilt from a1 afterwards. */
  slli RV_TMP0, RV_TMP1, 2
  lui RV_TMP1, %hi(gpsp_rv32_io_write_mask16)
  add RV_TMP1, RV_TMP1, RV_TMP0
  lw RV_TMP0, %lo(gpsp_rv32_io_write_mask16)(RV_TMP1)
  beqz RV_TMP0, .Lstore16_miss
  and a2, a2, RV_TMP0
  slli RV_TMP1, a1, 8
  srli RV_TMP1, RV_TMP1, 8
  lui RV_TMP0, %hi(io_registers)
  add RV_TMP0, RV_TMP0, RV_TMP1
  sh a2, %lo(io_registers)(RV_TMP0)
  j .Lstore16_hit
.Lstore16_ie:
  lui RV_TMP0, %hi(io_registers)
  addi RV_TMP0, RV_TMP0, %lo(io_registers)
  sh a2, 0x200(RV_TMP0)
  irq_check 0x200
.Lstore16_ime:
  lui RV_TMP0, %hi(io_registers)
  addi RV_TMP0, RV_TMP0, %lo(io_registers)
  sh a2, 0x208(RV_TMP0)
  irq_check 0x208
.Lstore16_if:
  /* Writing ones acknowledges; it cannot raise an IRQ. */
  lui RV_TMP0, %hi(io_registers)
  addi RV_TMP0, RV_TMP0, %lo(io_registers)
  lhu RV_TMP1, 0x202(RV_TMP0)
  not a2, a2
  and RV_TMP1, RV_TMP1, a2
  sh RV_TMP1, 0x202(RV_TMP0)
.Lstore16_hit:
  io_hit 0
.Lstore16_miss:
  jr a0
  .size gpsp_rv32_arm_io_store16, .-gpsp_rv32_arm_io_store16
  .option pop

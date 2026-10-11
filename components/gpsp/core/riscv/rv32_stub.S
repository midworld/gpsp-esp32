/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_register_map.h"

/* RV32 generated-code entry/exit ABI. A budget guard may reenter after itself.
 * Generated code keeps gp, tp and sp unchanged. Use gpsp_rv32_call_helper for
 * C calls. This entry point is not the gpSP frame dispatcher. */

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

  /* Replace NZCV in RV_TMP0 with RV_FLAG_*, each 0 or 1. Clobbers RV_TMP1. */
  .macro pack_nzcv
  slli RV_TMP0, RV_TMP0, 4
  srli RV_TMP0, RV_TMP0, 4
#if GPSP_RV32_USE_ZBA
  sh1add RV_TMP1, RV_FLAG_N, RV_FLAG_Z
  sh1add RV_TMP1, RV_TMP1, RV_FLAG_C
  sh1add RV_TMP1, RV_TMP1, RV_FLAG_V
  slli RV_TMP1, RV_TMP1, 28
  or RV_TMP0, RV_TMP0, RV_TMP1
#else
  slli RV_TMP1, RV_FLAG_N, 31
  or RV_TMP0, RV_TMP0, RV_TMP1
  slli RV_TMP1, RV_FLAG_Z, 30
  or RV_TMP0, RV_TMP0, RV_TMP1
  slli RV_TMP1, RV_FLAG_C, 29
  or RV_TMP0, RV_TMP0, RV_TMP1
  slli RV_TMP1, RV_FLAG_V, 28
  or RV_TMP0, RV_TMP0, RV_TMP1
#endif
  .endm

  /* RV_FLAG_* = N, Z, C, V of RV_TMP0. */
  .macro unpack_nzcv
  srli RV_FLAG_N, RV_TMP0, 31
#if GPSP_RV32_USE_ZBS
  bexti RV_FLAG_Z, RV_TMP0, 30
  bexti RV_FLAG_C, RV_TMP0, 29
  bexti RV_FLAG_V, RV_TMP0, 28
#else
  srli RV_FLAG_Z, RV_TMP0, 30
  andi RV_FLAG_Z, RV_FLAG_Z, 1
  srli RV_FLAG_C, RV_TMP0, 29
  andi RV_FLAG_C, RV_FLAG_C, 1
  srli RV_FLAG_V, RV_TMP0, 28
  andi RV_FLAG_V, RV_FLAG_V, 1
#endif
  .endm

  /* Load or store one guest register at its reg[] slot. With caller=1,
     skip registers that survive a C call. */
  .macro guest_reg op, num, index, caller
  .if !\caller || GPSP_RV32_CALLER_SAVED(\num)
  \op x\num, (\index * 4)(RV_STATE)
  .endif
  .endm

  .macro guest_regs op, caller=0
  guest_reg \op, GPSP_RV32_REG_GUEST_R0, 0, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R1, 1, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R2, 2, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R3, 3, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R4, 4, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R5, 5, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R6, 6, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R7, 7, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R8, 8, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R9, 9, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R10, 10, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R11, 11, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R12, 12, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R13, 13, \caller
  guest_reg \op, GPSP_RV32_REG_GUEST_R14, 14, \caller
  .endm

  /* Save or restore the guest registers that a C call may clobber, in
     consecutive stack words from \offset below \end. */
  .macro guest_stack_reg op, num
  .if GPSP_RV32_CALLER_SAVED(\num)
  \op x\num, .Lguest_slot(sp)
  .set .Lguest_slot, .Lguest_slot + 4
  .endif
  .endm

  .macro guest_stack op, offset, end
  .set .Lguest_slot, \offset
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R0
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R1
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R2
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R3
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R4
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R5
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R6
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R7
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R8
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R9
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R10
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R11
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R12
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R13
  guest_stack_reg \op, GPSP_RV32_REG_GUEST_R14
  .if .Lguest_slot > \end
  .error "guest registers do not fit the stack frame"
  .endif
  .endm

/* Detail builds keep the C update path for their counters. */
#if defined(GPSP_RV32_JIT) && \
    !defined(GPSP_BENCHMARK_JIT_DETAIL) && \
    !defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
#define RV32_LEAN_BUDGET 1
#endif

  /* Negative V tags pending ADD operands in RV_FLAG_N/RV_FLAG_Z. */
  .macro resolve_lazy_add
  bgez RV_FLAG_V, .Llazy_done\@
  add RV_TMP0, RV_FLAG_N, RV_FLAG_Z
  sltu RV_FLAG_C, RV_TMP0, RV_FLAG_N
  xor RV_TMP1, RV_FLAG_N, RV_TMP0
  xor RV_FLAG_V, RV_FLAG_Z, RV_TMP0
  and RV_TMP1, RV_TMP1, RV_FLAG_V
  srli RV_FLAG_V, RV_TMP1, 31
  srli RV_FLAG_N, RV_TMP0, 31
  sltiu RV_FLAG_Z, RV_TMP0, 1
.Llazy_done\@:
  .endm
  .align 2
  .globl gpsp_rv32_run_block
  .type gpsp_rv32_run_block, @function
gpsp_rv32_run_block:
  /* a0: reg[]; a1: remaining cycles; a2: generated block address. */
  addi sp, sp, -64
  /* Save the C callee-saved registers, whatever their JIT roles. */
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

  mv RV_STATE, a0
  mv RV_BUDGET, a1
  lw RV_PC, 60(RV_STATE)
  lw RV_TMP0, 64(RV_STATE)      /* CPSR */
  unpack_nzcv

  guest_regs lw

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  rdcycle RV_TMP0
  sw RV_TMP0, 52(sp)
#if defined(GPSP_RV32_JIT)
  lla RV_TMP0, gpsp_rv32_native_budget_cycles
  lw RV_TMP0, 0(RV_TMP0)
  sw RV_TMP0, 56(sp)
#endif
#endif
  jalr ra, 0(a2)
  /* Direct-load helpers restore ra to this exit after their own call. */
  .globl gpsp_rv32_block_return
gpsp_rv32_block_return:
.Lblock_return:
  /* A guard skips this reset and returns its next native PC in a1. */
  li a1, 0
#if defined(GPSP_RV32_JIT)
  bnez a1, .Lnative_budget
#endif
.Lblock_return_finish:
  resolve_lazy_add
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  rdcycle RV_TMP0
  lw RV_TMP1, 52(sp)
  sub RV_TMP0, RV_TMP0, RV_TMP1
#if defined(GPSP_RV32_JIT)
  lla RV_TMP1, gpsp_rv32_native_budget_cycles
  lw a0, 0(RV_TMP1)
  lw RV_TMP1, 56(sp)
  sub a0, a0, RV_TMP1
  sub RV_TMP0, RV_TMP0, a0
#endif
  lla RV_TMP1, gpsp_rv32_jit_code_cycles
  lw a0, 0(RV_TMP1)
  add a0, a0, RV_TMP0
  sw a0, 0(RV_TMP1)
  /* Carry the low-word wrap into the high word. */
  sltu RV_TMP0, a0, RV_TMP0
  lw a0, 4(RV_TMP1)
  add a0, a0, RV_TMP0
  sw a0, 4(RV_TMP1)
#endif

  guest_regs sw
  sw RV_PC, 60(RV_STATE)

  /* Replace CPSR.NZCV; preserve T, mode and all other CPSR bits. */
  lw RV_TMP0, 64(RV_STATE)
  pack_nzcv
  sw RV_TMP0, 64(RV_STATE)

  mv a0, RV_BUDGET
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
  .size gpsp_rv32_run_block, .-gpsp_rv32_run_block

#if defined(GPSP_RV32_JIT)
  .align 2
.Lnative_budget:
#if defined(RV32_LEAN_BUDGET)
  /* Call update_gba directly unless debt needs the C slicing loop. */
  li RV_TMP0, -64
  blt RV_BUDGET, RV_TMP0, .Lnative_budget_c
  guest_regs sw
  sw RV_PC, 60(RV_STATE)

  resolve_lazy_add
  lw RV_TMP0, 64(RV_STATE)
  pack_nzcv
  sw RV_TMP0, 64(RV_STATE)

  addi sp, sp, -16
  sw a1, 0(sp)
#if defined(GPSP_BENCHMARK)
  /* The dispatcher removes scheduler time from block time. */
  rdcycle RV_TMP0
  sw RV_TMP0, 4(sp)
#endif
  mv a0, RV_BUDGET
  call update_gba
#if defined(GPSP_BENCHMARK)
  rdcycle RV_TMP0
  lw RV_TMP1, 4(sp)
  sub RV_TMP0, RV_TMP0, RV_TMP1
  lla RV_TMP1, gpsp_rv32_native_budget_cycles
  lw a1, 0(RV_TMP1)
  add a1, a1, RV_TMP0
  sw a1, 0(RV_TMP1)
  sltu RV_TMP0, a1, RV_TMP0
  lw a1, 4(RV_TMP1)
  add a1, a1, RV_TMP0
  sw a1, 4(RV_TMP1)
#endif
  lw RV_TMP0, 0(sp)
  addi sp, sp, 16
  /* No status bits: no frame end and no IRQ, so PC, CPSR and guest
     registers are unchanged. Code caches flush only after an alert. */
  srli RV_TMP1, a0, 30
  bnez RV_TMP1, .Lnative_budget_finish
  lla RV_TMP1, gpsp_rv32_pending_alert
  lw RV_TMP1, 0(RV_TMP1)
  bnez RV_TMP1, .Lnative_budget_finish
  lw RV_TMP1, 72(RV_STATE)      /* CPU_HALT_STATE */
  bnez RV_TMP1, .Lnative_budget_finish
  lla RV_TMP1, cheat_master_hook
  lw RV_TMP1, 0(RV_TMP1)
  beq RV_TMP1, RV_PC, .Lnative_budget_finish
  lla RV_TMP1, gpsp_rv32_native_budget_updates
  lw a1, 0(RV_TMP1)
  addi a1, a1, 1
  sw a1, 0(RV_TMP1)

  guest_regs lw, 1
  slli RV_BUDGET, a0, 17
  srli RV_BUDGET, RV_BUDGET, 17
  lla ra, .Lblock_return
  jr RV_TMP0

.Lnative_budget_finish:
  call gpsp_rv32_native_budget_finish
  j .Lbudget_reload

.Lnative_budget_c:
#endif
  /* Keep the guard's native continuation while the scheduler runs. */
  addi sp, sp, -64
  sw ra, 0(sp)
  sw a1, 4(sp)
  guest_stack sw, 8, 64

  guest_regs sw
  sw RV_PC, 60(RV_STATE)

  resolve_lazy_add
  lw RV_TMP0, 64(RV_STATE)
  pack_nzcv
  sw RV_TMP0, 64(RV_STATE)

  mv a0, RV_STATE
  mv a1, RV_BUDGET
  call gpsp_rv32_native_budget_update
  beqz a1, .Lbudget_dispatch

  /* The unchanged guest state lets callee-saved registers stay live. */
  lw ra, 0(sp)
  lw RV_TMP0, 4(sp)
  guest_stack lw, 8, 64
  addi sp, sp, 64
  slli RV_BUDGET, a0, 17
  srli RV_BUDGET, RV_BUDGET, 17
  jr RV_TMP0

.Lbudget_dispatch:
  lw ra, 0(sp)
  addi sp, sp, 64
.Lbudget_reload:
  guest_regs lw
  lw RV_PC, 60(RV_STATE)
  lw RV_TMP0, 64(RV_STATE)
  unpack_nzcv

  slli RV_BUDGET, a0, 17
  srli RV_BUDGET, RV_BUDGET, 17
  bltz a0, .Lbudget_frame
  li a1, 2
  j .Lblock_return_finish
.Lbudget_frame:
  mv RV_BUDGET, a0
  li a1, -1
  j .Lblock_return_finish
#endif

  .align 2
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  .globl gpsp_rv32_call_memory_read_helper
  .type gpsp_rv32_call_memory_read_helper, @function
gpsp_rv32_call_memory_read_helper:
  li RV_TMP0, 1
  j .Lhelper_entry
  .size gpsp_rv32_call_memory_read_helper, .-gpsp_rv32_call_memory_read_helper
#endif
  .globl gpsp_rv32_call_helper
  .type gpsp_rv32_call_helper, @function
gpsp_rv32_call_helper:
  /* a0: callback; a1/a2: guest arguments. ra: resume address in block. */
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  li RV_TMP0, 0
.Lhelper_entry:
  rdcycle RV_TMP1
  addi sp, sp, -48
  sw RV_TMP1, 32(sp)
  sw RV_TMP0, 36(sp)
  sw a1, 40(sp)
#else
  addi sp, sp, -32
#endif
  sw ra, 0(sp)
  sw a0, 4(sp)
  sw RV_BUDGET, 8(sp)           /* C helper may update remaining cycles */
  sw a1, 12(sp)
  sw a2, 16(sp)
  sw RV_PC, 20(sp)              /* PC-change detection */

  guest_regs sw
  sw RV_PC, 60(RV_STATE)

  resolve_lazy_add
  lw RV_TMP0, 64(RV_STATE)
  pack_nzcv
  sw RV_TMP0, 64(RV_STATE)
  andi RV_TMP0, RV_TMP0, 0x3f   /* mode and T bits */
  sw RV_TMP0, 24(sp)
  lw RV_TMP0, 68(RV_STATE)      /* gpSP CPU_MODE */
  sw RV_TMP0, 28(sp)

  lw RV_TMP0, 4(sp)
  mv a0, RV_STATE
  addi a1, sp, 8
  lw a2, 12(sp)
  lw a3, 16(sp)
  jalr ra, 0(RV_TMP0)
  sw a0, 12(sp)                 /* helper value */
  sw a1, 16(sp)                 /* helper-requested exit */

  lw RV_BUDGET, 8(sp)
  lw RV_PC, 60(RV_STATE)
  lw RV_TMP0, 64(RV_STATE)
  unpack_nzcv
  andi RV_TMP0, RV_TMP0, 0x3f
  lw RV_TMP1, 24(sp)
  xor RV_TMP0, RV_TMP0, RV_TMP1
  lw RV_TMP1, 20(sp)
  xor RV_TMP1, RV_TMP1, RV_PC
  or RV_TMP0, RV_TMP0, RV_TMP1
  lw RV_TMP1, 68(RV_STATE)
  lw a1, 28(sp)
  xor RV_TMP1, RV_TMP1, a1
  or RV_TMP0, RV_TMP0, RV_TMP1
  lw a1, 16(sp)
  or a1, a1, RV_TMP0

  guest_regs lw

  lw a0, 12(sp)
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  rdcycle RV_TMP0
  lw RV_TMP1, 32(sp)
  sub RV_TMP0, RV_TMP0, RV_TMP1
  sw RV_TMP0, 44(sp)
  lla RV_TMP1, gpsp_rv32_jit_helper_cycles
  lw a2, 0(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 0(RV_TMP1)
  sltu RV_TMP0, a2, RV_TMP0
  lw a2, 4(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 4(RV_TMP1)

  lla RV_TMP1, gpsp_rv32_jit_helper_calls
  lw a2, 0(RV_TMP1)
  addi a2, a2, 1
  sw a2, 0(RV_TMP1)
  sltiu RV_TMP0, a2, 1
  lw a2, 4(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 4(RV_TMP1)

  lw a2, 36(sp)
  beqz a2, .Lhelper_not_rom_read
  lw a2, 40(sp)
  srli a2, a2, 24
  addi a2, a2, -8
  sltiu a2, a2, 6
  beqz a2, .Lhelper_not_rom_read

  lw RV_TMP0, 44(sp)
  lla RV_TMP1, gpsp_rv32_jit_rom_read_bridge_cycles
  lw a2, 0(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 0(RV_TMP1)
  sltu RV_TMP0, a2, RV_TMP0
  lw a2, 4(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 4(RV_TMP1)

  lla RV_TMP1, gpsp_rv32_jit_rom_read_bridge_calls
  lw a2, 0(RV_TMP1)
  addi a2, a2, 1
  sw a2, 0(RV_TMP1)
  sltiu RV_TMP0, a2, 1
  lw a2, 4(RV_TMP1)
  add a2, a2, RV_TMP0
  sw a2, 4(RV_TMP1)
.Lhelper_not_rom_read:
#endif
  lw RV_TMP0, 0(sp)             /* address after the generated call */
  lla ra, .Lblock_return       /* restore the eventual ret target */
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  addi sp, sp, 48
#else
  addi sp, sp, 32
#endif
  jr RV_TMP0
  .size gpsp_rv32_call_helper, .-gpsp_rv32_call_helper

  .align 2
  .globl gpsp_rv32_thumb_multi_slow_bridge
  .type gpsp_rv32_thumb_multi_slow_bridge, @function
gpsp_rv32_thumb_multi_slow_bridge:
  /* A1 holds the opcode; the helper returns to the block exit in RA. */
  lla a0, gpsp_rv32_thumb_multi_slow
  li a2, 0
  j gpsp_rv32_call_helper
  .size gpsp_rv32_thumb_multi_slow_bridge, .-gpsp_rv32_thumb_multi_slow_bridge

  .section .text.gpsp_rv32_thumb_side_preflight_stub,"ax",@progbits
  .align 2
  .globl gpsp_rv32_thumb_side_preflight_stub
  .type gpsp_rv32_thumb_side_preflight_stub, @function
gpsp_rv32_thumb_side_preflight_stub:
  /* RV_TMP1 points to the targets and wait byte; a0 packs the two counts. */
  bgtz RV_BUDGET, .Lside_budget
  /* Retry this adapter after the boundary. */
  addi a1, RV_TMP1, -12
  jalr zero, ra, 4
.Lside_budget:
  srli a1, a0, 5                 /* Remaining Thumb instructions. */
  andi a0, a0, 31                /* Instructions before this side entry. */
  lw RV_TMP0, 8(RV_TMP1)
  lbu RV_TMP0, 0(RV_TMP0)
  mul a1, a1, RV_TMP0
  bge RV_BUDGET, a1, .Lside_fast
  lw RV_TMP0, 4(RV_TMP1)        /* One-instruction slow path. */
  jr RV_TMP0
.Lside_fast:
  sub RV_BUDGET, RV_BUDGET, a1
  slli a0, a0, 1
  sub RV_PC, RV_PC, a0          /* Shared finish uses the original head. */
  lw RV_TMP0, 0(RV_TMP1)
  jr RV_TMP0
  .size gpsp_rv32_thumb_side_preflight_stub, .-gpsp_rv32_thumb_side_preflight_stub
  .option pop

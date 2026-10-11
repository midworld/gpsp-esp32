/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_system.h"

#include "../common.h"
#include "rv32_arm_alu.h"
#include "rv32_arm_branch.h"
#include "rv32_register_map.h"

#include <string.h>

static uint32_t pending_alert;

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
extern uint64_t gpsp_rv32_jit_system_calls;
#define count_system_call() (gpsp_rv32_jit_system_calls++)
#else
#define count_system_call() ((void)0)
#endif

uint32_t gpsp_rv32_arm_system_take_alert(void)
{
  uint32_t alert = pending_alert;
  pending_alert = 0;
  return alert;
}

static void finish_fetch(u32 *state, uint32_t *cycles)
{
  *cycles -= ws_cyc_seq[(state[REG_PC] >> 24) & 15][1];
  if (state[REG_PC] == idle_loop_target_pc && (int32_t)*cycles > 0)
    *cycles = 0;
}

static uint64_t msr_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t value, uint32_t control)
{
  count_system_call();
  u32 *state = (u32 *)(void *)regs;
  unsigned pfield = control & 3;
  state[REG_PC] += 4;
  if (control & 4)
  {
    uint32_t mask = spsr_masks[pfield];
    u32 *saved = &REG_SPSR(state[CPU_MODE]);
    *saved = (value & mask) | (*saved & ~mask);
  }
  else
  {
    uint32_t mask = cpsr_masks[pfield][PRIVMODE(state[CPU_MODE])];
    state[REG_CPSR] = (value & mask) | (state[REG_CPSR] & ~mask);
    if (mask & 0xff)
    {
      set_cpu_mode(cpu_modes[state[REG_CPSR] & 15]);
      if (check_and_raise_interrupts())
      {
        *cycles -= take_irq_entry_cycles();
        return GPSP_RV32_HELPER_EXIT;
      }
    }
  }
  finish_fetch(state, cycles);
  return GPSP_RV32_HELPER_EXIT;
}

static uint64_t swi_effect(uint32_t *regs, uint32_t *cycles, bool thumb)
{
  count_system_call();
  u32 *state = (u32 *)(void *)regs;
  uint32_t pc = state[REG_PC];
  uint32_t cpsr = state[REG_CPSR];
  state[REG_BUS_VALUE] = 0xe3a02004;
  REG_MODE(MODE_SUPERVISOR)[6] = pc + (thumb ? 2 : 4);
  REG_SPSR(MODE_SUPERVISOR) = cpsr;
  state[REG_PC] = 8;
  state[REG_CPSR] = (cpsr & ~0x3fu) | 0x93u;
  set_cpu_mode(MODE_SUPERVISOR);
  if (!thumb)
    finish_fetch(state, cycles);
  return GPSP_RV32_HELPER_EXIT;
}

static uint64_t swi_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t unused0, uint32_t unused1)
{
  (void)unused0;
  (void)unused1;
  return swi_effect(regs, cycles, false);
}

static uint64_t thumb_swi_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t unused0, uint32_t unused1)
{
  (void)unused0;
  (void)unused1;
  return swi_effect(regs, cycles, true);
}

static uint64_t exception_return_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t raw_target, uint32_t unused)
{
  count_system_call();
  u32 *state = (u32 *)(void *)regs;
  (void)unused;
  state[REG_PC] = raw_target;
  if (state[CPU_MODE] != MODE_USER && state[CPU_MODE] != MODE_SYSTEM)
  {
    state[REG_CPSR] = REG_SPSR(state[CPU_MODE]);
    set_cpu_mode(cpu_modes[state[REG_CPSR] & 15]);
    if (check_and_raise_interrupts())
    {
      *cycles -= take_irq_entry_cycles();
      return GPSP_RV32_HELPER_EXIT;
    }
  }
  state[REG_PC] &=
      ((state[REG_CPSR] & 0x20) ? ~1u : ~3u);
  if (!(state[REG_CPSR] & 0x20))
    finish_fetch(state, cycles);
  return GPSP_RV32_HELPER_EXIT;
}

static uint64_t swap_memory(u32 *state, uint32_t *cycles,
    uint32_t address, uint32_t value, bool byte)
{
  count_system_call();
  unsigned width = byte ? 0 : 1;
  if (address < 0x10000000u)
    *cycles -= 2u * ws_cyc_nseq[address >> 24][width];

  /* Keep the instruction PC visible to BIOS and open-bus reads. */
  uint32_t previous = byte ? read_memory8(address) : read_memory32(address);
  cpu_alert_type alert = byte ? write_memory8(address, (u8)value) :
      write_memory32(address & ~3u, value);
  pending_alert |= alert;
  --*cycles; /* ARM7TDMI SWP internal cycle. */
  state[REG_PC] += 4;
  finish_fetch(state, cycles);
  return (uint64_t)previous | GPSP_RV32_HELPER_EXIT;
}

static uint64_t swap_byte_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t address, uint32_t value)
{
  return swap_memory((u32 *)(void *)regs, cycles, address, value, true);
}

static uint64_t swap_word_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t address, uint32_t value)
{
  return swap_memory((u32 *)(void *)regs, cycles, address, value, false);
}

typedef enum
{
  SYSTEM_NONE,
  SYSTEM_MRS,
  SYSTEM_MSR_REG,
  SYSTEM_MSR_IMM,
  SYSTEM_SWI,
  SYSTEM_SWAP,
  SYSTEM_EXCEPTION_RETURN,
} system_kind;

static system_kind decode(uint32_t opcode)
{
  if ((opcode & 0x0fbf0fffu) == 0x010f0000u)
    return SYSTEM_MRS;
  if ((opcode & 0x0fb0fff0u) == 0x0120f000u)
    return SYSTEM_MSR_REG;
  if ((opcode & 0x0fb0f000u) == 0x0320f000u)
    return SYSTEM_MSR_IMM;
  if ((opcode & 0x0fb00ff0u) == 0x01000090u)
    return SYSTEM_SWAP;
  if ((opcode & 0x0f000000u) == 0x0f000000u)
    return SYSTEM_SWI;
  if ((opcode & 0x0c10f000u) == 0x0010f000u &&
      (((opcode >> 21) & 15) < 8 || ((opcode >> 21) & 15) > 11) &&
      ((opcode & 0x02000000u) || (opcode & 0x90) != 0x90))
    return SYSTEM_EXCEPTION_RETURN;
  return SYSTEM_NONE;
}

static bool emit_constant(gpsp_rv32_code *code, unsigned rd, uint32_t value)
{
  int low = (int)(value & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((value + 0x800u) >> 12) & 0xfffff;
  if (!upper)
    return gpsp_rv32_addi(code, rd, X0, low);
  return gpsp_rv32_lui(code, rd, upper) &&
      (!low || gpsp_rv32_addi(code, rd, rd, low));
}

static bool emit_address(gpsp_rv32_code *code, unsigned rd, uintptr_t value)
{
  return value <= UINT32_MAX && emit_constant(code, rd, (uint32_t)value);
}

/* index = base + index * 4 */
static bool emit_word_index(gpsp_rv32_code *code, unsigned index,
    unsigned base)
{
#if GPSP_RV32_USE_ZBA
  return gpsp_rv32_sh2add(code, index, index, base);
#else
  return gpsp_rv32_slli(code, index, index, 2) &&
      gpsp_rv32_add(code, index, index, base);
#endif
}

static bool emit_mrs(gpsp_rv32_code *code, uint32_t opcode,
    uintptr_t spsr_base)
{
  unsigned rd = (opcode >> 12) & 15;
  if (opcode & 0x00400000u)
  {
    return gpsp_rv32_lw(code, A0, RV_STATE, CPU_MODE * 4) &&
        gpsp_rv32_andi(code, A0, A0, 15) &&
        emit_address(code, A1, spsr_base) &&
        emit_word_index(code, A0, A1) &&
        gpsp_rv32_lw(code, guest_reg_map[rd], A0, 0);
  }

  /* CPSR.NZCV live in RV_FLAG_* until the next helper call or block exit. */
#if GPSP_RV32_USE_ZBA
  return gpsp_rv32_lw(code, A0, RV_STATE, GPSP_RV32_CPSR_OFFSET) &&
      gpsp_rv32_slli(code, A0, A0, 4) &&
      gpsp_rv32_srli(code, A0, A0, 4) &&
      gpsp_rv32_sh1add(code, RV_TMP0, RV_FLAG_N, RV_FLAG_Z) &&
      gpsp_rv32_sh1add(code, RV_TMP0, RV_TMP0, RV_FLAG_C) &&
      gpsp_rv32_sh1add(code, RV_TMP0, RV_TMP0, RV_FLAG_V) &&
      gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, 28) &&
      gpsp_rv32_or(code, guest_reg_map[rd], A0, RV_TMP0);
#else
  return gpsp_rv32_lw(code, A0, RV_STATE, GPSP_RV32_CPSR_OFFSET) &&
      gpsp_rv32_slli(code, A0, A0, 4) &&
      gpsp_rv32_srli(code, A0, A0, 4) &&
      gpsp_rv32_slli(code, RV_TMP0, RV_FLAG_N, 31) &&
      gpsp_rv32_or(code, A0, A0, RV_TMP0) &&
      gpsp_rv32_slli(code, RV_TMP0, RV_FLAG_Z, 30) &&
      gpsp_rv32_or(code, A0, A0, RV_TMP0) &&
      gpsp_rv32_slli(code, RV_TMP0, RV_FLAG_C, 29) &&
      gpsp_rv32_or(code, A0, A0, RV_TMP0) &&
      gpsp_rv32_slli(code, RV_TMP0, RV_FLAG_V, 28) &&
      gpsp_rv32_or(code, guest_reg_map[rd], A0, RV_TMP0);
#endif
}

static uint32_t msr_immediate(uint32_t opcode)
{
  unsigned rotate = ((opcode >> 8) & 15) * 2;
  uint32_t value = opcode & 0xff;
  return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

static bool emit_msr_value(gpsp_rv32_code *code, uint32_t opcode,
    system_kind kind)
{
  if (kind == SYSTEM_MSR_IMM)
    return emit_constant(code, A1, msr_immediate(opcode));
  return gpsp_rv32_addi(code, A1, guest_reg_map[opcode & 15], 0);
}

static bool emit_msr_flags(gpsp_rv32_code *code, uint32_t opcode,
    system_kind kind)
{
  if (kind == SYSTEM_MSR_IMM)
  {
    uint32_t value = msr_immediate(opcode);
    return gpsp_rv32_addi(code, RV_FLAG_N, X0, (value >> 31) & 1) &&
        gpsp_rv32_addi(code, RV_FLAG_Z, X0, (value >> 30) & 1) &&
        gpsp_rv32_addi(code, RV_FLAG_C, X0, (value >> 29) & 1) &&
        gpsp_rv32_addi(code, RV_FLAG_V, X0, (value >> 28) & 1);
  }
  unsigned source = guest_reg_map[opcode & 15];
#if GPSP_RV32_USE_ZBS
  return gpsp_rv32_srli(code, RV_FLAG_N, source, 31) &&
      gpsp_rv32_bexti(code, RV_FLAG_Z, source, 30) &&
      gpsp_rv32_bexti(code, RV_FLAG_C, source, 29) &&
      gpsp_rv32_bexti(code, RV_FLAG_V, source, 28);
#else
  return gpsp_rv32_srli(code, RV_FLAG_N, source, 31) &&
      gpsp_rv32_srli(code, RV_FLAG_Z, source, 30) &&
      gpsp_rv32_andi(code, RV_FLAG_Z, RV_FLAG_Z, 1) &&
      gpsp_rv32_srli(code, RV_FLAG_C, source, 29) &&
      gpsp_rv32_andi(code, RV_FLAG_C, RV_FLAG_C, 1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, source, 28) &&
      gpsp_rv32_andi(code, RV_FLAG_V, RV_FLAG_V, 1);
#endif
}

static bool emit_msr_spsr(gpsp_rv32_code *code, uint32_t opcode,
    system_kind kind, unsigned pfield, uintptr_t spsr_base)
{
  if (!pfield)
    return true;

  return emit_msr_value(code, opcode, kind) &&
      gpsp_rv32_lw(code, A0, RV_STATE, CPU_MODE * 4) &&
      gpsp_rv32_andi(code, A0, A0, 15) &&
      emit_address(code, A2, spsr_base) &&
      emit_word_index(code, A0, A2) &&
      gpsp_rv32_lw(code, A2, A0, 0) &&
      emit_constant(code, RV_TMP0, spsr_masks[pfield]) &&
      gpsp_rv32_and(code, A1, A1, RV_TMP0) &&
#if GPSP_RV32_USE_ZBB
      gpsp_rv32_andn(code, A2, A2, RV_TMP0) &&
#else
      gpsp_rv32_xori(code, RV_TMP0, RV_TMP0, -1) &&
      gpsp_rv32_and(code, A2, A2, RV_TMP0) &&
#endif
      gpsp_rv32_or(code, A2, A2, A1) &&
      gpsp_rv32_sw(code, A2, A0, 0);
}

static bool emit_skip_advance(gpsp_rv32_code *code, uint32_t pc)
{
  uint32_t next = pc + 4;
  uintptr_t wait = (uintptr_t)&ws_cyc_seq[(next >> 24) & 15][1];
  if (!gpsp_rv32_addi(code, RV_PC, RV_PC, 4) ||
      !emit_address(code, RV_TMP0, wait) ||
      !gpsp_rv32_lbu(code, RV_TMP1, RV_TMP0, 0) ||
      !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1))
    return false;
  if (next == idle_loop_target_pc)
  {
    uint8_t *spent = code->ptr;
    if (!gpsp_rv32_bge(code, X0, RV_BUDGET, 0) ||
        !gpsp_rv32_addi(code, RV_BUDGET, X0, 0) ||
        !gpsp_rv32_patch_branch(code, spent, code->ptr))
      return false;
  }
  return gpsp_rv32_jalr(code, X0, RA, 0);
}

extern void gpsp_rv32_system_exit_shared(void);

static bool emit_call_exit(gpsp_rv32_code *code, uintptr_t callback,
    unsigned result_register)
{
  if (!emit_address(code, A0, callback) ||
      !emit_address(code, RV_TMP1, (uintptr_t)gpsp_rv32_call_helper) ||
      !gpsp_rv32_jalr(code, RA, RV_TMP1, 0) ||
      (result_register != 15 &&
       !gpsp_rv32_addi(code, guest_reg_map[result_register], A0, 0)))
    return false;
  /* SWP may leave an SMC alert for the dispatcher. Other helpers only
     change PC and CPSR, so a cached target can run directly. */
  if (result_register == 15)
    return emit_address(code, RV_TMP1,
            (uintptr_t)gpsp_rv32_system_exit_shared) &&
        gpsp_rv32_jalr(code, X0, RV_TMP1, 0);
  return gpsp_rv32_jalr(code, X0, RA, 0);
}

static bool emit_div_body(gpsp_rv32_code *code, bool swapped)
{
  unsigned numerator = guest_reg_map[swapped ? 1 : 0];
  unsigned denominator = guest_reg_map[swapped ? 0 : 1];
  /* Keep both inputs live until DIV and REM have read them. */
  return gpsp_rv32_div(code, A0, numerator, denominator) &&
      gpsp_rv32_rem(code, A1, numerator, denominator) &&
      gpsp_rv32_srai(code, A2, A0, 31) &&
      gpsp_rv32_xor(code, guest_reg_map[3], A0, A2) &&
      gpsp_rv32_sub(code, guest_reg_map[3], guest_reg_map[3], A2) &&
      gpsp_rv32_addi(code, guest_reg_map[0], A0, 0) &&
      gpsp_rv32_addi(code, guest_reg_map[1], A1, 0) &&
      emit_constant(code, A2, 0xe3a02004u) &&
      gpsp_rv32_sw(code, A2, RV_STATE, REG_BUS_VALUE * 4) &&
      gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -64);
}

static bool emit_hle_div(gpsp_rv32_code *code, bool thumb, bool swapped)
{
  unsigned denominator = guest_reg_map[swapped ? 0 : 1];
  uint8_t *zero = code->ptr;
  if (!gpsp_rv32_beq(code, denominator, X0, 0) ||
      !emit_div_body(code, swapped))
    return false;

  uint8_t *done = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0))
    return false;
  uint8_t *slow = code->ptr;
  /* BIOS division can hang on zero; preserve that path instead of using
   * RV32M's defined divide-by-zero result. */
  return emit_call_exit(code, (uintptr_t)(thumb ? thumb_swi_helper :
      swi_helper), 15) &&
      gpsp_rv32_patch_branch(code, zero, slow) &&
      gpsp_rv32_patch_branch(code, done, code->ptr);
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
  if (instruction_pc & 3)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;

  system_kind kind = decode(opcode);
  if (kind == SYSTEM_NONE)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
  unsigned condition = opcode >> 28;
  if (condition == 15)
    return GPSP_RV32_ARM_SYSTEM_EMITTED;

  unsigned rd = (opcode >> 12) & 15;
  if ((kind == SYSTEM_MRS || kind == SYSTEM_SWAP) && rd == 15)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
  if ((kind == SYSTEM_MSR_REG || kind == SYSTEM_SWAP) &&
      (opcode & 15) == 15)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
  if (kind == SYSTEM_SWAP && ((opcode >> 16) & 15) == 15)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;

  unsigned pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);
  unsigned swi_number = (opcode >> 16) & 0xff;
  bool hle_div = kind == SYSTEM_SWI && (swi_number == 6 || swi_number == 7);
  bool inline_operation = kind == SYSTEM_MRS ||
      hle_div ||
      ((kind == SYSTEM_MSR_REG || kind == SYSTEM_MSR_IMM) &&
       ((opcode & 0x00400000u) || pfield == 0 || pfield == 2));

  uint32_t words[192];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  gpsp_rv32_arm_condition_skip skip = {{NULL, NULL}, 0};
  if (condition != 14 && kind != SYSTEM_EXCEPTION_RETURN &&
      !gpsp_rv32_emit_arm_condition_skip(&staged, condition, &skip))
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;

  bool ok = true;
  if (kind == SYSTEM_MRS)
    ok = emit_mrs(&staged, opcode, (uintptr_t)&spsr[0]);
  else if (kind == SYSTEM_MSR_REG || kind == SYSTEM_MSR_IMM)
  {
    if (opcode & 0x00400000u)
      ok = emit_msr_spsr(&staged, opcode, kind, pfield,
          (uintptr_t)&spsr[0]);
    else if (inline_operation)
      ok = pfield != 2 || emit_msr_flags(&staged, opcode, kind);
    else
      ok = emit_msr_value(&staged, opcode, kind) &&
          gpsp_rv32_addi(&staged, A2, X0, pfield |
              ((opcode & 0x00400000u) ? 4 : 0)) &&
          emit_call_exit(&staged, (uintptr_t)msr_helper, 15);
  }
  else if (kind == SYSTEM_SWI)
    ok = hle_div ? emit_hle_div(&staged, false, swi_number == 7) :
        emit_call_exit(&staged, (uintptr_t)swi_helper, 15);
  else if (kind == SYSTEM_EXCEPTION_RETURN)
  {
    gpsp_rv32_arm_alu_result alu = gpsp_rv32_emit_arm_alu(&staged,
        opcode & ~0x00100000u, instruction_pc);
    if (alu == GPSP_RV32_ARM_ALU_BUFFER_FULL)
      return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
    if (alu != GPSP_RV32_ARM_ALU_EXIT)
      return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
    uint8_t *not_taken = staged.ptr;
    ok = gpsp_rv32_beq(&staged, A1, X0, 0) &&
        gpsp_rv32_addi(&staged, A1, A2, 0) &&
        emit_call_exit(&staged, (uintptr_t)exception_return_helper, 15) &&
        gpsp_rv32_patch_branch(&staged, not_taken, staged.ptr) &&
        emit_skip_advance(&staged, instruction_pc);
  }
  else
  {
    unsigned rn = (opcode >> 16) & 15;
    unsigned rm = opcode & 15;
    ok = gpsp_rv32_addi(&staged, A1, guest_reg_map[rn], 0) &&
        gpsp_rv32_addi(&staged, A2, guest_reg_map[rm], 0) &&
        emit_call_exit(&staged, (uintptr_t)((opcode & 0x00400000u) ?
            swap_byte_helper : swap_word_helper), rd);
  }
  if (!ok ||
      (condition != 14 &&
       kind != SYSTEM_EXCEPTION_RETURN &&
       !gpsp_rv32_patch_arm_condition_skip(&staged, &skip, staged.ptr)) ||
      (!inline_operation && condition != 14 &&
       kind != SYSTEM_EXCEPTION_RETURN &&
       !emit_skip_advance(&staged, instruction_pc)))
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return inline_operation ? GPSP_RV32_ARM_SYSTEM_EMITTED :
      GPSP_RV32_ARM_SYSTEM_EXIT;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_thumb_swi(
    gpsp_rv32_code *code, uint16_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
  if ((instruction_pc & 1) || (opcode & 0xff00) != 0xdf00)
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;

  uint32_t words[32];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  bool hle_div = (opcode & 0xff) == 6 || (opcode & 0xff) == 7;
  if (!(hle_div ? emit_hle_div(&staged, true, (opcode & 0xff) == 7) :
          emit_call_exit(&staged, (uintptr_t)thumb_swi_helper, 15)))
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_SYSTEM_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return hle_div ? GPSP_RV32_ARM_SYSTEM_EMITTED :
      GPSP_RV32_ARM_SYSTEM_EXIT;
}

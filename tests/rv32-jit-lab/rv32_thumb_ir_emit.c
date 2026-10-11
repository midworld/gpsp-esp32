/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_thumb_ir_emit.h"

#include "rv32_block_link.h"
#include "rv32_fallback.h"
#include "rv32_register_map.h"

#include <limits.h>
#include <string.h>

static int address_low(uint32_t address)
{
  int low = address & 0xfff;
  return low >= 0x800 ? low - 0x1000 : low;
}

static uint32_t address_upper(uint32_t address)
{
  return ((address + 0x800u) >> 12) & 0xfffff;
}

static bool emit_wait(gpsp_rv32_code *code, uint32_t seq, uint32_t nseq,
    bool need_nseq)
{
  uint32_t upper = address_upper(seq);
  if ((upper && !gpsp_rv32_lui(code, RV_TMP0, upper)) ||
      !gpsp_rv32_lbu(code, A0, upper ? RV_TMP0 : X0, address_low(seq)))
    return false;
  if (!need_nseq)
    return true;

  uint32_t other_upper = address_upper(nseq);
  return (upper == other_upper || !other_upper ||
      gpsp_rv32_lui(code, RV_TMP0, other_upper)) &&
      gpsp_rv32_lbu(code, A1, other_upper ? RV_TMP0 : X0,
          address_low(nseq));
}

static bool emit_guard(gpsp_rv32_code *code)
{
  /* Keep the existing native resume contract when the budget is exhausted. */
  uint8_t *skip = code->ptr;
  return gpsp_rv32_blt(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_jalr(code, A1, RA, 4) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_budget_gate(gpsp_rv32_code *code, uint8_t **site)
{
  uint8_t *skip = code->ptr;
  if (!gpsp_rv32_blt(code, RV_TMP0, RV_BUDGET, 0))
    return false;
  *site = code->ptr;
  return gpsp_rv32_jal(code, X0, 0) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_jump_to_offset(gpsp_rv32_code *code, size_t base_offset,
    size_t target_offset)
{
  int32_t from = (int32_t)(base_offset + (size_t)(code->ptr - code->start));
  return gpsp_rv32_jal(code, X0, (int32_t)target_offset - from);
}

static bool emit_budget_gate_to_offset(gpsp_rv32_code *code,
    size_t base_offset, size_t cold_offset)
{
  uint8_t *skip = code->ptr;
  return gpsp_rv32_blt(code, RV_TMP0, RV_BUDGET, 0) &&
      emit_jump_to_offset(code, base_offset, cold_offset) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool supported_region(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_ir_environment *environment)
{
  if (!ir || !environment || ir->count != 4 ||
      !(environment->entry_mask & 1) ||
      (environment->entry_mask & ~0x0f) ||
      !environment->sequential_wait_address ||
      !environment->nonsequential_wait_address ||
      !environment->interpreter_stub_address ||
      (environment->interpreter_stub_address & 3))
    return false;

  const gpsp_rv32_thumb_node *nodes = ir->nodes;
  if (nodes[0].kind != GPSP_RV32_THUMB_IR_TST ||
      nodes[1].kind != GPSP_RV32_THUMB_IR_BEQ ||
      nodes[1].taken != 3 ||
      nodes[2].kind != GPSP_RV32_THUMB_IR_MOV_IMM ||
      nodes[3].kind != GPSP_RV32_THUMB_IR_MOV_IMM ||
      nodes[0].fallthrough != 1 || nodes[1].fallthrough != 2 ||
      nodes[2].fallthrough != 3 || nodes[3].fallthrough != 4 ||
      (nodes[0].flags_live_out & GPSP_RV32_THUMB_FLAG_N) ||
      !(nodes[0].flags_live_out & GPSP_RV32_THUMB_FLAG_Z) ||
      (nodes[1].flags_live_out & GPSP_RV32_THUMB_FLAG_Z) ||
      (nodes[2].flags_live_out &
          (GPSP_RV32_THUMB_FLAG_N | GPSP_RV32_THUMB_FLAG_Z)) ||
      (ir->pc >> 24) != ((ir->pc + 8) >> 24))
    return false;

  for (unsigned i = 1; i <= 4; i++)
    if (environment->idle_loop_target_pc == ir->pc + i * 2)
      return false;
  return true;
}

static bool emit_side_entry(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_ir_environment *environment, unsigned entry,
    size_t base_offset, size_t mov1_offset, size_t mov2_offset,
    size_t cold_offset, bool cold_known, uint8_t **cold_site)
{
  *cold_site = NULL;
  if (entry == 1)
  {
    uint8_t *z_clear;
    uint8_t *not_taken;
    /* External BEQ uses the incoming canonical Z, not the skipped TST. */
    if (!emit_guard(code) ||
        !emit_wait(code, environment->sequential_wait_address,
            environment->nonsequential_wait_address, true) ||
        !gpsp_rv32_slli(code, RV_TMP0, A0, 1) ||
        !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, A1) ||
        !(cold_known ?
          emit_budget_gate_to_offset(code, base_offset, cold_offset) :
          emit_budget_gate(code, cold_site)) ||
        !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP0) ||
        !gpsp_rv32_addi(code, RV_PC, RV_PC, -2))
      return false;
    z_clear = code->ptr;
    if (!gpsp_rv32_beq(code, RV_FLAG_Z, X0, 0) ||
        !emit_jump_to_offset(code, base_offset, mov2_offset))
      return false;
    not_taken = code->ptr;
    return gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, A0) &&
        emit_jump_to_offset(code, base_offset, mov1_offset) &&
        gpsp_rv32_patch_branch(code, z_clear, not_taken);
  }
  if (entry == 2)
  {
    if (!emit_guard(code) ||
        !emit_wait(code, environment->sequential_wait_address, 0, false) ||
        !gpsp_rv32_addi(code, RV_TMP0, A0, 0) ||
        !(cold_known ?
          emit_budget_gate_to_offset(code, base_offset, cold_offset) :
          emit_budget_gate(code, cold_site)) ||
        !gpsp_rv32_slli(code, RV_TMP0, A0, 1) ||
        !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP0) ||
        !gpsp_rv32_addi(code, RV_PC, RV_PC, -4))
      return false;
    return emit_jump_to_offset(code, base_offset, mov1_offset);
  }
  if (entry == 3)
  {
    if (!emit_guard(code) ||
        !emit_wait(code, environment->sequential_wait_address, 0, false) ||
        !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, A0) ||
        !gpsp_rv32_addi(code, RV_PC, RV_PC, -6))
      return false;
    return emit_jump_to_offset(code, base_offset, mov2_offset);
  }
  return false;
}

bool gpsp_rv32_thumb_ir_emit(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_ir_environment *environment,
    gpsp_rv32_code *output, gpsp_rv32_thumb_ir_layout *layout)
{
  if (!supported_region(ir, environment) || !output || !layout ||
      !output->start || !output->ptr || !output->end ||
      ((uintptr_t)output->ptr & 3) || output->ptr != output->start ||
      output->ptr > output->end)
    return false;

  /* The four-op shape, all entries, and distinct wait pages fit here. */
  uint32_t words[64];
  gpsp_rv32_code code;
  gpsp_rv32_code_child(&code, output, words, sizeof(words));
  uint8_t *cold_sites[3];
  unsigned cold_count = 0;
  uint8_t *head_to_mov2;
  uint8_t *mov1;
  uint8_t *mov2;
  uint8_t *exit_link;
  uint8_t *entry[4] = {code.ptr, NULL, NULL, NULL};
  const gpsp_rv32_thumb_node *nodes = ir->nodes;

  /* Preflight the longest prefix before the last instruction: N + 3S. */
  if (!emit_guard(&code) ||
      !emit_wait(&code, environment->sequential_wait_address,
          environment->nonsequential_wait_address, true) ||
      !gpsp_rv32_slli(&code, RV_TMP0, A0, 1) ||
      !gpsp_rv32_add(&code, RV_TMP0, RV_TMP0, A0) ||
      !gpsp_rv32_add(&code, RV_TMP0, RV_TMP0, A1) ||
      !emit_budget_gate(&code, &cold_sites[cold_count++]) ||
      !gpsp_rv32_sub(&code, RV_BUDGET, RV_BUDGET, RV_TMP0))
    return false;

  unsigned test_reg = guest_reg_map[nodes[0].rd];
  if (nodes[0].rd != nodes[0].rs)
  {
    if (!gpsp_rv32_and(&code, RV_TMP1, test_reg,
            guest_reg_map[nodes[0].rs]))
      return false;
    test_reg = RV_TMP1;
  }
  head_to_mov2 = code.ptr;
  if (!gpsp_rv32_beq(&code, test_reg, X0, 0) ||
      !gpsp_rv32_sub(&code, RV_BUDGET, RV_BUDGET, A0))
    return false;

  mov1 = code.ptr;
  if (!gpsp_rv32_addi(&code, guest_reg_map[nodes[2].rd], X0,
          nodes[2].imm))
    return false;
  mov2 = code.ptr;
  if (!gpsp_rv32_addi(&code, guest_reg_map[nodes[3].rd], X0,
          nodes[3].imm) ||
      !gpsp_rv32_addi(&code, RV_FLAG_N, X0, 0) ||
      !gpsp_rv32_addi(&code, RV_FLAG_Z, X0, nodes[3].imm == 0) ||
      !gpsp_rv32_addi(&code, RV_PC, RV_PC, 8) ||
      !gpsp_rv32_emit_link_slot(&code, &exit_link))
    return false;

  for (unsigned i = 1; i < 4; i++)
  {
    if (!(environment->entry_mask & (1U << i)))
      continue;
    entry[i] = code.ptr;
    uint8_t *cold_site;
    if (!emit_side_entry(&code, environment, i, 0,
            (size_t)(mov1 - code.start), (size_t)(mov2 - code.start),
            0, false, &cold_site))
      return false;
    if (cold_site)
      cold_sites[cold_count++] = cold_site;
  }

  uint8_t *cold = code.ptr;
  if (gpsp_rv32_emit_interpreter_step_stub(&code,
          environment->interpreter_stub_address) !=
      GPSP_RV32_FALLBACK_EMITTED ||
      !gpsp_rv32_patch_branch(&code, head_to_mov2, mov2))
    return false;
  for (unsigned i = 0; i < cold_count; i++)
    if (!gpsp_rv32_patch_branch(&code, cold_sites[i], cold))
      return false;

  size_t size = (size_t)(code.ptr - code.start);
  if (size > UINT16_MAX || (size_t)(output->end - output->ptr) < size)
    return false;
  gpsp_rv32_thumb_ir_layout result;
  for (unsigned i = 0; i < 4; i++)
    result.entry_offset[i] = entry[i] ?
        (uint16_t)(entry[i] - code.start) : UINT16_MAX;
  result.exit_link_offset = (uint16_t)(exit_link - code.start);
  result.mov1_offset = (uint16_t)(mov1 - code.start);
  result.mov2_offset = (uint16_t)(mov2 - code.start);
  result.cold_offset = (uint16_t)(cold - code.start);
  result.code_bytes = (uint16_t)size;
  result.staging_bytes = sizeof(words);
  memcpy(output->ptr, words, size);
  output->ptr += size;
  *layout = result;
  return true;
}

bool gpsp_rv32_thumb_ir_append_entry(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_ir_environment *environment, unsigned entry,
    gpsp_rv32_code *output, gpsp_rv32_thumb_ir_layout *layout)
{
  if (!supported_region(ir, environment) || entry < 1 || entry > 3 ||
      !output || !layout || !output->start || !output->ptr || !output->end ||
      ((uintptr_t)output->start & 3) ||
      output->ptr < output->start || output->ptr > output->end ||
      layout->entry_offset[entry] != UINT16_MAX ||
      (size_t)(output->ptr - output->start) != layout->code_bytes ||
      layout->mov1_offset >= layout->code_bytes ||
      layout->mov2_offset >= layout->code_bytes ||
      layout->cold_offset + 8 > layout->code_bytes)
    return false;

  uint32_t words[24];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, output, words, sizeof(words));
  uint8_t *cold_site;
  if (!emit_side_entry(&staged, environment, entry,
          layout->code_bytes, layout->mov1_offset, layout->mov2_offset,
          layout->cold_offset, true, &cold_site))
    return false;

  size_t appended = (size_t)(staged.ptr - staged.start);
  if (layout->code_bytes > UINT16_MAX - appended ||
      (size_t)(output->end - output->ptr) < appended)
    return false;
  memcpy(output->ptr, staged.start, appended);
  layout->entry_offset[entry] = layout->code_bytes;
  layout->code_bytes += appended;
  output->ptr += appended;
  return true;
}

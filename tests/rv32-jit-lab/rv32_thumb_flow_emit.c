/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_thumb_flow_emit.h"

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

static bool valid_input(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment,
    const gpsp_rv32_code *output)
{
  if (!ir || !environment || !output || !output->start || !output->ptr ||
      !output->end || !ir->count ||
      ir->count > GPSP_RV32_THUMB_IR_MAX_OPS ||
      !environment->sequential_wait_address ||
      !environment->nonsequential_wait_address ||
      !environment->interpreter_stub_address ||
      (environment->interpreter_stub_address & 3) ||
      (ir->pc & 1) || ir->pc > UINT32_MAX - ir->count * 2 ||
      (ir->pc >> 24) < 8 || (ir->pc >> 24) > 13 ||
      (ir->pc >> 24) != ((ir->pc + ir->count * 2) >> 24) ||
      ((uintptr_t)output->ptr & 3) || output->ptr != output->start ||
      output->ptr > output->end)
    return false;

  for (unsigned i = 0; i < ir->count; i++)
  {
    if (environment->idle_loop_target_pc == ir->pc + (i + 1) * 2)
      return false;
    const gpsp_rv32_thumb_node *node = &ir->nodes[i];
    if (node->fallthrough != i + 1 || node->rd >= 8 || node->rs >= 8 ||
        (node->kind != GPSP_RV32_THUMB_IR_TST &&
         node->kind != GPSP_RV32_THUMB_IR_BEQ &&
         node->kind != GPSP_RV32_THUMB_IR_MOV_IMM) ||
        (node->kind == GPSP_RV32_THUMB_IR_BEQ &&
         (node->taken <= i || node->taken > ir->count)))
      return false;
  }
  return true;
}

static bool emit_guard(gpsp_rv32_code *code)
{
  uint8_t *skip = code->ptr;
  return gpsp_rv32_blt(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_jalr(code, A1, RA, 4) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_wait(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_flow_environment *environment, bool branch)
{
  uint32_t seq = environment->sequential_wait_address;
  uint32_t upper = address_upper(seq);
  if ((upper && !gpsp_rv32_lui(code, RV_TMP0, upper)) ||
      !gpsp_rv32_lbu(code, A0, upper ? RV_TMP0 : X0, address_low(seq)))
    return false;
  if (!branch)
    return true;

  uint32_t nseq = environment->nonsequential_wait_address;
  uint32_t other_upper = address_upper(nseq);
  return (upper == other_upper || !other_upper ||
      gpsp_rv32_lui(code, RV_TMP0, other_upper)) &&
      gpsp_rv32_lbu(code, A1, other_upper ? RV_TMP0 : X0,
          address_low(nseq));
}

static bool emit_scaled_wait(gpsp_rv32_code *code, unsigned count)
{
  if (count == 1)
    return gpsp_rv32_addi(code, RV_TMP0, A0, 0);

  unsigned first = 0;
  while (!(count & (1U << first)))
    first++;
  if (!(first ? gpsp_rv32_slli(code, RV_TMP0, A0, first) :
        gpsp_rv32_addi(code, RV_TMP0, A0, 0)))
    return false;
  for (unsigned bit = first + 1; bit < 6; bit++)
    if (count & (1U << bit))
    {
      if (!gpsp_rv32_slli(code, A2, A0, bit) ||
          !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, A2))
        return false;
    }
  return true;
}

static bool emit_tst(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_node *node, unsigned flags_live)
{
  if (!(flags_live & (GPSP_RV32_THUMB_FLAG_N |
          GPSP_RV32_THUMB_FLAG_Z)))
    return true;
  unsigned value = guest_reg_map[node->rd];
  if (node->rd != node->rs)
  {
    if (!gpsp_rv32_and(code, RV_TMP1, value, guest_reg_map[node->rs]))
      return false;
    value = RV_TMP1;
  }
  return (!(flags_live & GPSP_RV32_THUMB_FLAG_N) ||
      gpsp_rv32_srli(code, RV_FLAG_N, value, 31)) &&
      (!(flags_live & GPSP_RV32_THUMB_FLAG_Z) ||
      gpsp_rv32_sltiu(code, RV_FLAG_Z, value, 1));
}

static bool emit_mov(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_node *node, unsigned flags_live)
{
  return gpsp_rv32_addi(code, guest_reg_map[node->rd], X0,
          node->imm) &&
      (!(flags_live & GPSP_RV32_THUMB_FLAG_N) ||
       gpsp_rv32_addi(code, RV_FLAG_N, X0, 0)) &&
      (!(flags_live & GPSP_RV32_THUMB_FLAG_Z) ||
       gpsp_rv32_addi(code, RV_FLAG_Z, X0, node->imm == 0));
}

/* Charge this path before either successor, including a scan-limit exit. */
static bool emit_block(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment,
    unsigned start, unsigned end, uint8_t **cold_site)
{
  bool branch = ir->nodes[end - 1].kind == GPSP_RV32_THUMB_IR_BEQ;
  unsigned seq_count = end - start;
  unsigned last_flag_writer = end;
  for (unsigned i = start; i < end; i++)
    if (ir->nodes[i].flags_write &
        (GPSP_RV32_THUMB_FLAG_N | GPSP_RV32_THUMB_FLAG_Z))
      last_flag_writer = i;
  unsigned cost_reg = seq_count == 1 && !branch ? A0 : RV_TMP0;
  if (!emit_guard(code) || !emit_wait(code, environment, branch))
    return false;
  if (seq_count == 1)
  {
    if (branch && !gpsp_rv32_add(code, RV_TMP0, A0, A1))
      return false;
  }
  else if (!emit_scaled_wait(code, seq_count) ||
      (branch && !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, A1)))
    return false;
  *cold_site = NULL;
  /* ROM Thumb waits are at most 9 cycles per access. */
  if ((seq_count + (unsigned)branch) * 9 > 64)
  {
    uint8_t *skip = code->ptr;
    if (!gpsp_rv32_bge(code, RV_BUDGET, cost_reg, 0))
      return false;
    *cold_site = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_patch_branch(code, skip, code->ptr))
      return false;
  }

  for (unsigned i = start; i < end; i++)
  {
    const gpsp_rv32_thumb_node *node = &ir->nodes[i];
    unsigned flags_live = node->flags_live_out;
    /* The scheduler can observe flags before the next basic block. */
    if (i == last_flag_writer)
      flags_live |= GPSP_RV32_THUMB_FLAG_N |
          GPSP_RV32_THUMB_FLAG_Z;
    if (node->kind == GPSP_RV32_THUMB_IR_TST)
    {
      if (!emit_tst(code, node, flags_live))
        return false;
    }
    else if (node->kind == GPSP_RV32_THUMB_IR_MOV_IMM &&
        !emit_mov(code, node, flags_live))
      return false;
  }

  return gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, cost_reg) &&
      gpsp_rv32_addi(code, RV_PC, RV_PC, (int)(end - start) * 2);
}

static bool emit_cold_stub(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_flow_environment *environment,
    const uint16_t *sites, unsigned count, uint8_t *base)
{
  if (!count)
    return true;
  uint8_t *cold = code->ptr;
  if (gpsp_rv32_emit_interpreter_step_stub(code,
          environment->interpreter_stub_address) !=
      GPSP_RV32_FALLBACK_EMITTED)
    return false;
  for (unsigned i = 0; i < count; i++)
    if (!gpsp_rv32_patch_branch(code, base + sites[i], cold))
      return false;
  return true;
}

bool gpsp_rv32_thumb_flow_emit(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment,
    gpsp_rv32_code *output, gpsp_rv32_thumb_flow_layout *layout)
{
  if (!layout || !valid_input(ir, environment, output))
    return false;

  uint8_t boundaries[GPSP_RV32_THUMB_IR_MAX_OPS + 1] = {1};
  for (unsigned i = 0; i < ir->count; i++)
    if (ir->nodes[i].kind == GPSP_RV32_THUMB_IR_BEQ)
    {
      if (ir->nodes[i].taken <= i ||
          ir->nodes[i].taken > ir->count)
        return false;
      boundaries[i + 1] = 1;
      boundaries[ir->nodes[i].taken] = 1;
    }
  boundaries[ir->count] = 1;

  gpsp_rv32_thumb_flow_layout result;
  for (unsigned i = 0; i < GPSP_RV32_THUMB_IR_MAX_OPS; i++)
    result.entry_offset[i] = UINT16_MAX;
  result.block_count = 0;
  uint16_t cold_sites[GPSP_RV32_THUMB_IR_MAX_OPS];
  uint16_t branch_sites[GPSP_RV32_THUMB_IR_MAX_OPS];
  for (unsigned i = 0; i < ir->count; i++)
    branch_sites[i] = UINT16_MAX;
  unsigned cold_count = 0;
  uint8_t *begin = output->ptr;
  gpsp_rv32_code_mark undo = gpsp_rv32_code_save(output);

  for (unsigned start = 0; start < ir->count;)
  {
    unsigned end = start + 1;
    while (!boundaries[end])
      end++;
    if ((size_t)(output->ptr - begin) > UINT16_MAX)
      goto failed;
    result.entry_offset[start] = (uint16_t)(output->ptr - begin);
    result.block_count++;
    uint8_t *cold_site;
    if (!emit_block(output, ir, environment, start, end,
            &cold_site))
      goto failed;
    if (cold_site)
    {
      if ((size_t)(cold_site - begin) > UINT16_MAX)
        goto failed;
      cold_sites[cold_count++] = (uint16_t)(cold_site - begin);
    }
    const gpsp_rv32_thumb_node *last = &ir->nodes[end - 1];
    if (last->kind == GPSP_RV32_THUMB_IR_BEQ && last->taken != end)
    {
      uint8_t *skip = output->ptr;
      if (!gpsp_rv32_beq(output, RV_FLAG_Z, X0, 0) ||
          !gpsp_rv32_addi(output, RV_PC, RV_PC,
              (int)(last->taken - end) * 2))
        goto failed;
      if ((size_t)(output->ptr - begin) > UINT16_MAX)
        goto failed;
      branch_sites[end - 1] = (uint16_t)(output->ptr - begin);
      if (!gpsp_rv32_jal(output, X0, 0) ||
          !gpsp_rv32_patch_branch(output, skip, output->ptr))
        goto failed;
    }
    start = end;
  }

  if ((size_t)(output->ptr - begin) > UINT16_MAX)
    goto failed;
  result.exit_link_offset = (uint16_t)(output->ptr - begin);
  uint8_t *exit_link;
  if (!gpsp_rv32_emit_link_slot(output, &exit_link) ||
      !emit_cold_stub(output, environment, cold_sites, cold_count, begin))
    goto failed;
  for (unsigned i = 0; i < ir->count; i++)
  {
    if (branch_sites[i] == UINT16_MAX)
      continue;
    unsigned target = ir->nodes[i].taken;
    uint8_t *address = target == ir->count ? exit_link :
        (result.entry_offset[target] == UINT16_MAX ? NULL :
        begin + result.entry_offset[target]);
    if (!address || !gpsp_rv32_patch_branch(output,
            begin + branch_sites[i], address))
      goto failed;
  }
  if ((size_t)(output->ptr - begin) > UINT16_MAX)
    goto failed;
  result.code_bytes = (uint16_t)(output->ptr - begin);
  *layout = result;
  return true;

failed:
  gpsp_rv32_code_restore(output, &undo);
  return false;
}

bool gpsp_rv32_thumb_flow_emit_side(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment, unsigned entry,
    unsigned join, uint64_t published_mask,
    const gpsp_rv32_thumb_flow_layout *main_layout,
    gpsp_rv32_code *output, gpsp_rv32_thumb_flow_side_layout *layout)
{
  if (!layout || !main_layout ||
      !valid_input(ir, environment, output) || entry >= ir->count ||
      main_layout->entry_offset[entry] != UINT16_MAX)
    return false;

  unsigned end = entry + 1;
  while (end < ir->count &&
      main_layout->entry_offset[end] == UINT16_MAX)
    end++;
  if (join <= entry || join > end ||
      !(published_mask & (UINT64_C(1) << join)) ||
      (published_mask & (UINT64_C(1) << entry)))
    return false;
  end = join;
  uint8_t *begin = output->ptr;
  gpsp_rv32_code_mark undo = gpsp_rv32_code_save(output);
  gpsp_rv32_thumb_flow_side_layout result = {0};
  uint8_t *cold_site;
  if (!emit_block(output, ir, environment, entry, end,
          &cold_site))
    goto failed;

  const gpsp_rv32_thumb_node *last = &ir->nodes[end - 1];
  if (last->kind == GPSP_RV32_THUMB_IR_BEQ && last->taken != end)
  {
    uint8_t *skip = output->ptr;
    if (!gpsp_rv32_beq(output, RV_FLAG_Z, X0, 0) ||
        !gpsp_rv32_addi(output, RV_PC, RV_PC,
            (int)(last->taken - end) * 2))
      goto failed;
    result.link_target[result.link_count] = last->taken;
    result.link_offset[result.link_count++] = (uint16_t)(output->ptr - begin);
    uint8_t *site;
    if (!gpsp_rv32_emit_link_slot(output, &site) ||
        !gpsp_rv32_patch_branch(output, skip, output->ptr))
      goto failed;
  }
  result.link_target[result.link_count] = end;
  result.link_offset[result.link_count++] = (uint16_t)(output->ptr - begin);
  uint8_t *site;
  uint16_t cold_offset = cold_site ? (uint16_t)(cold_site - begin) : 0;
  if (!gpsp_rv32_emit_link_slot(output, &site) ||
      !emit_cold_stub(output, environment, &cold_offset,
          cold_site ? 1 : 0, begin) ||
      (size_t)(output->ptr - begin) > UINT16_MAX)
    goto failed;
  result.code_bytes = (uint16_t)(output->ptr - begin);
  *layout = result;
  return true;

failed:
  gpsp_rv32_code_restore(output, &undo);
  return false;
}

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_thumb_flow_full.h"

#include "rv32_arm_system.h"
#include "rv32_block_emit.h"
#include "rv32_block_link.h"
#include "rv32_fallback.h"
#include "rv32_flow_timing.h"
#include "rv32_interpreter_stub.h"
#include "rv32_register_map.h"
#include "rv32_thumb_alu.h"

#include <limits.h>
#include <string.h>

extern uint32_t idle_loop_target_pc;
extern uint8_t ws_cyc_nseq[16][2];

#define FLOW_ALU_PREFLIGHT_LIMIT 48

static uint32_t branch_target(uint32_t pc, uint16_t opcode, bool conditional)
{
  int32_t offset = opcode & (conditional ? 0xff : 0x7ff);
  if (offset & (conditional ? 0x80 : 0x400))
    offset -= conditional ? 0x100 : 0x800;
  return pc + 4 + (uint32_t)(offset * 2);
}

static uint32_t bl_pair_target(uint32_t pc, uint16_t first,
    uint16_t second)
{
  int32_t high = first & 0x7ff;
  if (high & 0x400)
    high -= 0x800;
  return pc + 4 + (uint32_t)(high * 4096) +
      ((second & 0x7ff) << 1);
}

static bool emit_bl_return(gpsp_rv32_code *code)
{
  /* RV_PC is halfword-aligned; LR after the BL pair has bit 0 set. */
  return gpsp_rv32_addi(code, RV_GUEST_LR, RV_PC, 5);
}

static uint8_t classify(uint16_t opcode)
{
  if ((opcode & 0xf800) == 0x4800 ||
      (opcode >= 0x5000 && opcode < 0xa000))
    return GPSP_RV32_THUMB_FLOW_MEMORY;
  if ((opcode & 0xfe00) == 0xb400 ||
      (opcode & 0xfe00) == 0xbc00 ||
      (opcode & 0xf000) == 0xc000)
    return gpsp_rv32_thumb_multi_native(opcode) ?
        GPSP_RV32_THUMB_FLOW_MULTI : GPSP_RV32_THUMB_FLOW_HELPER;
  if ((opcode & 0xf000) == 0xd000)
  {
    unsigned condition = (opcode >> 8) & 15;
    return condition < 14 ? GPSP_RV32_THUMB_FLOW_BCC :
        condition == 15 ? GPSP_RV32_THUMB_FLOW_SYSTEM :
        GPSP_RV32_THUMB_FLOW_HELPER;
  }
  if ((opcode & 0xf800) == 0xe000)
    return GPSP_RV32_THUMB_FLOW_B;
  if ((opcode & 0xf800) == 0xf800)
    return GPSP_RV32_THUMB_FLOW_DYNAMIC;
  if ((opcode & 0xfc00) == 0x4400)
  {
    unsigned operation = (opcode >> 8) & 3;
    unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
    if (operation == 3 || (rd == 15 && operation != 1))
      return gpsp_rv32_thumb_needs_helper(opcode) ?
          GPSP_RV32_THUMB_FLOW_HELPER : GPSP_RV32_THUMB_FLOW_DYNAMIC;
  }
  if (gpsp_rv32_thumb_needs_helper(opcode))
    return GPSP_RV32_THUMB_FLOW_HELPER;
  return gpsp_rv32_thumb_batch_alu(opcode) ?
      GPSP_RV32_THUMB_FLOW_ALU : GPSP_RV32_THUMB_FLOW_SINGLE;
}

bool gpsp_rv32_thumb_flow_full_decode(uint32_t pc,
    const uint16_t *opcodes, size_t count,
    gpsp_rv32_thumb_flow_full_ir *out)
{
  if (!opcodes || !out || !count ||
      count > GPSP_RV32_THUMB_FLOW_MAX_OPS || (pc & 1) ||
      pc > UINT32_MAX - count * 2)
    return false;

  out->pc = pc;
  out->interpreter_stub_address = 0;
  out->count = (uint8_t)count;
  out->rom_region = false;
  out->fallthrough_live = 15;
  for (size_t i = 0; i < count; i++)
  {
    gpsp_rv32_thumb_flow_op *op = &out->ops[i];
    op->opcode = opcodes[i];
    op->literal_value = 0;
    op->literal_constant = 0;
    op->entry_required = i == 0;
    op->flag_data = 0x000f;
    op->target_live = 15;
    op->kind = classify(opcodes[i]);
  }
  for (size_t i = 0; i < count; i++)
  {
    uint8_t kind = out->ops[i].kind;
    if (kind != GPSP_RV32_THUMB_FLOW_BCC &&
        kind != GPSP_RV32_THUMB_FLOW_B)
      continue;
    uint32_t target = branch_target(pc + i * 2, opcodes[i],
        kind == GPSP_RV32_THUMB_FLOW_BCC);
    if (target >= pc && target < pc + count * 2 && !(target & 1))
      out->ops[(target - pc) / 2].entry_required = 1;
  }
  return true;
}

uint32_t gpsp_rv32_thumb_flow_branch_target(
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned index)
{
  if (!ir || index >= ir->count)
    return UINT32_MAX;
  uint8_t kind = ir->ops[index].kind;
  if (kind != GPSP_RV32_THUMB_FLOW_B && kind != GPSP_RV32_THUMB_FLOW_BCC)
    return UINT32_MAX;
  return branch_target(ir->pc + index * 2, ir->ops[index].opcode,
      kind == GPSP_RV32_THUMB_FLOW_BCC);
}

static uint8_t condition_flags(unsigned condition);

/* NZCV = 8/4/2/1. Report reads and guaranteed writes; false at control
   flow. Writes that depend on a shift count of zero are left out. */
static bool thumb_flag_step(uint16_t opcode, uint8_t *read, uint8_t *write)
{
  *read = 0;
  *write = 0;
  switch (opcode >> 13)
  {
    case 0:
      if ((opcode & 0x1800) == 0x1800)
        *write = 15;
      else
        *write = ((opcode >> 11) & 3) == 0 && !((opcode >> 6) & 31) ?
            12 : 14;
      return true;
    case 1:
      *write = ((opcode >> 11) & 3) == 0 ? 12 : 15;
      return true;
    case 2:
      if ((opcode & 0xfc00) == 0x4000)
      {
        unsigned operation = (opcode >> 6) & 15;
        if (operation == 5 || operation == 6)
          *read = 2;
        *write = operation == 5 || operation == 6 ||
            (operation >= 9 && operation <= 11) ? 15 : 12;
        return true;
      }
      if ((opcode & 0xfc00) == 0x4400)
      {
        unsigned operation = (opcode >> 8) & 3;
        unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
        if (operation == 3 || (rd == 15 && operation != 1))
          return false;
        *write = operation == 1 ? 15 : 0;
      }
      return true;
    case 3:
    case 4:
      return true;
    case 5:
      if ((opcode & 0xf000) == 0xa000 || (opcode & 0xff00) == 0xb000)
        return true;
      return (opcode & 0xf600) == 0xb400 && (opcode & 0xff00) != 0xbd00;
    case 6:
      if ((opcode & 0xf000) == 0xc000)
        return true;
      if (((opcode >> 8) & 15) < 14)
        *read = condition_flags((opcode >> 8) & 15);
      return false;
    default:
      return false;
  }
}

uint8_t gpsp_rv32_thumb_flow_live_flags(const uint16_t *opcodes,
    size_t count)
{
  uint8_t live = 0;
  uint8_t pending = 15;
  for (size_t i = 0; opcodes && i < count && pending; i++)
  {
    uint8_t read, write;
    bool more = thumb_flag_step(opcodes[i], &read, &write);
    live |= read & pending;
    if (!more)
      break;
    pending &= ~write;
  }
  return live | pending;
}

static bool record_offset(uint8_t *begin, uint8_t *at, uint16_t *offset)
{
  if (at < begin || (size_t)(at - begin) > UINT16_MAX)
    return false;
  *offset = (uint16_t)(at - begin);
  return true;
}

/* Entries and side bodies are published as offsets in four-byte units. */
static bool record_entry(gpsp_rv32_code *code, uint8_t *begin,
    uint16_t *offset)
{
  return gpsp_rv32_align4(code) && record_offset(begin, code->ptr, offset);
}

static uint8_t condition_flags(unsigned condition)
{
  static const uint8_t flags[14] =
  {
    4, 4, 2, 2, 8, 8, 1, 1, 6, 6, 9, 9, 13, 13,
  };
  return condition < 14 ? flags[condition] : 15;
}

static uint8_t successor_flags(const gpsp_rv32_thumb_flow_full_ir *ir,
    const uint8_t *live_in, uint32_t target, uint8_t outside)
{
  if (target < ir->pc || target >= ir->pc + ir->count * 2 ||
      (target & 1))
    return outside;
  return live_in[(target - ir->pc) / 2];
}

static void analyze_flags(const gpsp_rv32_thumb_flow_full_ir *ir,
    uint8_t *needed)
{
  uint8_t live_in[GPSP_RV32_THUMB_FLOW_MAX_OPS] = {0};
  uint8_t live_out[GPSP_RV32_THUMB_FLOW_MAX_OPS] = {0};
  bool changed;
  do
  {
    changed = false;
    for (unsigned i = ir->count; i-- > 0;)
    {
      const gpsp_rv32_thumb_flow_op *op = &ir->ops[i];
      uint32_t pc = ir->pc + i * 2;
      uint8_t kind = op->kind;
      uint8_t out = i + 1 == ir->count ? ir->fallthrough_live :
          live_in[i + 1];
      if (kind == GPSP_RV32_THUMB_FLOW_B ||
          kind == GPSP_RV32_THUMB_FLOW_BCC)
      {
        uint8_t target = successor_flags(ir, live_in,
            branch_target(pc, op->opcode,
                kind == GPSP_RV32_THUMB_FLOW_BCC), op->target_live);
        out = kind == GPSP_RV32_THUMB_FLOW_B ? target : out | target;
      }
      /* Data transfers neither read nor write NZCV. A slow exit may
         store stale dead flags, which later guest code overwrites. */
      bool transfer = kind == GPSP_RV32_THUMB_FLOW_MEMORY ||
          (kind == GPSP_RV32_THUMB_FLOW_MULTI &&
           (op->opcode & 0xff00) != 0xbd00);
      if (kind == GPSP_RV32_THUMB_FLOW_MULTI && !transfer)
        out = 15;
      if (kind == GPSP_RV32_THUMB_FLOW_DYNAMIC ||
          kind == GPSP_RV32_THUMB_FLOW_SYSTEM ||
          kind == GPSP_RV32_THUMB_FLOW_HELPER)
        out = 15;

      uint16_t status = op->flag_data;
      uint8_t must_write = (status >> 4) & 15;
      uint8_t read = (status >> 8) & 15;
      if (kind == GPSP_RV32_THUMB_FLOW_BCC)
      {
        must_write = 0;
        read = condition_flags((op->opcode >> 8) & 15);
      }
      else if (op->literal_constant || transfer)
      {
        must_write = 0;
        read = 0;
      }
      else if (kind != GPSP_RV32_THUMB_FLOW_ALU &&
          kind != GPSP_RV32_THUMB_FLOW_SINGLE)
      {
        must_write = 0;
        read = 15;
      }
      uint8_t in = read | (out & ~must_write);
      changed |= live_out[i] != out || live_in[i] != in;
      live_out[i] = out;
      live_in[i] = in;
    }
  } while (changed);

  for (unsigned i = 0; i < ir->count; i++)
    needed[i] = (ir->ops[i].flag_data & 15) & live_out[i];
}

static bool register_add(uint16_t opcode)
{
  return (opcode & 0xfe00) == 0x1800;
}

static bool lazy_gap_alu(const gpsp_rv32_thumb_flow_op *op,
    uint8_t needed)
{
  uint16_t opcode = op->opcode;
  bool shift = (opcode & 0xe000) == 0 &&
      (opcode & 0x1800) != 0x1800;
  bool imm_add_sub = (opcode & 0xe000) == 0x2000 &&
      ((opcode >> 11) & 3) >= 2;
  return op->kind == GPSP_RV32_THUMB_FLOW_ALU &&
      !op->literal_constant && !needed &&
      !(op->flag_data >> 8) && (shift || imm_add_sub);
}

static void analyze_lazy_add(const gpsp_rv32_thumb_flow_full_ir *ir,
    const uint8_t *needed, uint8_t *lazy)
{
  memset(lazy, 0, GPSP_RV32_THUMB_FLOW_MAX_OPS);
  for (unsigned i = 0; i < ir->count; i++)
  {
    if (ir->ops[i].kind != GPSP_RV32_THUMB_FLOW_ALU ||
        !register_add(ir->ops[i].opcode) || needed[i] != 15)
      continue;
    unsigned j = i + 1;
    while (j < ir->count &&
        ir->ops[j].kind == GPSP_RV32_THUMB_FLOW_MEMORY &&
        !ir->ops[j].literal_constant)
      j++;
    if (j == i + 1 || j == ir->count ||
        ir->ops[j].kind != GPSP_RV32_THUMB_FLOW_ALU)
      continue;
    unsigned head = j;
    while (j < ir->count)
    {
      const gpsp_rv32_thumb_flow_op *op = &ir->ops[j];
      uint32_t pc = ir->pc + j * 2;
      uint32_t head_pc = ir->pc + head * 2;
      if (op->kind != GPSP_RV32_THUMB_FLOW_ALU ||
          op->literal_constant ||
          (j > head && op->entry_required) ||
          (pc >> 15) != (head_pc >> 15) ||
          ((pc + 2) >> 24) != ((head_pc + 2) >> 24) ||
          pc + 2 == idle_loop_target_pc)
        break;
      if (register_add(op->opcode) && needed[j] == 15)
      {
        lazy[i] = 1;
        break;
      }
      if (!lazy_gap_alu(op, needed[j]))
        break;
      j++;
    }
  }
}

static bool emit_lazy_add(gpsp_rv32_code *code, uint16_t opcode)
{
  unsigned lhs = guest_reg_map[(opcode >> 3) & 7];
  unsigned rhs = guest_reg_map[(opcode >> 6) & 7];
  unsigned dest = guest_reg_map[opcode & 7];
  return gpsp_rv32_addi(code, RV_FLAG_N, lhs, 0) &&
      gpsp_rv32_addi(code, RV_FLAG_Z, rhs, 0) &&
      gpsp_rv32_add(code, dest, RV_FLAG_N, RV_FLAG_Z) &&
      gpsp_rv32_addi(code, RV_FLAG_V, X0, -1);
}

static bool emit_flow_alu(gpsp_rv32_code *code, uint16_t opcode,
    uint32_t pc, uint8_t needed, bool lazy)
{
  if (lazy)
    return emit_lazy_add(code, opcode);
  return gpsp_rv32_emit_thumb(code, opcode, pc, needed) ==
      GPSP_RV32_THUMB_EMITTED;
}

static bool emit_step(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_flow_full_ir *ir)
{
  uintptr_t target = ir->interpreter_stub_address ?
      ir->interpreter_stub_address :
      (uintptr_t)gpsp_rv32_interpreter_step_stub;
  return gpsp_rv32_emit_interpreter_step_stub(code,
      target) ==
      GPSP_RV32_FALLBACK_EMITTED;
}

static bool emit_alu_region(gpsp_rv32_code *code, uint8_t *begin,
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned start,
    unsigned end, const uint8_t *needed, const uint8_t *lazy,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  unsigned count = end - start;
  uint32_t pc = ir->pc + start * 2;

  if (!record_entry(code, begin, &layout->entry_offset[start]))
    return false;
  uint8_t *cold_site = NULL;
  bool preflight = count *
      gpsp_rv32_flow_fetch_bound(pc + 2, true) >
      FLOW_ALU_PREFLIGHT_LIMIT;
  if (preflight)
  {
    if (!gpsp_rv32_flow_preflight_seq(code, pc + 2,
            count, true, &cold_site))
      return false;
  }
  else if (!gpsp_rv32_flow_guard(code) ||
      !gpsp_rv32_flow_charge_seq(code, pc + 2, count, true))
    return false;

  for (unsigned i = start; i < end; i++)
  {
    if (!record_entry(code, begin, &layout->body_offset[i]) ||
        !emit_flow_alu(code, ir->ops[i].opcode,
            ir->pc + i * 2, needed[i], lazy[i]))
      return false;
    if (ir->rom_region && i > start)
    {
      layout->side_prefix[i] = i - start;
      layout->side_remaining[i] = end - i;
    }
  }
  if (!gpsp_rv32_flow_advance_pc(code, pc, count, true))
    return false;
  if (!cold_site)
    return true;

  uint8_t *skip = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0))
    return false;
  uint8_t *cold = code->ptr;
  if (!emit_step(code, ir) ||
      !gpsp_rv32_patch_branch(code, cold_site, cold) ||
      !gpsp_rv32_patch_branch(code, skip, code->ptr))
    return false;
  return true;
}

static unsigned pure_worst_cost(uint32_t pc, bool literal)
{
  /* ROM wait maxima are 3/5/9 seq and 10 + seq for a word nseq. */
  unsigned region = pc >> 24;
  unsigned seq = region < 10 ? 3 : region < 12 ? 5 : 9;
  return literal ? seq * 2 + 10 : seq;
}

static unsigned pure_captured_cost(uint32_t pc, bool literal)
{
  unsigned cost = gpsp_rv32_flow_fetch_bound(pc + 2, true);
  return cost + (literal ? ws_cyc_nseq[pc >> 24][1] : 0);
}

static bool emit_mixed_region(gpsp_rv32_code *code, uint8_t *begin,
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned start,
    unsigned end, const uint8_t *needed, const uint8_t *lazy,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  uint32_t pc = ir->pc + start * 2;
  unsigned total = 0;
  for (unsigned i = start; i < end; i++)
    total += pure_captured_cost(ir->pc + i * 2,
        ir->ops[i].literal_constant);
  if (total > 48 ||
      !record_entry(code, begin, &layout->entry_offset[start]) ||
      !gpsp_rv32_flow_guard(code))
    return false;

  unsigned prefix = 0;
  for (unsigned i = start; i < end; i++)
  {
    const gpsp_rv32_thumb_flow_op *op = &ir->ops[i];
    if (!record_entry(code, begin, &layout->body_offset[i]) ||
        !(op->literal_constant ?
          gpsp_rv32_flow_literal_body(code, op->opcode,
              op->literal_value) :
          emit_flow_alu(code, op->opcode,
              ir->pc + i * 2, needed[i], lazy[i])))
      return false;
    if (i > start)
    {
      layout->side_prefix[i] = i - start;
      layout->side_remaining[i] = end - i;
      layout->side_prefix_cost[i] = prefix;
      layout->side_total_cost[i] = total;
    }
    prefix += pure_captured_cost(ir->pc + i * 2,
        op->literal_constant);
  }
  return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)total) &&
      gpsp_rv32_flow_advance_pc(code, pc, end - start, true);
}

/* A region may hold RAM stub sites among pure work. PC stays lazy: it
   is synced to a site's PC right before the call, and the stub leaves it
   past the site. Fetches of earlier ops are charged before each site, so a
   helper exit leaves only work that ran; the rest is charged at the end.
   The stubs check the budget, so a region headed by a site has no guard. */
static bool is_stub_site(const gpsp_rv32_thumb_flow_op *op)
{
  return op->kind == GPSP_RV32_THUMB_FLOW_MEMORY && !op->literal_constant;
}

static bool emit_memory_region(gpsp_rv32_code *code, uint8_t *begin,
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned start,
    unsigned end, const uint8_t *needed, const uint8_t *lazy,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  uint32_t pc = ir->pc + start * 2;
  unsigned total = 0;
  for (unsigned i = start; i < end; i++)
    total += pure_captured_cost(ir->pc + i * 2,
        ir->ops[i].literal_constant);
  if (total > 48 ||
      !record_entry(code, begin, &layout->entry_offset[start]) ||
      (!is_stub_site(&ir->ops[start]) && !gpsp_rv32_flow_guard(code)))
    return false;

  uint32_t synced = pc;
  unsigned prefix = 0;
  unsigned charged = 0;
  for (unsigned i = start; i < end; i++)
  {
    const gpsp_rv32_thumb_flow_op *op = &ir->ops[i];
    uint32_t op_pc = ir->pc + i * 2;
    if (!record_entry(code, begin, &layout->body_offset[i]))
      return false;
    if (is_stub_site(op))
    {
      if ((prefix != charged &&
           !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET,
               -(int)(prefix - charged))) ||
          (op_pc != synced &&
           !gpsp_rv32_addi(code, RV_PC, RV_PC, (int)(op_pc - synced))) ||
          !gpsp_rv32_emit_thumb_memory_head(code, op_pc, op->opcode))
        return false;
      synced = op_pc + 2;
      charged = prefix;
    }
    else if (!(op->literal_constant ?
          gpsp_rv32_flow_literal_body(code, op->opcode,
              op->literal_value) :
          emit_flow_alu(code, op->opcode, op_pc, needed[i], lazy[i])))
      return false;
    if (i > start)
    {
      layout->side_prefix[i] = i - start;
      layout->side_remaining[i] = end - i;
      layout->side_prefix_cost[i] = prefix;
      layout->side_total_cost[i] = total | GPSP_RV32_THUMB_FLOW_MEMORY_HEAD;
    }
    prefix += pure_captured_cost(op_pc, op->literal_constant);
  }
  uint32_t end_pc = ir->pc + end * 2;
  return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)(total - charged)) &&
      (end_pc == synced ||
       gpsp_rv32_flow_advance_pc(code, synced, (end_pc - synced) / 2, true));
}

static bool emit_bcc_skip(gpsp_rv32_code *code, unsigned condition,
    uint8_t **skip)
{
  *skip = code->ptr;
  switch (condition)
  {
    case 0:  return gpsp_rv32_beq(code, RV_FLAG_Z, X0, 0);
    case 1:  return gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0);
    case 2:  return gpsp_rv32_beq(code, RV_FLAG_C, X0, 0);
    case 3:  return gpsp_rv32_bne(code, RV_FLAG_C, X0, 0);
    case 4:  return gpsp_rv32_beq(code, RV_FLAG_N, X0, 0);
    case 5:  return gpsp_rv32_bne(code, RV_FLAG_N, X0, 0);
    case 6:  return gpsp_rv32_beq(code, RV_FLAG_V, X0, 0);
    case 7:  return gpsp_rv32_bne(code, RV_FLAG_V, X0, 0);
    /* C and Z are bits: HI iff C > Z; LS iff C <= Z. */
    case 8:  return gpsp_rv32_bge(code, RV_FLAG_Z, RV_FLAG_C, 0);
    case 9:  return gpsp_rv32_blt(code, RV_FLAG_Z, RV_FLAG_C, 0);
    case 10: return gpsp_rv32_bne(code, RV_FLAG_N, RV_FLAG_V, 0);
    case 11: return gpsp_rv32_beq(code, RV_FLAG_N, RV_FLAG_V, 0);
    case 12:
    case 13:
      if (!gpsp_rv32_xor(code, RV_TMP0, RV_FLAG_N, RV_FLAG_V) ||
          !gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_FLAG_Z))
        return false;
      *skip = code->ptr;
      return condition == 12 ? gpsp_rv32_bne(code, RV_TMP0, X0, 0) :
          gpsp_rv32_beq(code, RV_TMP0, X0, 0);
    default: return false;
  }
}

static bool emit_bl_pair(gpsp_rv32_code *code, uint8_t *begin,
    uint32_t pc, uint32_t target, unsigned first,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  gpsp_rv32_code_mark saved = gpsp_rv32_code_save(code);
  uint16_t entry_offset;
  uint16_t body_offset;
  uint16_t link_offset;
  if (layout->link_count == GPSP_RV32_THUMB_FLOW_MAX_OPS ||
      !record_entry(code, begin, &entry_offset) ||
      !gpsp_rv32_flow_guard(code) ||
      !record_entry(code, begin, &body_offset) ||
      !emit_bl_return(code) ||
      !gpsp_rv32_flow_bl_pair_advance(code, pc, target))
    goto failed;

  uint8_t *site = NULL;
  if (!gpsp_rv32_emit_link_slot(code, &site) || !site ||
      !record_offset(begin, site, &link_offset))
    goto failed;
  layout->entry_offset[first] = entry_offset;
  layout->body_offset[first] = body_offset;
  layout->link_offset[layout->link_count] = link_offset;
  layout->link_target[layout->link_count++] = target;
  return true;

failed:
  gpsp_rv32_code_restore(code, &saved);
  return false;
}

static bool emit_branch_region(gpsp_rv32_code *code, uint8_t *begin,
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned start,
    unsigned branch, const uint8_t *needed, const uint8_t *lazy,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  uint32_t head_pc = ir->pc + start * 2;
  uint32_t branch_pc = ir->pc + branch * 2;
  const gpsp_rv32_thumb_flow_op *last = &ir->ops[branch];
  bool conditional = last->kind == GPSP_RV32_THUMB_FLOW_BCC;
  uint32_t target = branch_target(branch_pc, last->opcode, conditional);
  unsigned prefix = 0;
  for (unsigned i = start; i < branch; i++)
    prefix += pure_captured_cost(ir->pc + i * 2,
        ir->ops[i].literal_constant);
  unsigned total = prefix + ws_cyc_nseq[target >> 24][0] +
      gpsp_rv32_flow_fetch_bound(target, true);
  if (total > 48 ||
      !record_entry(code, begin, &layout->entry_offset[start]) ||
      !gpsp_rv32_flow_guard(code))
    return false;

  unsigned prior = 0;
  for (unsigned i = start; i < branch; i++)
  {
    const gpsp_rv32_thumb_flow_op *op = &ir->ops[i];
    if (!record_entry(code, begin, &layout->body_offset[i]) ||
        !(op->literal_constant ?
          gpsp_rv32_flow_literal_body(code, op->opcode,
              op->literal_value) :
          emit_flow_alu(code, op->opcode,
              ir->pc + i * 2, needed[i], lazy[i])))
      return false;
    if (i > start)
    {
      layout->side_prefix[i] = i - start;
      layout->side_remaining[i] = branch + 1 - i;
      layout->side_prefix_cost[i] = prior;
      layout->side_total_cost[i] = total;
    }
    prior += pure_captured_cost(ir->pc + i * 2,
        op->literal_constant);
  }

  if (!record_entry(code, begin, &layout->body_offset[branch]))
    return false;
  layout->side_prefix[branch] = branch - start;
  layout->side_remaining[branch] = 1;
  layout->side_prefix_cost[branch] = prefix;
  layout->side_total_cost[branch] = total;

  /* Side adapters credit skipped waits and rewind RV_PC to the run head. */
  if (!gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)total) ||
      !gpsp_rv32_flow_set_branch_pc(code, head_pc, target))
    return false;
  uint8_t *skip = NULL;
  if (conditional &&
      !emit_bcc_skip(code, (last->opcode >> 8) & 15, &skip))
    return false;
  uint8_t *site = NULL;
  if (!gpsp_rv32_emit_link_slot(code, &site) || !site ||
      layout->link_count == GPSP_RV32_THUMB_FLOW_MAX_OPS ||
      !record_offset(begin, site,
          &layout->link_offset[layout->link_count]))
    return false;
  layout->link_target[layout->link_count++] = target;
  return !conditional ||
      (gpsp_rv32_patch_branch(code, skip, code->ptr) &&
       gpsp_rv32_addi(code, RV_PC, RV_PC,
           (int32_t)(branch_pc + 2 - target)));
}

static bool emit_bcc(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode, bool guard_entry, uint8_t **link_site,
    uint32_t *link_target)
{
  uint32_t target = branch_target(pc, opcode, true);
  unsigned condition = (opcode >> 8) & 15;
  uint8_t *skip;
  bool shared_wait = (target >> 24) == ((pc + 2) >> 24) &&
      target != idle_loop_target_pc &&
      pc + 2 != idle_loop_target_pc;
  if (shared_wait)
  {
    if ((guard_entry && !gpsp_rv32_flow_guard(code)) ||
        !gpsp_rv32_flow_branch_advance(code, pc, target, true) ||
        !emit_bcc_skip(code, condition, &skip))
      return false;
    if (!gpsp_rv32_emit_link_slot(code, link_site) ||
        !gpsp_rv32_patch_branch(code, skip, code->ptr) ||
        !gpsp_rv32_addi(code, RV_PC, RV_PC,
            (int32_t)(pc + 2 - target)))
      return false;
    *link_target = target;
    return true;
  }
  if ((guard_entry && !gpsp_rv32_flow_guard(code)) ||
      !emit_bcc_skip(code, condition, &skip))
    return false;
  if (!gpsp_rv32_flow_branch_advance(code, pc, target, true) ||
      !gpsp_rv32_emit_link_slot(code, link_site) ||
      !gpsp_rv32_patch_branch(code, skip, code->ptr) ||
      !gpsp_rv32_flow_branch_advance(code, pc, pc + 2, true))
    return false;
  *link_target = target;
  return true;
}

static bool emit_b(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode, bool guard_entry, uint8_t **link_site,
    uint32_t *link_target)
{
  uint32_t target = branch_target(pc, opcode, false);
  if ((guard_entry && !gpsp_rv32_flow_guard(code)) ||
      !gpsp_rv32_flow_branch_advance(code, pc, target, true) ||
      !gpsp_rv32_emit_link_slot(code, link_site))
    return false;
  *link_target = target;
  return true;
}

static bool emit_single(gpsp_rv32_code *code,
    const gpsp_rv32_thumb_flow_full_ir *ir,
    const gpsp_rv32_thumb_flow_op *op, uint32_t pc,
    uint8_t needed_flags, bool lazy, bool guard_entry, bool memory_resume,
    bool *ends_block, bool *uses_helper,
    uint8_t **link_site, uint32_t *link_target)
{
  uint16_t opcode = op->opcode;
  gpsp_rv32_code_mark entry = gpsp_rv32_code_save(code);
  *ends_block = false;
  *uses_helper = false;
  *link_site = NULL;
  if (op->literal_constant)
  {
    if ((opcode & 0xf800) != 0x4800)
      return false;
    return gpsp_rv32_flow_literal_constant(code, pc,
        opcode, op->literal_value, guard_entry);
  }
  if (op->kind == GPSP_RV32_THUMB_FLOW_BCC)
    return emit_bcc(code, pc, opcode, guard_entry,
        link_site, link_target);
  if (op->kind == GPSP_RV32_THUMB_FLOW_B)
  {
    *ends_block = true;
    return emit_b(code, pc, opcode, guard_entry,
        link_site, link_target);
  }
  if (guard_entry && !gpsp_rv32_flow_guard(code))
    return false;
  if (op->kind == GPSP_RV32_THUMB_FLOW_MEMORY)
  {
    bool advanced = false;
    if (!gpsp_rv32_emit_thumb_memory_body(code, pc,
            opcode, memory_resume, &advanced))
      return false;
    return advanced || gpsp_rv32_flow_advance(code, pc, true);
  }
  if (op->kind == GPSP_RV32_THUMB_FLOW_MULTI)
    return gpsp_rv32_emit_thumb_multi_body(code, pc,
        opcode, ends_block);
  if (op->kind == GPSP_RV32_THUMB_FLOW_SYSTEM)
  {
    gpsp_rv32_arm_system_result result =
        gpsp_rv32_emit_thumb_swi(code, opcode, pc);
    if (result == GPSP_RV32_ARM_SYSTEM_EMITTED)
      return gpsp_rv32_flow_advance(code, pc, true);
    if (result == GPSP_RV32_ARM_SYSTEM_EXIT)
    {
      *ends_block = true;
      return true;
    }
    if (result == GPSP_RV32_ARM_SYSTEM_BUFFER_FULL)
      return false;
  }
  else if (op->kind != GPSP_RV32_THUMB_FLOW_HELPER)
  {
    gpsp_rv32_thumb_result result = lazy ?
        (emit_flow_alu(code, opcode, pc, needed_flags, true) ?
            GPSP_RV32_THUMB_EMITTED : GPSP_RV32_THUMB_BUFFER_FULL) :
        gpsp_rv32_emit_thumb(code, opcode, pc, needed_flags);
    if (result == GPSP_RV32_THUMB_EMITTED)
      return gpsp_rv32_flow_advance(code, pc, true);
    if (result == GPSP_RV32_THUMB_EXIT)
    {
      *ends_block = true;
      return gpsp_rv32_flow_dynamic_thumb_exit(code,
          opcode, false);
    }
    if (result == GPSP_RV32_THUMB_BUFFER_FULL)
      return false;
  }

  /* The helper executes one instruction and returns to the block runner. */
  gpsp_rv32_code_restore(code, &entry);
  *ends_block = true;
  *uses_helper = true;
  return gpsp_rv32_flow_guard(code) && emit_step(code, ir);
}

bool gpsp_rv32_thumb_flow_full_emit(
    const gpsp_rv32_thumb_flow_full_ir *ir, uint8_t **translation_ptr,
    uint8_t *translation_limit,
    gpsp_rv32_thumb_flow_full_layout *layout)
{
  if (!ir || !translation_ptr || !*translation_ptr ||
      !translation_limit || !layout || !ir->count ||
      ir->count > GPSP_RV32_THUMB_FLOW_MAX_OPS ||
      (ir->pc & 1) || ir->pc > UINT32_MAX - ir->count * 2 ||
      ((uintptr_t)*translation_ptr & 3) ||
      *translation_ptr > translation_limit)
    return false;

  uint8_t *begin = *translation_ptr;
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, begin,
      (size_t)(translation_limit - begin));
  gpsp_rv32_thumb_flow_full_layout result;
  memset(&result, 0, sizeof(result));
  uint8_t needed[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint8_t lazy[GPSP_RV32_THUMB_FLOW_MAX_OPS] = {0};
  analyze_flags(ir, needed);
  analyze_lazy_add(ir, needed, lazy);
  /* Worst-case ROM waits bound unchecked work to 48 cycles. */
  unsigned budget_distance = 0;
  for (unsigned i = 0; i < GPSP_RV32_THUMB_FLOW_MAX_OPS; i++)
  {
    result.entry_offset[i] = UINT16_MAX;
    result.body_offset[i] = UINT16_MAX;
  }

  for (unsigned i = 0; i < ir->count;)
  {
    unsigned end = i + 1;
    uint32_t pc = ir->pc + i * 2;
    if (ir->rom_region && i + 1 < ir->count &&
        (ir->ops[i].opcode & 0xf800) == 0xf000 &&
        (ir->ops[i + 1].opcode & 0xf800) == 0xf800 &&
        !ir->ops[i + 1].entry_required &&
        (pc >> 15) == ((pc + 2) >> 15) &&
        pc + 2 != idle_loop_target_pc)
    {
      uint32_t target = bl_pair_target(pc, ir->ops[i].opcode,
          ir->ops[i + 1].opcode);
      if ((target >> 24) >= 8 && (target >> 24) <= 13 &&
          target != pc + 2)
      {
        if (emit_bl_pair(&code, begin, pc, target, i, &result))
        {
          result.guest_ops = i + 2;
          result.native_ops += 2;
          result.region_count++;
          result.entry_count++;
          result.ends_block = true;
          break;
        }
      }
    }
    if (ir->rom_region && (pc >> 24) >= 8 && (pc >> 24) <= 13)
    {
      /* Each stub site checks the budget. After the last check, a slow
         Game Pak word (18) plus the rest of a 48-cycle region (42 without
         the site's own 6) stays within the scheduler's 64-cycle bound. */
      unsigned worst = 0;
      unsigned sites = 0;
      end = i;
      while (end < ir->count)
      {
        const gpsp_rv32_thumb_flow_op *op = &ir->ops[end];
        uint32_t candidate = ir->pc + end * 2;
        bool literal = op->literal_constant;
        bool site = op->kind == GPSP_RV32_THUMB_FLOW_MEMORY && !literal &&
            (op->opcode & 0xf800) != 0x4800;
        uint32_t literal_pc = (candidate & ~3U) + 4 +
            ((op->opcode & 0xff) << 2);
        unsigned next = pure_worst_cost(candidate, literal) +
            (site ? 6 : 0);
        unsigned limit = 48;
        if ((op->kind != GPSP_RV32_THUMB_FLOW_ALU && !literal && !site) ||
            (literal && (op->opcode & 0xf800) != 0x4800) ||
            (literal && (literal_pc >> 15) != (candidate >> 15)) ||
            (end > i && op->entry_required) ||
            (candidate >> 15) != (pc >> 15) ||
            (candidate >> 24) != (pc >> 24) ||
            ((candidate + 2) >> 24) != (pc >> 24) ||
            candidate + 2 == idle_loop_target_pc ||
            worst + next > limit)
          break;
        worst += next;
        sites += site;
        end++;
      }
      if (sites && end - i >= 2)
      {
        if (!emit_memory_region(&code, begin, ir, i, end,
                needed, lazy, &result))
          return false;
        result.guest_ops = end;
        result.native_ops += end - i;
        result.region_count++;
        result.entry_count++;
        budget_distance = FLOW_ALU_PREFLIGHT_LIMIT;
        i = end;
        continue;
      }
      end = i + 1;
    }
    if (ir->rom_region &&
        (ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_ALU ||
         ir->ops[i].literal_constant))
    {
      unsigned worst = 0;
      end = i;
      while (end < ir->count && end - i < 15)
      {
        const gpsp_rv32_thumb_flow_op *op = &ir->ops[end];
        uint32_t candidate = ir->pc + end * 2;
        bool literal = op->literal_constant;
        uint32_t literal_pc = (candidate & ~3U) + 4 +
            ((op->opcode & 0xff) << 2);
        unsigned next = pure_worst_cost(candidate, literal);
        if ((op->kind != GPSP_RV32_THUMB_FLOW_ALU && !literal) ||
            (literal && (op->opcode & 0xf800) != 0x4800) ||
            (literal && (literal_pc >> 15) != (candidate >> 15)) ||
            (end > i && op->entry_required) ||
            (candidate >> 24) < 8 || (candidate >> 24) > 13 ||
            (candidate >> 15) != (pc >> 15) ||
            ((candidate + 2) >> 24) != (pc >> 24) ||
            candidate + 2 == idle_loop_target_pc ||
            worst + next > 48)
          break;
        worst += next;
        end++;
      }
      if (end > i && end < ir->count &&
          (ir->ops[end].kind == GPSP_RV32_THUMB_FLOW_B ||
           ir->ops[end].kind == GPSP_RV32_THUMB_FLOW_BCC) &&
          !ir->ops[end].entry_required)
      {
        uint32_t branch_pc = ir->pc + end * 2;
        bool conditional = ir->ops[end].kind ==
            GPSP_RV32_THUMB_FLOW_BCC;
        uint32_t target = branch_target(branch_pc,
            ir->ops[end].opcode, conditional);
        if ((target >> 24) == (pc >> 24) &&
            ((branch_pc + 2) >> 24) == (pc >> 24) &&
            target != idle_loop_target_pc &&
            branch_pc + 2 != idle_loop_target_pc &&
            worst + pure_worst_cost(branch_pc, false) + 9 <= 48)
        {
          if (!emit_branch_region(&code, begin, ir, i, end,
                  needed, lazy, &result))
            return false;
          result.guest_ops = end + 1;
          result.native_ops += end + 1 - i;
          result.region_count++;
          result.entry_count++;
          result.fused_branches++;
          result.ends_block = !conditional;
          budget_distance = worst + pure_worst_cost(branch_pc, false) + 9;
          if (result.ends_block)
            break;
          i = end + 1;
          continue;
        }
      }
      end = i + 1;
    }
    if (ir->rom_region &&
        (ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_ALU ||
         ir->ops[i].literal_constant))
    {
      unsigned worst = 0;
      bool has_literal = false;
      end = i;
      while (end < ir->count)
      {
        const gpsp_rv32_thumb_flow_op *op = &ir->ops[end];
        uint32_t candidate = ir->pc + end * 2;
        bool literal = op->literal_constant;
        uint32_t literal_pc = (candidate & ~3U) + 4 +
            ((op->opcode & 0xff) << 2);
        unsigned next = pure_worst_cost(candidate, literal);
        if ((op->kind != GPSP_RV32_THUMB_FLOW_ALU && !literal) ||
            (literal && (op->opcode & 0xf800) != 0x4800) ||
            (literal && (literal_pc >> 15) != (candidate >> 15)) ||
            (end > i && op->entry_required) ||
            (candidate >> 24) < 8 || (candidate >> 24) > 13 ||
            (candidate >> 15) != (pc >> 15) ||
            (candidate >> 24) != (pc >> 24) ||
            ((candidate + 2) >> 24) != (pc >> 24) ||
            candidate + 2 == idle_loop_target_pc ||
            worst + next > 48)
          break;
        worst += next;
        has_literal |= literal;
        end++;
      }
      if (end - i >= 2 && has_literal)
      {
        if (!emit_mixed_region(&code, begin, ir, i, end,
                needed, lazy, &result))
          return false;
        result.guest_ops = end;
        result.native_ops += end - i;
        result.region_count++;
        result.entry_count++;
        budget_distance = worst;
        i = end;
        continue;
      }
      end = i + 1;
    }
    if (ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_ALU &&
        !ir->ops[i].literal_constant &&
        pc + 2 != idle_loop_target_pc)
    {
      while (end < ir->count &&
          ir->ops[end].kind == GPSP_RV32_THUMB_FLOW_ALU &&
          !ir->ops[end].literal_constant &&
          !ir->ops[end].entry_required &&
          ((ir->pc + end * 2) >> 15) == (pc >> 15) &&
          ((ir->pc + end * 2 + 2) >> 24) == ((pc + 2) >> 24) &&
          ir->pc + (end + 1) * 2 != idle_loop_target_pc)
        end++;
    }
    if (end - i >= 2)
    {
      if (!emit_alu_region(&code, begin, ir, i, end,
              needed, lazy, &result))
        return false;
      result.guest_ops = end;
      result.native_ops += end - i;
      result.cold_stubs += (end - i) *
          gpsp_rv32_flow_fetch_bound(pc + 2, true) >
          FLOW_ALU_PREFLIGHT_LIMIT;
      result.region_count++;
      result.entry_count++;
      unsigned region_bound = (end - i) *
          gpsp_rv32_flow_fetch_bound(pc + 2, true);
      budget_distance = region_bound > FLOW_ALU_PREFLIGHT_LIMIT ?
          FLOW_ALU_PREFLIGHT_LIMIT : region_bound;
      i = end;
      continue;
    }

    if (!record_entry(&code, begin, &result.entry_offset[i]) ||
        !record_entry(&code, begin, &result.body_offset[i]))
      return false;
    uint8_t *link_site = NULL;
    uint32_t link_target = 0;
    bool ends_block;
    bool uses_helper;
    bool guard_entry = true;
    bool memory_resume = false;
    unsigned worst_cycles = ir->ops[i].literal_constant ? 28 :
        ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_ALU ? 9 :
        ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_BCC ||
        ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_B ? 18 : 48;
    /* A chunk head and every local target retain a budget guard. */
    guard_entry = !ir->rom_region || i == 0 ||
        ir->ops[i].entry_required ||
        budget_distance + worst_cycles > FLOW_ALU_PREFLIGHT_LIMIT ||
        (!ir->ops[i].literal_constant &&
         ir->ops[i].kind != GPSP_RV32_THUMB_FLOW_ALU &&
         ir->ops[i].kind != GPSP_RV32_THUMB_FLOW_BCC &&
         ir->ops[i].kind != GPSP_RV32_THUMB_FLOW_B);
    memory_resume = ir->rom_region &&
        ir->ops[i].kind == GPSP_RV32_THUMB_FLOW_MEMORY &&
        !ir->ops[i].literal_constant &&
        (ir->ops[i].opcode & 0xf800) != 0x4800 &&
        pc + 2 != idle_loop_target_pc;
    if (memory_resume)
      guard_entry = false;
    if (!emit_single(&code, ir, &ir->ops[i], pc, needed[i], lazy[i],
            guard_entry, memory_resume,
            &ends_block, &uses_helper,
            &link_site, &link_target))
      return false;
    if (!guard_entry && !uses_helper && !memory_resume)
      result.unguarded_mask |= 1U << i;
    budget_distance = guard_entry ? worst_cycles :
        budget_distance + worst_cycles;
    if (link_site)
    {
      if (result.link_count == GPSP_RV32_THUMB_FLOW_MAX_OPS ||
          !record_offset(begin, link_site,
              &result.link_offset[result.link_count]))
        return false;
      result.link_target[result.link_count++] = link_target;
    }
    result.guest_ops = i + 1;
    result.helper_ops += uses_helper;
    result.native_ops += !uses_helper;
    result.region_count++;
    result.entry_count++;
    result.ends_block = ends_block;
    if (result.ends_block)
      break;
    i++;
  }
  if (!record_offset(begin, code.ptr, &result.code_bytes))
    return false;
  *translation_ptr = code.ptr;
  *layout = result;
  return true;
}

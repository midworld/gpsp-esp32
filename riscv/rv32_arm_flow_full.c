/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_flow_full.h"

#include "rv32_arm_alu.h"
#include "rv32_arm_branch.h"
#include "rv32_arm_flow_alu.h"
#include "rv32_arm_memory.h"
#include "rv32_arm_mul.h"
#include "rv32_arm_system.h"
#include "rv32_block_link.h"
#include "rv32_fallback.h"
#include "rv32_flow_timing.h"
#include "rv32_interpreter_stub.h"
#include "rv32_register_map.h"

#include <limits.h>
#include <string.h>

extern uint32_t idle_loop_target_pc;

static uint8_t condition_flags(unsigned condition)
{
  static const uint8_t flags[16] =
  {
    4, 4, 2, 2, 8, 8, 1, 1,
    6, 6, 9, 9, 13, 13, 0, 0,
  };
  return flags[condition];
}

static bool is_branch(uint32_t opcode)
{
  return ((opcode & 0x0ffffff0u) == 0x012fff10u &&
          (opcode & 15) != 15) ||
      (opcode & 0x0e000000u) == 0x0a000000u;
}

/* Switch slot trick: some games (seen in Doom 2) swap RAM NOPs
   (MOV r0, r0) and B every frame. Once a swap is seen, the NOP there is
   emitted as a branch to the next op, so later swaps patch it in place. */
#define SWITCH_SLOTS 32
static uint32_t switch_slots[SWITCH_SLOTS];
static unsigned switch_slot_count;

/* Mirrors share one slot. */
static uint32_t switch_address(uint32_t pc)
{
  return (pc >> 24) == 2 ? 0x02000000u | (pc & 0x3ffffu) :
      0x03000000u | (pc & 0x7fffu);
}

bool gpsp_rv32_arm_flow_switch_known(uint32_t pc)
{
  pc = switch_address(pc);
  for (unsigned i = 0; i < switch_slot_count; i++)
  {
    if (switch_slots[i] == pc)
      return true;
  }
  return false;
}

void gpsp_rv32_arm_flow_switch_note(uint32_t pc)
{
  pc = switch_address(pc);
  if (switch_slot_count < SWITCH_SLOTS &&
      !gpsp_rv32_arm_flow_switch_known(pc))
    switch_slots[switch_slot_count++] = pc;
}

unsigned gpsp_rv32_arm_flow_switch_list(const uint32_t **slots)
{
  *slots = switch_slots;
  return switch_slot_count;
}

void gpsp_rv32_arm_flow_switch_clear(void)
{
  switch_slot_count = 0;
}

/* Track RAM imm8 patch attempts; learned ops read the byte at run time. */
#define IMMEDIATE_SITES 32
#define IMMEDIATE_LEARN 4
static struct
{
  uint32_t pc;
  const uint8_t *byte;
  unsigned hits;
} immediate_sites[IMMEDIATE_SITES];
static unsigned immediate_site_count;

static const uint8_t *immediate_byte(uint32_t pc)
{
  if ((pc >> 24) != 2 && (pc >> 24) != 3)
    return NULL;
  pc = switch_address(pc);
  for (unsigned i = 0; i < immediate_site_count; i++)
  {
    if (immediate_sites[i].pc == pc)
      return immediate_sites[i].hits >= IMMEDIATE_LEARN ?
          immediate_sites[i].byte : NULL;
  }
  return NULL;
}

bool gpsp_rv32_arm_flow_immediate_known(uint32_t pc)
{
  return immediate_byte(pc) != NULL;
}

bool gpsp_rv32_arm_flow_immediate_note(uint32_t pc, const uint8_t *byte)
{
  pc = switch_address(pc);
  unsigned i = 0;
  while (i < immediate_site_count && immediate_sites[i].pc != pc)
    i++;
  if (i == immediate_site_count)
  {
    if (i == IMMEDIATE_SITES)
      return false;
    immediate_sites[i].pc = pc;
    immediate_sites[i].byte = byte;
    immediate_sites[i].hits = 0;
    immediate_site_count++;
  }
  if (immediate_sites[i].hits < IMMEDIATE_LEARN)
    immediate_sites[i].hits++;
  return immediate_sites[i].hits >= IMMEDIATE_LEARN;
}

void gpsp_rv32_arm_flow_immediate_clear(void)
{
  immediate_site_count = 0;
}

static bool is_ram_switch_nop(uint32_t pc, uint32_t opcode)
{
  return opcode == 0xe1a00000u && ((pc >> 24) == 2 || (pc >> 24) == 3) &&
      gpsp_rv32_arm_flow_switch_known(pc);
}

/* A learned slot that holds B keeps the switch form for a later NOP. */
static bool is_ram_switch_b(uint32_t pc, uint32_t opcode)
{
  return (opcode & 0xff000000u) == 0xea000000u &&
      ((pc >> 24) == 2 || (pc >> 24) == 3) &&
      gpsp_rv32_arm_flow_switch_known(pc);
}

bool gpsp_rv32_arm_flow_imm_op(uint32_t opcode)
{
  return (opcode >> 28) != 0xf &&
      (opcode & 0x0e000000) == 0x02000000 &&   /* data processing #imm */
      (opcode & 0x01900000) != 0x01000000 &&   /* not MSR */
      !(opcode & 0x00000f00) &&                /* rotate 0 */
      ((opcode >> 12) & 15) != 15 && ((opcode >> 16) & 15) != 15;
}

bool gpsp_rv32_arm_flow_patch_site(uint32_t pc, uint32_t opcode,
    unsigned mode)
{
  return ((pc >> 24) == 2 || (pc >> 24) == 3) &&
      (mode == GPSP_RV32_ARM_FLOW_OP_BRANCH ||
       mode == GPSP_RV32_ARM_FLOW_OP_SWITCH ||
       (mode && gpsp_rv32_arm_flow_imm_op(opcode)));
}

static uint32_t static_target(uint32_t pc, uint32_t opcode)
{
  int32_t displacement =
      (int32_t)((opcode & 0x00ffffffu) ^ 0x00800000u) - 0x00800000;
  return pc + 8 + (uint32_t)(displacement * 4);
}

static bool shared_branch_target(uint32_t pc, uint32_t opcode,
    uint32_t *target)
{
  unsigned condition = opcode >> 28;
  if (condition >= 14 || (opcode & 0x0f000000u) != 0x0a000000u)
    return false;

  *target = static_target(pc, opcode);
  int32_t offset = (int32_t)*target;
  /* pc=0 must force an absolute PC in the shared taken exit. */
  return offset < -2048 || offset > 2047;
}

static bool is_multiply(uint32_t opcode)
{
  return (opcode & 0x0fc000f0u) == 0x00000090u ||
      (opcode & 0x0f8000f0u) == 0x00800090u;
}

/* Single, halfword and block transfers. They neither read nor write NZCV
   apart from their condition. */
static bool is_transfer(uint32_t opcode)
{
  return (opcode & 0x0c000000u) == 0x04000000u ||
      (opcode & 0x0e000000u) == 0x08000000u ||
      ((opcode & 0x0e000090u) == 0x00000090u && (opcode & 0x60u));
}

/* Group for native RAM waits. Slower paths still charge their actual waits;
   the runtime carries any excess past the scheduler's 64-cycle clamp. */
static unsigned transfer_ram_cost(uint32_t opcode)
{
  if (!is_transfer(opcode))
    return 0;
  if ((opcode & 0x0e000000u) == 0x08000000u)
    return __builtin_popcount(opcode & 0xffffu) * 6;
  bool word = (opcode & 0x0c000000u) == 0x04000000u &&
      !(opcode & 0x00400000u);
  return word ? 6 : 3;
}

/* Flags an S-bit form may write. Logical forms can also set C. */
static uint8_t flags_write(uint32_t opcode)
{
  if (is_transfer(opcode) || !(opcode & 0x00100000u))
    return 0;
  if (is_multiply(opcode))
    return 14;
  unsigned operation = (opcode >> 21) & 15;
  return (operation >= 2 && operation <= 7) ||
      operation == 10 || operation == 11 ? 15 : 14;
}

/* Flags an ALU or multiply form always writes when it executes. */
static uint8_t flags_kill(uint32_t opcode)
{
  uint8_t write = flags_write(opcode);
  return write == 15 ? 15 : write & 12;
}

/* ADC/SBC/RSC and RRX read C; logical forms may keep it. */
static uint8_t flags_input(uint32_t opcode)
{
  if (is_multiply(opcode))
    return 0;
  if (is_transfer(opcode))
  {
    /* A register offset shifted by RRX reads C. */
    bool shifted = (opcode & 0x0e000000u) == 0x06000000u;
    return shifted && !(opcode & 0x10u) && ((opcode >> 5) & 3) == 3 &&
        !((opcode >> 7) & 31) ? 2 : 0;
  }
  unsigned operation = (opcode >> 21) & 15;
  bool rrx = !(opcode & 0x02000000u) && !(opcode & 0x10u) &&
      ((opcode >> 5) & 3) == 3 && !((opcode >> 7) & 31);
  return operation == 5 || operation == 6 || operation == 7 || rrx ? 2 : 0;
}

/* RV_PC holds head_pc inside a region, which charged every fetch up front.
   A helper exit charges its own fetch, so it refunds this op and the
   ones after it. */
static bool emit_pure(gpsp_rv32_code *code, uint32_t opcode, uint32_t pc,
    uint32_t head_pc, unsigned remaining)
{
  /* The ALU, multiply and memory emitters test the ARM condition. */
  if (is_transfer(opcode))
    return gpsp_rv32_emit_arm_memory_lazy(code, opcode, pc,
        (int32_t)(pc - head_pc), (int32_t)(remaining *
            gpsp_rv32_flow_fetch_bound(head_pc + 4, false))) ==
        GPSP_RV32_ARM_MEMORY_EMITTED;
  if (is_multiply(opcode))
    return gpsp_rv32_emit_arm_mul(code, opcode, pc) ==
        GPSP_RV32_ARM_MUL_EMITTED;
  if ((opcode >> 28) == 14 && gpsp_rv32_arm_flow_direct_supported(opcode))
    return gpsp_rv32_arm_flow_emit_direct(code, opcode);
  return gpsp_rv32_emit_arm_alu(code, opcode, pc) ==
      GPSP_RV32_ARM_ALU_EMITTED;
}

/* Straight-line data processing and multiplies without PC writes. */
static bool pure_supported(uint32_t opcode, uint32_t pc)
{
  if ((opcode >> 28) == 15)
    return false;
  if (is_transfer(opcode))
  {
    /* The trial rejects transfers that write PC. */
  }
  else if (is_multiply(opcode))
  {
    if (((opcode >> 16) & 15) == 15 || ((opcode >> 12) & 15) == 15)
      return false;
  }
  else
  {
    unsigned operation = (opcode >> 21) & 15;
    bool set_flags = (opcode & 0x00100000u) != 0;
    if ((opcode & 0x0c000000u) ||
        (opcode & 0x02000090u) == 0x00000090u ||
        (operation >= 8 && operation <= 11 && !set_flags) ||
        ((opcode >> 12) & 15) == 15)
      return false;
  }
  uint32_t words[128];
  gpsp_rv32_code trial;
  gpsp_rv32_code_init(&trial, words, sizeof(words));
  return emit_pure(&trial, opcode, pc, pc, 1);
}


/* Report flags read and always written; false at control flow, PC
   writes and forms the walk does not model. */
static bool arm_flag_step(uint32_t opcode, uint8_t *read, uint8_t *kill)
{
  unsigned condition = opcode >> 28;
  *read = 0;
  *kill = 0;
  if (condition == 15)
    return false;
  *read = condition_flags(condition);
  if (is_branch(opcode))
    return false;
  if (is_transfer(opcode))
  {
    bool block = (opcode & 0x0e000000u) == 0x08000000u;
    if (block ? (opcode & 0x00408000u) != 0 :
        ((opcode >> 12) & 15) == 15)
      return false;
    *read |= flags_input(opcode);
    return true;
  }
  if (opcode & 0x0c000000u)
    return false;
  if (is_multiply(opcode))
  {
    if ((opcode & 0x00100000u) && condition == 14)
      *kill = 12;
    return true;
  }
  unsigned operation = (opcode >> 21) & 15;
  bool set = opcode & 0x00100000u;
  if ((opcode & 0x0e000090u) == 0x00000090u ||
      (!set && operation >= 8 && operation <= 11) ||
      ((opcode >> 12) & 15) == 15)
    return false;
  *read |= flags_input(opcode);
  if (set && condition == 14)
    *kill = flags_kill(opcode);
  return true;
}

uint8_t gpsp_rv32_arm_flow_live_flags(uint32_t pc, const uint32_t *opcodes,
    size_t count)
{
  uint8_t live = 0;
  uint8_t pending = 15;
  for (size_t i = 0; opcodes && i < count && pending; i++)
  {
    uint8_t read, kill;
    /* A switch slot may become a B later. */
    if (is_ram_switch_nop(pc + i * 4, opcodes[i]))
      break;
    bool more = arm_flag_step(opcodes[i], &read, &kill);
    live |= read & pending;
    if (!more)
      break;
    pending &= ~kill;
  }
  return live | pending;
}

uint32_t gpsp_rv32_arm_flow_branch_target(const gpsp_rv32_arm_flow_ir *ir,
    unsigned index)
{
  if (!ir || index >= ir->count ||
      (ir->nodes[index].opcode & 0x0f000000u) != 0x0a000000u ||
      ir->nodes[index].condition == 15 ||
      is_ram_switch_b(ir->pc + index * 4, ir->nodes[index].opcode))
    return UINT32_MAX;
  return static_target(ir->pc + index * 4, ir->nodes[index].opcode);
}

bool gpsp_rv32_arm_flow_full_decode(uint32_t pc, const uint32_t *opcodes,
    size_t count, uint32_t required_entry_mask, gpsp_rv32_arm_flow_ir *out)
{
  if (!opcodes || !out || !count ||
      count > GPSP_RV32_ARM_FLOW_MAX_OPS || (pc & 3) ||
      pc > UINT32_MAX - count * 4)
    return false;

  uint8_t region[GPSP_RV32_ARM_FLOW_MAX_OPS] = {1};
  uint32_t end = pc + count * 4;
  for (size_t i = 0; i < count; i++)
  {
    uint32_t opcode = opcodes[i];
    if ((opcode & 0x0e000000u) == 0x0a000000u)
    {
      uint32_t target = static_target(pc + i * 4, opcode);
      if (target >= pc && target < end && !(target & 3))
        region[(target - pc) / 4] = 1;
    }
    if (required_entry_mask & (1u << i))
      region[i] = 1;
    if (pc + i * 4 == idle_loop_target_pc)
      region[i] = 1;
  }

  out->pc = pc;
  out->count = (uint8_t)count;
  out->fallthrough_live = 15;
  unsigned run_cost = 0;
  unsigned run_data = 0;
  for (size_t i = 0; i < count; i++)
  {
    gpsp_rv32_arm_flow_node *node = &out->nodes[i];
    node->opcode = opcodes[i];
    node->target_live = 15;
    node->kind = is_branch(opcodes[i]) ||
        is_ram_switch_nop(pc + i * 4, opcodes[i]) ?
        GPSP_RV32_ARM_FLOW_BRANCH :
        pure_supported(opcodes[i], pc + i * 4) ?
        GPSP_RV32_ARM_FLOW_DIRECT_ALU : GPSP_RV32_ARM_FLOW_OTHER;
    node->condition = opcodes[i] >> 28;
    node->flags_read = condition_flags(node->condition);
    node->flags_write = 0;
    if (node->kind == GPSP_RV32_ARM_FLOW_OTHER)
      node->flags_read = 15;
    else if (node->kind == GPSP_RV32_ARM_FLOW_DIRECT_ALU)
    {
      node->flags_read |= flags_input(opcodes[i]);
      node->flags_write = flags_write(opcodes[i]);
    }
    node->entry_required = (required_entry_mask >> i) & 1;
    node->region_start = region[i];
    /* Pure runs share one guard, cycle charge and PC update. */
    if (i && (node->kind != GPSP_RV32_ARM_FLOW_DIRECT_ALU ||
        out->nodes[i - 1].kind != GPSP_RV32_ARM_FLOW_DIRECT_ALU ||
        ((pc + (i + 1) * 4) >> 24) !=
            ((pc + i * 4) >> 24)))
      node->region_start = 1;
    /* Keep RAM paths within 64 cycles, except a single large transfer.
       Slow paths may delay events but must not lose charged cycles. */
    if (node->kind == GPSP_RV32_ARM_FLOW_DIRECT_ALU)
    {
      unsigned data = transfer_ram_cost(opcodes[i]);
      unsigned cost = gpsp_rv32_flow_fetch_bound(pc + i * 4 + 4, false) +
          data;
      if (node->region_start)
        run_cost = run_data = 0;
      else if ((run_data || data) && run_cost + cost > 64)
      {
        node->region_start = 1;
        run_cost = run_data = 0;
      }
      run_cost += cost;
      run_data += data;
    }
  }
  return true;
}

static bool emit_constant(gpsp_rv32_code *code, unsigned rd, uint32_t value)
{
  int low = value & 0xfff;
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((value + 0x800u) >> 12) & 0xfffff;
  return (!upper || gpsp_rv32_lui(code, rd, upper)) &&
      (!low ? (upper || gpsp_rv32_addi(code, rd, X0, 0)) :
       gpsp_rv32_addi(code, rd, upper ? rd : X0, low));
}

static bool emit_interpreter(gpsp_rv32_code *code)
{
  return gpsp_rv32_emit_interpreter_step_stub(code,
      (uintptr_t)gpsp_rv32_interpreter_step_stub) ==
      GPSP_RV32_FALLBACK_EMITTED;
}

/* Backward NZCV liveness; unknown successors keep all flags live. */
static void analyze_flags(const gpsp_rv32_arm_flow_ir *ir, uint8_t *needed)
{
  uint8_t live_in[GPSP_RV32_ARM_FLOW_MAX_OPS] = {0};
  bool changed;
  do
  {
    changed = false;
    for (unsigned i = ir->count; i-- > 0;)
    {
      const gpsp_rv32_arm_flow_node *node = &ir->nodes[i];
      uint32_t pc = ir->pc + i * 4;
      uint8_t out = i + 1 == ir->count ? ir->fallthrough_live :
          live_in[i + 1];
      uint8_t in;
      if (node->kind == GPSP_RV32_ARM_FLOW_BRANCH)
      {
        uint32_t opcode = node->opcode;
        uint8_t target_live = node->target_live;
        /* A call may return to code that reads the caller's flags. */
        if ((opcode & 0x0f000000u) == 0x0a000000u &&
            !is_ram_switch_b(pc, opcode))
        {
          uint32_t target = static_target(pc, opcode);
          if (target >= ir->pc && target < ir->pc + ir->count * 4)
            target_live = live_in[(target - ir->pc) / 4];
        }
        out = node->condition == 14 ? target_live : out | target_live;
        in = node->flags_read | out;
      }
      else if (node->kind == GPSP_RV32_ARM_FLOW_DIRECT_ALU)
      {
        uint8_t kill = node->condition == 14 ?
            flags_kill(node->opcode) : 0;
        in = node->flags_read | (out & ~kill);
        needed[i] = node->flags_write & out;
      }
      else
        in = 15;
      if (live_in[i] != in)
      {
        live_in[i] = in;
        changed = true;
      }
    }
  } while (changed);
}

static bool emit_direct_region(gpsp_rv32_code *code, uint8_t *begin,
    gpsp_rv32_arm_flow_layout *layout, const gpsp_rv32_arm_flow_ir *ir,
    unsigned start, unsigned end, const uint8_t *needed,
    uint8_t **cold_site)
{
  uint32_t pc = ir->pc + start * 4;
  unsigned count = end - start;
  *cold_site = NULL;
  /* Decode bounds RAM paths, except single atomic transfers.
     Longer pure runs check their fetch cost before execution. */
  if (count * gpsp_rv32_flow_fetch_bound(pc + 4, false) > 64)
  {
    if (!gpsp_rv32_flow_preflight_seq(code, pc + 4,
            count, false, cold_site))
      return false;
  }
  else if (!gpsp_rv32_flow_guard(code) ||
      !gpsp_rv32_flow_charge_seq(code, pc + 4, count, false))
    return false;

  for (unsigned i = start; i < end; i++)
  {
    uint32_t opcode = ir->nodes[i].opcode;
    unsigned operation = (opcode >> 21) & 15;
    uint8_t *op = code->ptr;
    unsigned mode = GPSP_RV32_ARM_FLOW_OP_PLAIN;
    if (ir->nodes[i].flags_write && !needed[i])
    {
      /* Dead flags: a compare does nothing; other forms drop S. */
      if (!is_multiply(opcode) && operation >= 8 && operation <= 11)
        mode = GPSP_RV32_ARM_FLOW_OP_DROPPED;
      else
      {
        opcode &= ~0x00100000u;
        mode = GPSP_RV32_ARM_FLOW_OP_NO_FLAGS;
      }
    }
    const uint8_t *immediate = mode == GPSP_RV32_ARM_FLOW_OP_DROPPED ||
        !gpsp_rv32_arm_flow_dynamic_supported(opcode) ? NULL :
        immediate_byte(ir->pc + i * 4);
    if (immediate)
    {
      if (!gpsp_rv32_arm_flow_emit_dynamic(code, opcode, immediate))
        return false;
      mode = GPSP_RV32_ARM_FLOW_OP_DYNAMIC;
    }
    else if (mode != GPSP_RV32_ARM_FLOW_OP_DROPPED)
    {
      /* A patched op is emitted again at the same length. */
      bool fixed = gpsp_rv32_arm_flow_patch_site(ir->pc + i * 4,
          ir->nodes[i].opcode, mode);
      if ((fixed && !gpsp_rv32_fixed_begin(code)) ||
          !emit_pure(code, opcode, ir->pc + i * 4, pc, end - i) ||
          (fixed && !gpsp_rv32_fixed_end(code)))
        return false;
    }
    if ((size_t)(op - begin) <= UINT16_MAX && code->ptr - op <= UINT8_MAX)
    {
      layout->op_offset[i] = (uint16_t)(op - begin);
      layout->op_bytes[i] = (uint8_t)(code->ptr - op);
      layout->op_mode[i] = (uint8_t)mode;
    }
  }
  return gpsp_rv32_flow_advance_pc(code, pc, count, false);
}


static bool record_link(gpsp_rv32_code *code, uint8_t *begin,
    gpsp_rv32_arm_flow_layout *layout, uint32_t target)
{
  uint8_t *site = NULL;
  if (layout->link_count == GPSP_RV32_ARM_FLOW_MAX_OPS ||
      !gpsp_rv32_emit_link_slot(code, &site) || !site ||
      (size_t)(site - begin) > UINT16_MAX)
    return false;
  unsigned index = layout->link_count++;
  layout->link_offset[index] = (uint16_t)(site - begin);
  layout->link_target[index] = target;
  return true;
}

static bool emit_branch(gpsp_rv32_code *code, uint8_t *begin,
    gpsp_rv32_arm_flow_layout *layout, unsigned index, uint32_t pc,
    uint32_t opcode, bool *terminal,
    gpsp_rv32_arm_condition_skip *shared_jump)
{
  if (!gpsp_rv32_flow_guard(code))
    return false;
  unsigned condition = opcode >> 28;
  if (condition == 15)
    return gpsp_rv32_flow_advance(code, pc, false);

  /* The inverse skip jumps to the taken exit. */
  if (shared_jump)
    return gpsp_rv32_emit_arm_condition_skip(code, condition ^ 1,
            shared_jump) && gpsp_rv32_flow_advance(code, pc, false);

  gpsp_rv32_arm_condition_skip skip = {{NULL, NULL}, 0};
  if (condition != 14 &&
      !gpsp_rv32_emit_arm_condition_skip(code, condition, &skip))
    return false;
  bool bx = (opcode & 0x0ffffff0u) == 0x012fff10u;
  if (bx)
  {
    /* A conditional BX not taken goes on to the next op. */
    if (gpsp_rv32_emit_arm_branch_taken(code, opcode, pc) !=
            GPSP_RV32_ARM_BRANCH_EXIT ||
        !gpsp_rv32_flow_dynamic_arm_bx(code) ||
        !gpsp_rv32_patch_arm_condition_skip(code, &skip, code->ptr) ||
        (condition != 14 && !gpsp_rv32_flow_advance(code, pc, false)))
      return false;
    *terminal = condition == 14;
    return true;
  }

  /* Switch slot trick: a NOP charges and moves like a NOP, then links
     to the next op. Its flags stay live (target_live 15) for any B. */
  bool nop = is_ram_switch_nop(pc, opcode);
  bool slot = nop || is_ram_switch_b(pc, opcode);
  uint32_t target = nop ? pc + 4 : static_target(pc, opcode);
  if (!nop && (opcode & 0x01000000u) &&
      !emit_constant(code, RV_GUEST_LR, pc + 4))
    return false;
  /* SMC expects the link slot immediately after the fixed-length advance. */
  bool fixed = condition == 14 && gpsp_rv32_arm_flow_patch_site(pc, opcode,
      slot ? GPSP_RV32_ARM_FLOW_OP_SWITCH : GPSP_RV32_ARM_FLOW_OP_BRANCH);
  if (fixed && !gpsp_rv32_align4(code))
    return false;
  uint8_t *advance = code->ptr;
  if ((fixed && !gpsp_rv32_fixed_begin(code)) ||
      !(nop ? gpsp_rv32_flow_advance(code, pc, false) :
        gpsp_rv32_flow_branch_advance(code, pc, target, false)) ||
      (fixed && !gpsp_rv32_fixed_end(code)))
    return false;
  if (condition == 14 && (size_t)(advance - begin) <= UINT16_MAX &&
      code->ptr - advance <= UINT8_MAX)
  {
    layout->op_offset[index] = (uint16_t)(advance - begin);
    layout->op_bytes[index] = (uint8_t)(code->ptr - advance);
    layout->op_mode[index] = slot ? GPSP_RV32_ARM_FLOW_OP_SWITCH :
        GPSP_RV32_ARM_FLOW_OP_BRANCH;
  }
  if (!record_link(code, begin, layout, target) ||
      !gpsp_rv32_patch_arm_condition_skip(code, &skip, code->ptr) ||
      (condition != 14 &&
       !gpsp_rv32_flow_advance(code, pc, false)))
    return false;
  return true;
}

static bool emit_other(gpsp_rv32_code *code, uint32_t pc,
    uint32_t opcode, bool *terminal, bool *helper)
{
  if (!gpsp_rv32_flow_guard(code))
    return false;
  gpsp_rv32_code_mark body = gpsp_rv32_code_save(code);

  gpsp_rv32_arm_system_result system =
      gpsp_rv32_emit_arm_system(code, opcode, pc);
  if (system == GPSP_RV32_ARM_SYSTEM_BUFFER_FULL)
    return false;
  if (system == GPSP_RV32_ARM_SYSTEM_EXIT)
  {
    *terminal = true;
    return true;
  }
  if (system == GPSP_RV32_ARM_SYSTEM_EMITTED)
    return gpsp_rv32_flow_advance(code, pc, false);

  gpsp_rv32_arm_alu_result alu =
      gpsp_rv32_emit_arm_alu_block(code, opcode, pc);
  if (alu == GPSP_RV32_ARM_ALU_BUFFER_FULL)
    return false;
  /* A conditional PC write not taken goes on to the next op. */
  bool conditional = (opcode >> 28) != 14;
  if (alu == GPSP_RV32_ARM_ALU_EXIT)
  {
    *terminal = !conditional;
    return gpsp_rv32_flow_dynamic_arm_pc_exit(code, pc, conditional);
  }
  if (alu == GPSP_RV32_ARM_ALU_EMITTED)
    return gpsp_rv32_flow_advance(code, pc, false);

  gpsp_rv32_arm_mul_result mul = gpsp_rv32_emit_arm_mul(code, opcode, pc);
  if (mul == GPSP_RV32_ARM_MUL_BUFFER_FULL)
    return false;
  if (mul == GPSP_RV32_ARM_MUL_EMITTED)
    return gpsp_rv32_flow_advance(code, pc, false);

  gpsp_rv32_arm_memory_result memory =
      gpsp_rv32_emit_arm_memory(code, opcode, pc);
  if (memory == GPSP_RV32_ARM_MEMORY_BUFFER_FULL)
    return false;
  if (memory == GPSP_RV32_ARM_MEMORY_PC_EXIT)
  {
    *terminal = !conditional;
    return gpsp_rv32_flow_dynamic_arm_pc_exit(code, pc, conditional);
  }
  if (memory == GPSP_RV32_ARM_MEMORY_EMITTED)
    return gpsp_rv32_flow_advance(code, pc, false);

  /* The interpreter owns this instruction's fetch and hardware effects. */
  gpsp_rv32_code_restore(code, &body);
  *terminal = true;
  *helper = true;
  return emit_interpreter(code);
}

typedef struct
{
  uint8_t index[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint8_t count[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint8_t used_count[GPSP_RV32_ARM_FLOW_MAX_OPS];
  uint32_t target[GPSP_RV32_ARM_FLOW_MAX_OPS];
  gpsp_rv32_arm_condition_skip jump[GPSP_RV32_ARM_FLOW_MAX_OPS];
  unsigned groups;
} gpsp_rv32_arm_shared_exits;

static bool emit_full(const gpsp_rv32_arm_flow_ir *ir,
    uint8_t **translation_ptr, uint8_t *translation_limit,
    gpsp_rv32_arm_flow_layout *layout,
    gpsp_rv32_arm_shared_exits *shared)
{
  if (!ir || !layout || !translation_ptr || !*translation_ptr ||
      !translation_limit || !ir->count ||
      ir->count > GPSP_RV32_ARM_FLOW_MAX_OPS ||
      (ir->pc & 3) || ir->pc > UINT32_MAX - ir->count * 4 ||
      ((uintptr_t)*translation_ptr & 3) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit)
    return false;

  uint8_t *begin = *translation_ptr;
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, begin, translation_limit - begin);
  gpsp_rv32_arm_flow_layout result = {0};
  uint8_t *cold_sites[GPSP_RV32_ARM_FLOW_MAX_OPS];
  unsigned cold_count = 0;
  uint8_t needed[GPSP_RV32_ARM_FLOW_MAX_OPS] = {0};
  analyze_flags(ir, needed);
  for (unsigned i = 0; i < GPSP_RV32_ARM_FLOW_MAX_OPS; i++)
    result.entry_offset[i] = UINT16_MAX;

  for (unsigned i = 0; i < ir->count;)
  {
    /* Region entries are published as offsets in four-byte units. */
    if (!gpsp_rv32_align4(&code) ||
        (size_t)(code.ptr - begin) > UINT16_MAX ||
        !ir->nodes[i].region_start)
      goto failed;
    result.entry_offset[i] = (uint16_t)(code.ptr - begin);
    result.region_count++;
    bool terminal = false;
    if (ir->nodes[i].kind == GPSP_RV32_ARM_FLOW_DIRECT_ALU)
    {
      unsigned end = i + 1;
      while (end < ir->count && !ir->nodes[end].region_start)
        end++;
      uint8_t *cold_site;
      if (!emit_direct_region(&code, begin, &result, ir, i, end, needed,
              &cold_site))
        goto failed;
      if (cold_site)
        cold_sites[cold_count++] = cold_site;
      i = end;
    }
    else
    {
      uint32_t pc = ir->pc + i * 4;
      bool helper = false;
      if (!(ir->nodes[i].kind == GPSP_RV32_ARM_FLOW_BRANCH ?
            emit_branch(&code, begin, &result, i, pc,
                ir->nodes[i].opcode, &terminal,
                !shared || shared->index[i] == UINT8_MAX ? NULL :
                &shared->jump[i]) :
            emit_other(&code, pc, ir->nodes[i].opcode, &terminal,
                &helper)))
        goto failed;
      result.helper_ops += helper;
      i++;
    }
    result.guest_ops = (uint8_t)i;
    if (terminal)
    {
      result.ends_block = true;
      break;
    }
  }

  bool shared_used = false;
  if (shared)
  {
    for (unsigned i = 0; i < result.guest_ops; i++)
      if (shared->jump[i].count)
      {
        shared_used = true;
        shared->used_count[shared->index[i]]++;
      }
    for (unsigned group = 0; group < shared->groups; group++)
      if (shared->used_count[group] == 1)
        goto failed;
  }
  if (shared_used || cold_count)
  {
    uint8_t *skip = NULL;
    if (!result.ends_block)
    {
      skip = code.ptr;
      if (!gpsp_rv32_jal(&code, X0, 0))
        goto failed;
    }
    for (unsigned group = 0; shared && group < shared->groups; group++)
    {
      if (!shared->used_count[group])
        continue;
      uint8_t *outlet = code.ptr;
      if (!gpsp_rv32_flow_branch_advance(&code, 0,
              shared->target[group], false) ||
          !record_link(&code, begin, &result, shared->target[group]))
        goto failed;
      for (unsigned i = 0; i < result.guest_ops; i++)
        if (shared->index[i] == group && shared->jump[i].count &&
            !gpsp_rv32_patch_arm_condition_skip(&code,
                &shared->jump[i], outlet))
          goto failed;
    }
    if (cold_count)
    {
      uint8_t *cold = code.ptr;
      if (!emit_interpreter(&code))
        goto failed;
      for (unsigned i = 0; i < cold_count; i++)
        if (!gpsp_rv32_patch_branch(&code, cold_sites[i], cold))
          goto failed;
    }
    if (skip && !gpsp_rv32_patch_branch(&code, skip, code.ptr))
      goto failed;
  }
  if ((size_t)(code.ptr - begin) > UINT16_MAX)
    goto failed;
  result.code_bytes = (uint16_t)(code.ptr - begin);
  *translation_ptr = code.ptr;
  *layout = result;
  return true;

failed:
  *translation_ptr = begin;
  return false;
}

static bool has_shared_target(const gpsp_rv32_arm_flow_ir *ir)
{
  if (!ir || !ir->count || ir->count > GPSP_RV32_ARM_FLOW_MAX_OPS)
    return false;
  for (unsigned i = 0; i < ir->count; i++)
  {
    uint32_t target;
    if (ir->nodes[i].kind != GPSP_RV32_ARM_FLOW_BRANCH ||
        !shared_branch_target(ir->pc + i * 4,
            ir->nodes[i].opcode, &target))
      continue;
    for (unsigned j = i + 1; j < ir->count; j++)
    {
      uint32_t next;
      if (ir->nodes[j].kind == GPSP_RV32_ARM_FLOW_BRANCH &&
          shared_branch_target(ir->pc + j * 4,
              ir->nodes[j].opcode, &next) && next == target)
        return true;
    }
  }
  return false;
}

static bool emit_with_shared(const gpsp_rv32_arm_flow_ir *ir,
    uint8_t **translation_ptr, uint8_t *translation_limit,
    gpsp_rv32_arm_flow_layout *layout)
{
  gpsp_rv32_arm_shared_exits shared = {0};
  for (unsigned i = 0; i < ir->count; i++)
  {
    shared.index[i] = UINT8_MAX;
    uint32_t target;
    if (ir->nodes[i].kind != GPSP_RV32_ARM_FLOW_BRANCH ||
        !shared_branch_target(ir->pc + i * 4,
            ir->nodes[i].opcode, &target))
      continue;
    unsigned group = 0;
    while (group < shared.groups && shared.target[group] != target)
      group++;
    if (group == shared.groups)
      shared.target[shared.groups++] = target;
    shared.index[i] = (uint8_t)group;
    shared.count[group]++;
  }
  for (unsigned i = 0; i < ir->count; i++)
    if (shared.index[i] != UINT8_MAX &&
        shared.count[shared.index[i]] < 2)
      shared.index[i] = UINT8_MAX;

  if (emit_full(ir, translation_ptr, translation_limit, layout, &shared))
    return true;
  /* An out-of-range RV branch or a full buffer retains the old layout. */
  return emit_full(ir, translation_ptr, translation_limit, layout, NULL);
}

bool gpsp_rv32_arm_flow_full_emit(const gpsp_rv32_arm_flow_ir *ir,
    uint8_t **translation_ptr, uint8_t *translation_limit,
    gpsp_rv32_arm_flow_layout *layout)
{
  return has_shared_target(ir) ?
      emit_with_shared(ir, translation_ptr, translation_limit, layout) :
      emit_full(ir, translation_ptr, translation_limit, layout, NULL);
}

/* Emits one op at code, which must hold its bytes. */
static bool emit_repatch(uint32_t *words, unsigned bytes, unsigned mode,
    uint32_t pc, uint32_t opcode)
{
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, words, bytes + 4);
  if (!gpsp_rv32_fixed_begin(&code))
    return false;
  /* Switch slot trick: the slot holds either the NOP or the B advance. */
  if (mode == GPSP_RV32_ARM_FLOW_OP_SWITCH && opcode == 0xe1a00000u)
    return gpsp_rv32_flow_advance(&code, pc, false) &&
        (unsigned)(code.ptr - (uint8_t *)words) == bytes;
  if (mode == GPSP_RV32_ARM_FLOW_OP_BRANCH ||
      mode == GPSP_RV32_ARM_FLOW_OP_SWITCH)
    return gpsp_rv32_flow_branch_advance(&code, pc,
            static_target(pc, opcode), false) &&
        (unsigned)(code.ptr - (uint8_t *)words) == bytes;
  if (mode == GPSP_RV32_ARM_FLOW_OP_NO_FLAGS)
    opcode &= ~0x00100000u;
  return emit_pure(&code, opcode, pc, pc, 1) &&
      (unsigned)(code.ptr - (uint8_t *)words) == bytes;
}

#if defined(GPSP_BENCHMARK_RAM_CODE)
unsigned gpsp_rv32_arm_flow_repatch_failure;
#define repatch_fail(reason) \
  (gpsp_rv32_arm_flow_repatch_failure = GPSP_RV32_REPATCH_##reason, false)
#else
#define repatch_fail(reason) false
#endif

bool gpsp_rv32_arm_flow_repatch(const uint8_t *native, unsigned bytes,
    unsigned mode, uint32_t pc, uint32_t old_opcode, uint32_t new_opcode,
    uint8_t *out)
{
  /* A dead compare stays dead whatever its operand. */
  if (mode == GPSP_RV32_ARM_FLOW_OP_DROPPED)
    return true;
  if ((mode != GPSP_RV32_ARM_FLOW_OP_PLAIN &&
       mode != GPSP_RV32_ARM_FLOW_OP_NO_FLAGS &&
       mode != GPSP_RV32_ARM_FLOW_OP_BRANCH &&
       mode != GPSP_RV32_ARM_FLOW_OP_SWITCH) || !bytes || bytes > 252)
    return repatch_fail(MODE);

  /* Emitting at two addresses exposes code that depends on its
     address; the old opcode must reproduce the code in place. */
  uint32_t first[64], second[64];
  if (!emit_repatch(first, bytes, mode, pc, old_opcode) ||
      memcmp(first, native, bytes))
    return repatch_fail(OLD);
  if (!emit_repatch(first, bytes, mode, pc, new_opcode))
    return repatch_fail(SIZE);
  if (!emit_repatch(second, bytes, mode, pc, new_opcode) ||
      memcmp(first, second, bytes))
    return repatch_fail(ADDRESS);
  memcpy(out, first, bytes);
  return true;
}
#undef repatch_fail

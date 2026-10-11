/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_thumb_ir.h"

bool gpsp_rv32_thumb_ir_decode(uint32_t pc, const uint16_t *opcodes,
    size_t count, gpsp_rv32_thumb_ir *out)
{
  if (!opcodes || !out || !count || count > GPSP_RV32_THUMB_IR_MAX_OPS ||
      (pc & 1) || pc > UINT32_MAX - count * 2 ||
      (pc & 0x7fff) + count * 2 > 0x8000)
    return false;

  const uint32_t end_pc = pc + count * 2;
  out->pc = pc;
  out->count = (uint8_t)count;

  for (size_t i = 0; i < count; i++)
  {
    uint16_t opcode = opcodes[i];
    gpsp_rv32_thumb_node *node = &out->nodes[i];

    node->opcode = opcode;
    node->rd = 0;
    node->rs = 0;
    node->imm = 0;
    node->fallthrough = (uint8_t)(i + 1);
    node->taken = (uint8_t)count;
    node->flags_read = 0;
    node->flags_write = 0;
    node->flags_live_in = 0;
    node->flags_live_out = 0;

    if ((opcode & 0xffc0) == 0x4200)
    {
      node->kind = GPSP_RV32_THUMB_IR_TST;
      node->rd = opcode & 7;
      node->rs = (opcode >> 3) & 7;
      node->flags_write = GPSP_RV32_THUMB_FLAG_N | GPSP_RV32_THUMB_FLAG_Z;
    }
    else if ((opcode & 0xff00) == 0xd000)
    {
      int32_t displacement = (int8_t)(opcode & 0xff) * 2;
      int64_t target_pc = (int64_t)pc + (int64_t)i * 2 + 4 + displacement;
      if (target_pc < (int64_t)pc + ((int64_t)i + 1) * 2 ||
          target_pc > end_pc)
        return false;

      node->kind = GPSP_RV32_THUMB_IR_BEQ;
      node->imm = opcode & 0xff;
      node->taken = (uint8_t)((target_pc - pc) / 2);
      node->flags_read = GPSP_RV32_THUMB_FLAG_Z;
    }
    else if ((opcode & 0xf800) == 0x2000)
    {
      node->kind = GPSP_RV32_THUMB_IR_MOV_IMM;
      node->rd = (opcode >> 8) & 7;
      node->imm = opcode & 0xff;
      node->flags_write = GPSP_RV32_THUMB_FLAG_N | GPSP_RV32_THUMB_FLAG_Z;
    }
    else
      return false;
  }

  for (size_t i = count; i-- > 0;)
  {
    gpsp_rv32_thumb_node *node = &out->nodes[i];
    uint8_t live = node->fallthrough == count ?
        GPSP_RV32_THUMB_FLAG_ALL :
        out->nodes[node->fallthrough].flags_live_in;

    if (node->kind == GPSP_RV32_THUMB_IR_BEQ)
      live |= node->taken == count ? GPSP_RV32_THUMB_FLAG_ALL :
          out->nodes[node->taken].flags_live_in;

    node->flags_live_out = live;
    node->flags_live_in = node->flags_read |
        (live & ~node->flags_write);
  }

  return true;
}

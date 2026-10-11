/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_branch.h"
#include "rv32_register_map.h"

#include <string.h>

static bool emit_constant(gpsp_rv32_code *code, unsigned rd, uint32_t value)
{
  int low = (int)(value & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper20 = ((value + 0x800u) >> 12) & 0xfffff;

  if (!upper20)
    return gpsp_rv32_addi(code, rd, X0, low);
  return gpsp_rv32_lui(code, rd, upper20) &&
      (!low || gpsp_rv32_addi(code, rd, rd, low));
}

bool gpsp_rv32_emit_arm_condition_skip(gpsp_rv32_code *code,
    unsigned condition, gpsp_rv32_arm_condition_skip *skip)
{
  if (!code || !code->start || !code->ptr || !code->end || !skip ||
      condition > 14 || ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return false;
  unsigned words = condition == 14 ? 0 :
      ((condition >= 8 && condition <= 9) ||
       (condition >= 12 && condition <= 13) ? 2 : 1);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < words * 4)
    return false;
  skip->count = 0;
  uint8_t *pass;
  switch (condition)
  {
    case 0:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_Z, X0, 0); /* EQ */
    case 1:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0); /* NE */
    case 2:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_C, X0, 0); /* CS */
    case 3:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_C, X0, 0); /* CC */
    case 4:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_N, X0, 0); /* MI */
    case 5:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_N, X0, 0); /* PL */
    case 6:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_V, X0, 0); /* VS */
    case 7:  skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_V, X0, 0); /* VC */
    case 8:  skip->site[skip->count++] = code->ptr;
             if (!gpsp_rv32_beq(code, RV_FLAG_C, X0, 0)) return false;
             skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0); /* HI */
    case 9:  pass = code->ptr;
             if (!gpsp_rv32_beq(code, RV_FLAG_C, X0, 0)) return false;
             skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_Z, X0, 0) &&
                 gpsp_rv32_patch_branch(code, pass, code->ptr); /* LS */
    case 10: skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_N, RV_FLAG_V, 0); /* GE */
    case 11: skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_N, RV_FLAG_V, 0); /* LT */
    case 12: skip->site[skip->count++] = code->ptr;
             if (!gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0)) return false;
             skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_bne(code, RV_FLAG_N, RV_FLAG_V, 0); /* GT */
    case 13: pass = code->ptr;
             if (!gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0)) return false;
             skip->site[skip->count++] = code->ptr;
             return gpsp_rv32_beq(code, RV_FLAG_N, RV_FLAG_V, 0) &&
                 gpsp_rv32_patch_branch(code, pass, code->ptr); /* LE */
    default: return true;
  }
}

bool gpsp_rv32_patch_arm_condition_skip(gpsp_rv32_code *code,
    const gpsp_rv32_arm_condition_skip *skip, uint8_t *target)
{
  if (!skip || skip->count > 2)
    return false;
  for (unsigned i = 0; i < skip->count; i++)
    if (!gpsp_rv32_patch_branch(code, skip->site[i], target))
      return false;
  return true;
}

static bool emit_taken_branch(gpsp_rv32_code *code, uint32_t opcode,
    uint32_t pc, bool bx)
{
  if (bx)
  {
    unsigned rm = opcode & 15;
    return gpsp_rv32_andi(code, A1, guest_reg_map[rm], 1) &&
        gpsp_rv32_andi(code, A0, guest_reg_map[rm], -2);
  }

  int32_t offset = (int32_t)(opcode & 0xffffff);
  if (offset & 0x800000)
    offset -= 0x1000000;
  if ((opcode & 0x01000000) && !emit_constant(code, RV_GUEST_LR, pc + 4))
    return false;
  return emit_constant(code, A0, pc + 8 + (uint32_t)(offset * 4));
}

gpsp_rv32_arm_branch_result gpsp_rv32_emit_arm_branch(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
  if (instruction_pc & 3)
    return GPSP_RV32_ARM_BRANCH_NEEDS_HELPER;

  bool bx = (opcode & 0x0ffffff0) == 0x012fff10;
  if (!bx && (opcode & 0x0e000000) != 0x0a000000)
    return GPSP_RV32_ARM_BRANCH_NEEDS_HELPER;
  if (bx && (opcode & 15) == 15)
    return GPSP_RV32_ARM_BRANCH_NEEDS_HELPER;

  uint32_t words[64];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  /* A failed condition keeps the fallthrough PC. */
  if (!emit_constant(&staged, A0, instruction_pc + 4) ||
      !gpsp_rv32_addi(&staged, A1, X0, 0))
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;

  unsigned condition = opcode >> 28;
  if (condition != 15)
  {
    gpsp_rv32_arm_condition_skip skip = { { NULL, NULL }, 0 };
    if (condition != 14)
      if (!gpsp_rv32_emit_arm_condition_skip(&staged, condition, &skip))
        return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
    if (!emit_taken_branch(&staged, opcode, instruction_pc, bx) ||
        !gpsp_rv32_patch_arm_condition_skip(&staged, &skip, staged.ptr))
      return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
  }

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return GPSP_RV32_ARM_BRANCH_EXIT;
}

gpsp_rv32_arm_branch_result gpsp_rv32_emit_arm_branch_taken(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
  if ((instruction_pc & 3) || (opcode >> 28) == 15)
    return GPSP_RV32_ARM_BRANCH_NEEDS_HELPER;

  bool bx = (opcode & 0x0ffffff0) == 0x012fff10;
  if ((!bx && (opcode & 0x0e000000) != 0x0a000000) ||
      (bx && (opcode & 15) == 15))
    return GPSP_RV32_ARM_BRANCH_NEEDS_HELPER;

  uint32_t words[8];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  if (!emit_taken_branch(&staged, opcode, instruction_pc, bx))
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return GPSP_RV32_ARM_BRANCH_EXIT;
}

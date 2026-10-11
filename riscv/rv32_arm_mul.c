/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_mul.h"
#include "rv32_register_map.h"

#include <string.h>

typedef struct
{
  unsigned condition;
  unsigned rd_hi;
  unsigned rd_lo;
  unsigned rn;
  unsigned rs;
  unsigned rm;
  bool is_long;
  bool is_signed;
  bool accumulate;
  bool set_flags;
} decoded_arm_mul;

static bool decode_arm_mul(uint32_t opcode, uint32_t instruction_pc,
    decoded_arm_mul *decoded)
{
  if (instruction_pc & 3)
    return false;

  decoded->condition = opcode >> 28;
  decoded->set_flags = (opcode & 0x00100000u) != 0;
  decoded->accumulate = (opcode & 0x00200000u) != 0;
  decoded->rs = (opcode >> 8) & 15;
  decoded->rm = opcode & 15;
  decoded->is_long = (opcode & 0x0f8000f0u) == 0x00800090u;

  if (decoded->is_long)
  {
    decoded->is_signed = (opcode & 0x00400000u) != 0;
    decoded->rd_hi = (opcode >> 16) & 15;
    decoded->rd_lo = (opcode >> 12) & 15;
    return decoded->rd_hi != 15 && decoded->rd_lo != 15 &&
        decoded->rm != 15 && decoded->rs != 15 &&
        decoded->rd_hi != decoded->rd_lo;
  }

  if ((opcode & 0x0fc000f0u) != 0x00000090u)
    return false;

  decoded->is_signed = false;
  decoded->rd_hi = (opcode >> 16) & 15; /* Short result Rd. */
  decoded->rn = (opcode >> 12) & 15;
  return decoded->rd_hi != 15 && decoded->rm != 15 &&
      decoded->rs != 15 &&
      (decoded->accumulate ? decoded->rn != 15 : decoded->rn == 0);
}

/* Build a 0/1 execute predicate in RV_TMP0. Flags stay in RV_FLAG_*. */
static bool emit_condition(gpsp_rv32_code *code, unsigned condition)
{
  switch (condition)
  {
    case 0:  /* EQ */
      return gpsp_rv32_addi(code, RV_TMP0, RV_FLAG_Z, 0);
    case 1:  /* NE */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_Z, 1);
    case 2:  /* CS */
      return gpsp_rv32_addi(code, RV_TMP0, RV_FLAG_C, 0);
    case 3:  /* CC */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_C, 1);
    case 4:  /* MI */
      return gpsp_rv32_addi(code, RV_TMP0, RV_FLAG_N, 0);
    case 5:  /* PL */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_N, 1);
    case 6:  /* VS */
      return gpsp_rv32_addi(code, RV_TMP0, RV_FLAG_V, 0);
    case 7:  /* VC */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_V, 1);
    case 8:  /* HI */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_Z, 1) &&
          gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_FLAG_C);
    case 9:  /* LS */
      return gpsp_rv32_xori(code, RV_TMP0, RV_FLAG_C, 1) &&
          gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_FLAG_Z);
    case 10: /* GE */
      return gpsp_rv32_xor(code, RV_TMP0, RV_FLAG_N, RV_FLAG_V) &&
          gpsp_rv32_xori(code, RV_TMP0, RV_TMP0, 1);
    case 11: /* LT */
      return gpsp_rv32_xor(code, RV_TMP0, RV_FLAG_N, RV_FLAG_V);
    case 12: /* GT */
      return gpsp_rv32_xor(code, RV_TMP0, RV_FLAG_N, RV_FLAG_V) &&
          gpsp_rv32_xori(code, RV_TMP0, RV_TMP0, 1) &&
          gpsp_rv32_xori(code, RV_TMP1, RV_FLAG_Z, 1) &&
          gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1);
    case 13: /* LE */
      return gpsp_rv32_xor(code, RV_TMP0, RV_FLAG_N, RV_FLAG_V) &&
          gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_FLAG_Z);
    default: return false;
  }
}

static bool emit_single(gpsp_rv32_code *code, const decoded_arm_mul *decoded)
{
  unsigned result = guest_reg_map[decoded->rd_hi];
  unsigned rm = guest_reg_map[decoded->rm];
  unsigned rs = guest_reg_map[decoded->rs];
  if (decoded->accumulate)
  {
    /* Keep the product separate until Rn has been read, even if Rd aliases it. */
    if (!gpsp_rv32_mul(code, RV_TMP0, rm, rs) ||
        !gpsp_rv32_add(code, result, RV_TMP0, guest_reg_map[decoded->rn]))
      return false;
  }
  else if (!gpsp_rv32_mul(code, result, rm, rs))
    return false;

  /* Multiply S changes N/Z, but keeps C/V. */
  return !decoded->set_flags ||
      (gpsp_rv32_srli(code, RV_FLAG_N, result, 31) &&
       gpsp_rv32_sltiu(code, RV_FLAG_Z, result, 1));
}

static bool emit_long(gpsp_rv32_code *code, const decoded_arm_mul *decoded)
{
  unsigned rm = guest_reg_map[decoded->rm];
  unsigned rs = guest_reg_map[decoded->rs];
  unsigned rd_lo = guest_reg_map[decoded->rd_lo];
  unsigned rd_hi = guest_reg_map[decoded->rd_hi];
  if (!decoded->accumulate)
  {
    bool low_alias = decoded->rd_lo == decoded->rm ||
        decoded->rd_lo == decoded->rs;
    if (!gpsp_rv32_mul(code, low_alias ? A2 : rd_lo, rm, rs) ||
        !(decoded->is_signed ? gpsp_rv32_mulh(code, rd_hi, rm, rs) :
                               gpsp_rv32_mulhu(code, rd_hi, rm, rs)) ||
        (low_alias && !gpsp_rv32_addi(code, rd_lo, A2, 0)))
      return false;
    return !decoded->set_flags ||
        (gpsp_rv32_srli(code, RV_FLAG_N, rd_hi, 31) &&
         gpsp_rv32_or(code, RV_TMP1, rd_lo, rd_hi) &&
         gpsp_rv32_sltiu(code, RV_FLAG_Z, RV_TMP1, 1));
  }

  /* Accumulation must read both old destination halves. */
  if (!gpsp_rv32_mul(code, A2, rm, rs) ||
      !(decoded->is_signed ? gpsp_rv32_mulh(code, RV_TMP0, rm, rs) :
                             gpsp_rv32_mulhu(code, RV_TMP0, rm, rs)))
    return false;

  if (!gpsp_rv32_add(code, A2, A2, rd_lo) ||
      !gpsp_rv32_sltu(code, RV_TMP1, A2, rd_lo) ||
      !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, rd_hi) ||
      !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, RV_TMP1))
    return false;

  if (decoded->set_flags &&
      (!gpsp_rv32_srli(code, RV_FLAG_N, RV_TMP0, 31) ||
       !gpsp_rv32_or(code, RV_TMP1, A2, RV_TMP0) ||
       !gpsp_rv32_sltiu(code, RV_FLAG_Z, RV_TMP1, 1)))
    return false;

  return gpsp_rv32_addi(code, rd_lo, A2, 0) &&
      gpsp_rv32_addi(code, rd_hi, RV_TMP0, 0);
}

gpsp_rv32_arm_mul_result gpsp_rv32_emit_arm_mul(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_MUL_BUFFER_FULL;

  decoded_arm_mul decoded = {0};
  if (!decode_arm_mul(opcode, instruction_pc, &decoded))
    return GPSP_RV32_ARM_MUL_UNSUPPORTED;

  /* On ARM7TDMI cond=1111 does not execute, but time still advances. */
  if (decoded.condition == 15)
    return GPSP_RV32_ARM_MUL_EMITTED;

  uint32_t words[64];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  uint8_t *skip_site = NULL;

  if (decoded.condition != 14)
  {
    if (!emit_condition(&staged, decoded.condition))
      return GPSP_RV32_ARM_MUL_BUFFER_FULL;
    skip_site = staged.ptr;
    if (!gpsp_rv32_beq(&staged, RV_TMP0, X0, 0))
      return GPSP_RV32_ARM_MUL_BUFFER_FULL;
  }

  if (!(decoded.is_long ? emit_long(&staged, &decoded) :
                           emit_single(&staged, &decoded)) ||
      (skip_site && !gpsp_rv32_patch_branch(&staged, skip_site, staged.ptr)))
    return GPSP_RV32_ARM_MUL_BUFFER_FULL;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_MUL_BUFFER_FULL;

  memcpy(code->ptr, words, size);
  code->ptr += size;
  return GPSP_RV32_ARM_MUL_EMITTED;
}

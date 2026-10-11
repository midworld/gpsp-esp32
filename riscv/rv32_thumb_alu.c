/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_thumb_alu.h"
#include "rv32_register_map.h"

#include <string.h>

enum {
  THUMB_FLAG_N = 8,
  THUMB_FLAG_Z = 4,
  THUMB_FLAG_C = 2,
  THUMB_FLAG_V = 1,
};

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

static bool emit_read(gpsp_rv32_code *code, unsigned rd, unsigned source,
    uint32_t pc)
{
  if (source == 15)
    return emit_constant(code, rd, pc + 4);
  return gpsp_rv32_addi(code, rd, guest_reg_map[source], 0);
}

static bool emit_write(gpsp_rv32_code *code, unsigned dest, unsigned source)
{
  return gpsp_rv32_addi(code, guest_reg_map[dest], source, 0);
}

static bool emit_nz_reg(gpsp_rv32_code *code, unsigned result,
    unsigned flags)
{
  return (!(flags & THUMB_FLAG_N) ||
          gpsp_rv32_srli(code, RV_FLAG_N, result, 31)) &&
      (!(flags & THUMB_FLAG_Z) || gpsp_rv32_sltiu(code, RV_FLAG_Z, result, 1));
}

static bool emit_nz(gpsp_rv32_code *code, unsigned flags)
{
  return emit_nz_reg(code, A2, flags);
}

/* The original operands remain intact while A2 holds the result. */
static bool emit_add_flags(gpsp_rv32_code *code, bool with_carry,
    unsigned flags, unsigned lhs, unsigned rhs)
{
  if (with_carry)
  {
    if (flags & THUMB_FLAG_C)
    {
      if (!gpsp_rv32_add(code, A2, lhs, rhs) ||
          !gpsp_rv32_sltu(code, RV_TMP0, A2, lhs) ||
          !gpsp_rv32_add(code, A2, A2, RV_FLAG_C) ||
          !gpsp_rv32_sltu(code, RV_TMP1, A2, RV_FLAG_C) ||
          !gpsp_rv32_or(code, RV_FLAG_C, RV_TMP0, RV_TMP1))
        return false;
    }
    else if (!gpsp_rv32_add(code, A2, lhs, rhs) ||
             !gpsp_rv32_add(code, A2, A2, RV_FLAG_C))
      return false;
  }
  else if (!gpsp_rv32_add(code, A2, lhs, rhs) ||
           ((flags & THUMB_FLAG_C) &&
            !gpsp_rv32_sltu(code, RV_FLAG_C, A2, lhs)))
    return false;

  if (!emit_nz(code, flags))
    return false;
  if (!(flags & THUMB_FLAG_V))
    return true;
  if (!with_carry)
  {
    /* Without overflow, the sum is below lhs exactly when rhs < 0. */
    return gpsp_rv32_slt(code, RV_TMP0, A2, lhs) &&
        gpsp_rv32_srli(code, RV_TMP1, rhs, 31) &&
        gpsp_rv32_xor(code, RV_FLAG_V, RV_TMP0, RV_TMP1);
  }
  /* Overflow changes the sign of both operands relative to the result. */
  return gpsp_rv32_xor(code, RV_TMP0, lhs, A2) &&
      gpsp_rv32_xor(code, RV_TMP1, rhs, A2) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

static bool emit_sub_flags(gpsp_rv32_code *code, bool with_carry,
    unsigned flags, unsigned lhs, unsigned rhs)
{
  if (with_carry)
  {
    if (!gpsp_rv32_xori(code, RV_TMP1, RV_FLAG_C, 1) ||
        !gpsp_rv32_sub(code, A2, lhs, rhs))
      return false;
    if ((flags & THUMB_FLAG_C) &&
        (!gpsp_rv32_sltu(code, RV_FLAG_C, lhs, rhs) ||
         !gpsp_rv32_sltu(code, RV_TMP0, A2, RV_TMP1) ||
         !gpsp_rv32_or(code, RV_FLAG_C, RV_FLAG_C, RV_TMP0)))
      return false;
    if (!gpsp_rv32_sub(code, A2, A2, RV_TMP1) ||
        ((flags & THUMB_FLAG_C) &&
         !gpsp_rv32_xori(code, RV_FLAG_C, RV_FLAG_C, 1)))
      return false;
  }
  else if (!gpsp_rv32_sub(code, A2, lhs, rhs) ||
           ((flags & THUMB_FLAG_C) &&
            (!gpsp_rv32_sltu(code, RV_FLAG_C, lhs, rhs) ||
             !gpsp_rv32_xori(code, RV_FLAG_C, RV_FLAG_C, 1))))
    return false;

  if (!emit_nz(code, flags))
    return false;
  if (!(flags & THUMB_FLAG_V))
    return true;
  if (!with_carry)
  {
    /* Without overflow, lhs < rhs exactly when the result is negative. */
    return gpsp_rv32_slt(code, RV_TMP0, lhs, rhs) &&
        ((flags & THUMB_FLAG_N) ?
         gpsp_rv32_xor(code, RV_FLAG_V, RV_TMP0, RV_FLAG_N) :
         (gpsp_rv32_srli(code, RV_TMP1, A2, 31) &&
          gpsp_rv32_xor(code, RV_FLAG_V, RV_TMP0, RV_TMP1)));
  }
  return gpsp_rv32_xor(code, RV_TMP0, lhs, rhs) &&
      gpsp_rv32_xor(code, RV_TMP1, lhs, A2) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

static bool emit_immediate_arithmetic(gpsp_rv32_code *code,
    unsigned dest, unsigned source, unsigned immediate, bool subtract,
    bool write_result, unsigned flags)
{
  if (!write_result && !flags)
    return true;

  if (!immediate)
  {
    if (write_result && source != dest &&
        !gpsp_rv32_addi(code, dest, source, 0))
      return false;
    return emit_nz_reg(code, write_result ? dest : source, flags) &&
        (!(flags & THUMB_FLAG_C) ||
         gpsp_rv32_addi(code, RV_FLAG_C, X0, subtract ? 1 : 0)) &&
        (!(flags & THUMB_FLAG_V) || gpsp_rv32_addi(code, RV_FLAG_V, X0, 0));
  }

  unsigned result = write_result ? dest : A2;
  unsigned original = source;
  if ((flags & THUMB_FLAG_V) && source == result)
  {
    if (!gpsp_rv32_addi(code, A0, source, 0))
      return false;
    original = A0;
  }

  /* The immediate is at most 255, so RV32I comparisons can use it directly. */
  if (subtract && (flags & THUMB_FLAG_C) &&
      (!gpsp_rv32_sltiu(code, RV_FLAG_C, source, immediate) ||
       !gpsp_rv32_xori(code, RV_FLAG_C, RV_FLAG_C, 1)))
    return false;
  if (!gpsp_rv32_addi(code, result, source,
          subtract ? -(int)immediate : (int)immediate) ||
      (!subtract && (flags & THUMB_FLAG_C) &&
       !gpsp_rv32_sltiu(code, RV_FLAG_C, result, immediate)) ||
      !emit_nz_reg(code, result, flags))
    return false;

  if (!(flags & THUMB_FLAG_V))
    return true;
  /* A positive immediate overflows exactly when the order flips. */
  if (!subtract)
    return gpsp_rv32_slt(code, RV_FLAG_V, result, original);
  if (flags & THUMB_FLAG_N)
    return gpsp_rv32_slti(code, RV_TMP0, original, (int)immediate) &&
        gpsp_rv32_xor(code, RV_FLAG_V, RV_TMP0, RV_FLAG_N);
#if GPSP_RV32_USE_ZBB
  return gpsp_rv32_andn(code, RV_TMP0, original, result) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
#else
  return gpsp_rv32_xori(code, RV_TMP0, result, -1) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, original) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
#endif
}

static bool emit_carry_bit(gpsp_rv32_code *code, unsigned source,
    unsigned bit)
{
#if GPSP_RV32_USE_ZBS
  if (bit != 31)
    return gpsp_rv32_bexti(code, RV_FLAG_C, source, bit);
#endif
  return gpsp_rv32_srli(code, RV_FLAG_C, source, bit) &&
      (bit == 31 || gpsp_rv32_andi(code, RV_FLAG_C, RV_FLAG_C, 1));
}

static bool emit_shift_imm(gpsp_rv32_code *code, unsigned kind,
    unsigned amount, unsigned flags, unsigned source, unsigned dest)
{
  switch (kind)
  {
    case 0: /* LSL */
      return (amount == 0 ?
              (source == dest || gpsp_rv32_addi(code, dest, source, 0)) :
              ((!(flags & THUMB_FLAG_C) ||
                emit_carry_bit(code, source, 32 - amount)) &&
               gpsp_rv32_slli(code, dest, source, amount))) &&
          emit_nz_reg(code, dest, flags);
    case 1: /* LSR, encoded zero means 32 */
      return (amount == 0 ?
              ((!(flags & THUMB_FLAG_C) ||
                gpsp_rv32_srli(code, RV_FLAG_C, source, 31)) &&
               gpsp_rv32_addi(code, dest, X0, 0)) :
              ((!(flags & THUMB_FLAG_C) ||
                emit_carry_bit(code, source, amount - 1)) &&
               gpsp_rv32_srli(code, dest, source, amount))) &&
          emit_nz_reg(code, dest, flags);
    case 2: /* ASR, encoded zero means 32 */
      return (amount == 0 ?
              ((!(flags & THUMB_FLAG_C) ||
                gpsp_rv32_srli(code, RV_FLAG_C, source, 31)) &&
               gpsp_rv32_srai(code, dest, source, 31)) :
              ((!(flags & THUMB_FLAG_C) ||
                emit_carry_bit(code, source, amount - 1)) &&
               gpsp_rv32_srai(code, dest, source, amount))) &&
          emit_nz_reg(code, dest, flags);
    default:
      return false;
  }
}

/* Thumb register shifts use the low eight bits of the count on ARM7TDMI. */
static bool emit_shift_reg(gpsp_rv32_code *code, unsigned kind,
    unsigned flags, unsigned value)
{
  uint8_t *zero = NULL, *small = NULL, *exact = NULL, *done = NULL;
  bool carry = (flags & THUMB_FLAG_C) != 0;
#if !GPSP_RV32_USE_ZBB
  if (kind == 3 && !carry)
  {
    /* Without carry output, the result needs only the low five bits. */
    return gpsp_rv32_andi(code, A1, A1, 31) &&
        gpsp_rv32_addi(code, RV_TMP0, X0, 32) &&
        gpsp_rv32_sub(code, RV_TMP0, RV_TMP0, A1) &&
        gpsp_rv32_srl(code, A2, value, A1) &&
        gpsp_rv32_sll(code, RV_TMP1, value, RV_TMP0) &&
        gpsp_rv32_or(code, A2, A2, RV_TMP1) &&
        emit_nz(code, flags);
  }
#endif
  if (!gpsp_rv32_andi(code, A1, A1, 255) ||
      !gpsp_rv32_addi(code, A2, value, 0))
    return false;
  zero = code->ptr;
  if (!gpsp_rv32_beq(code, A1, X0, 0))
    return false;

  if (kind == 3) /* ROR */
  {
#if GPSP_RV32_USE_ZBB
    if (!gpsp_rv32_ror(code, A2, value, A1) ||
        !gpsp_rv32_srli(code, RV_FLAG_C, A2, 31))
      return false;
#else
    if (!gpsp_rv32_andi(code, A1, A1, 31))
      return false;
    exact = code->ptr;
    if (!gpsp_rv32_beq(code, A1, X0, 0) ||
        !gpsp_rv32_addi(code, RV_TMP0, X0, 32) ||
        !gpsp_rv32_sub(code, RV_TMP0, RV_TMP0, A1) ||
        !gpsp_rv32_sll(code, RV_TMP1, value, RV_TMP0) ||
        !gpsp_rv32_srl(code, A2, value, A1) ||
        !gpsp_rv32_or(code, A2, A2, RV_TMP1))
      return false;
    done = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_patch_branch(code, exact, code->ptr))
      return false;
    /* A positive multiple of 32 keeps the source value. */
    if (!gpsp_rv32_addi(code, A2, value, 0) ||
        !gpsp_rv32_patch_branch(code, done, code->ptr) ||
        !gpsp_rv32_srli(code, RV_FLAG_C, A2, 31))
      return false;
#endif
  }
  else
  {
    if (!gpsp_rv32_sltiu(code, RV_TMP0, A1, 32))
      return false;
    small = code->ptr;
    if (!gpsp_rv32_bne(code, RV_TMP0, X0, 0))
      return false;

    if (kind == 2) /* ASR >= 32 */
    {
      if ((carry && !gpsp_rv32_srli(code, RV_FLAG_C, value, 31)) ||
          !gpsp_rv32_srai(code, A2, value, 31))
        return false;
    }
    else
    {
      if ((carry && !gpsp_rv32_addi(code, RV_FLAG_C, X0, 0)) ||
          !gpsp_rv32_addi(code, A2, X0, 0))
        return false;
      if (carry)
      {
        exact = code->ptr;
        if (!gpsp_rv32_addi(code, RV_TMP0, A1, -32) ||
            !gpsp_rv32_bne(code, RV_TMP0, X0, 0) ||
            (kind == 0 && !gpsp_rv32_andi(code, RV_FLAG_C, value, 1)) ||
            (kind == 1 && !gpsp_rv32_srli(code, RV_FLAG_C, value, 31)) ||
            !gpsp_rv32_patch_branch(code, exact + 4, code->ptr))
          return false;
      }
    }
    done = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_patch_branch(code, small, code->ptr))
      return false;

    if (kind == 0) /* LSL 1..31 */
    {
#if GPSP_RV32_USE_ZBS
      /* bext reads the low five bits: 32 - n equals -n there. */
      if ((carry &&
           (!gpsp_rv32_sub(code, RV_TMP0, X0, A1) ||
            !gpsp_rv32_bext(code, RV_FLAG_C, value, RV_TMP0))) ||
#else
      if ((carry &&
           (!gpsp_rv32_addi(code, RV_TMP0, X0, 32) ||
            !gpsp_rv32_sub(code, RV_TMP0, RV_TMP0, A1) ||
            !gpsp_rv32_srl(code, RV_TMP1, value, RV_TMP0) ||
            !gpsp_rv32_andi(code, RV_FLAG_C, RV_TMP1, 1))) ||
#endif
          !gpsp_rv32_sll(code, A2, value, A1))
        return false;
    }
    else /* LSR/ASR 1..31 */
    {
#if GPSP_RV32_USE_ZBS
      if ((carry &&
           (!gpsp_rv32_addi(code, RV_TMP0, A1, -1) ||
            !gpsp_rv32_bext(code, RV_FLAG_C, value, RV_TMP0))) ||
#else
      if ((carry &&
           (!gpsp_rv32_addi(code, RV_TMP0, A1, -1) ||
            !gpsp_rv32_srl(code, RV_TMP1, value, RV_TMP0) ||
            !gpsp_rv32_andi(code, RV_FLAG_C, RV_TMP1, 1))) ||
#endif
          !(kind == 1 ? gpsp_rv32_srl(code, A2, value, A1) :
                          gpsp_rv32_sra(code, A2, value, A1)))
        return false;
    }
    if (!gpsp_rv32_patch_branch(code, done, code->ptr))
      return false;
  }

  return gpsp_rv32_patch_branch(code, zero, code->ptr) &&
      emit_nz(code, flags);
}

static bool emit_basic_alu(gpsp_rv32_code *code, uint16_t opcode,
    unsigned flags)
{
  unsigned rd = opcode & 7;
  unsigned rs = (opcode >> 3) & 7;
  unsigned operation = (opcode >> 6) & 15;
  unsigned dest = guest_reg_map[rd];
  unsigned source = guest_reg_map[rs];
  if (((operation == 8 && !(flags & (THUMB_FLAG_N | THUMB_FLAG_Z))) ||
       ((operation == 10 || operation == 11) && !flags)))
    return true;

  switch (operation)
  {
    case 0:  /* AND */
      return (rd == rs || gpsp_rv32_and(code, dest, dest, source)) &&
          emit_nz_reg(code, dest, flags);
    case 1:  /* EOR */
      return gpsp_rv32_xor(code, dest, dest, source) &&
          emit_nz_reg(code, dest, flags);
    case 12: /* ORR */
      return (rd == rs || gpsp_rv32_or(code, dest, dest, source)) &&
          emit_nz_reg(code, dest, flags);
    case 13: /* MUL */
      return gpsp_rv32_mul(code, dest, dest, source) &&
          emit_nz_reg(code, dest, flags);
    case 14: /* BIC */
      if (rd == rs)
        return gpsp_rv32_addi(code, dest, X0, 0) &&
            emit_nz_reg(code, dest, flags);
#if GPSP_RV32_USE_ZBB
      return gpsp_rv32_andn(code, dest, dest, source) &&
          emit_nz_reg(code, dest, flags);
#else
      return gpsp_rv32_xori(code, RV_TMP0, source, -1) &&
          gpsp_rv32_and(code, dest, dest, RV_TMP0) &&
          emit_nz_reg(code, dest, flags);
#endif
    case 15: /* MVN */
      return gpsp_rv32_xori(code, dest, source, -1) &&
          emit_nz_reg(code, dest, flags);
    case 8:  /* TST */
      return (rd == rs || gpsp_rv32_and(code, A2, dest, source)) &&
          emit_nz_reg(code, rd == rs ? dest : A2, flags);
    case 2:  /* LSL */
    case 3:  /* LSR */
    case 4:  /* ASR */
    case 7:  /* ROR */
#if GPSP_RV32_USE_ZBB
      if (operation == 7 && !(flags & THUMB_FLAG_C))
        return gpsp_rv32_ror(code, dest, dest, source) &&
            emit_nz_reg(code, dest, flags);
#endif
      return gpsp_rv32_addi(code, A1, source, 0) &&
          emit_shift_reg(code, operation == 7 ? 3 : operation - 2,
              flags, dest) &&
          emit_write(code, rd, A2);
    case 5:  /* ADC */
      if (!(flags & (THUMB_FLAG_C | THUMB_FLAG_V)))
        return gpsp_rv32_add(code, dest, dest, source) &&
            gpsp_rv32_add(code, dest, dest, RV_FLAG_C) &&
            emit_nz_reg(code, dest, flags);
      return emit_add_flags(code, true, flags, dest, source) &&
          emit_write(code, rd, A2);
    case 6:  /* SBC */
      if (!(flags & (THUMB_FLAG_C | THUMB_FLAG_V)))
        return gpsp_rv32_xori(code, RV_TMP1, RV_FLAG_C, 1) &&
            gpsp_rv32_sub(code, dest, dest, source) &&
            gpsp_rv32_sub(code, dest, dest, RV_TMP1) &&
            emit_nz_reg(code, dest, flags);
      return emit_sub_flags(code, true, flags, dest, source) &&
          emit_write(code, rd, A2);
    case 9:  /* NEG */
      if (!(flags & (THUMB_FLAG_C | THUMB_FLAG_V)))
        return gpsp_rv32_sub(code, dest, X0, source) &&
            emit_nz_reg(code, dest, flags);
      return emit_sub_flags(code, false, flags, X0, source) &&
          emit_write(code, rd, A2);
    case 10: /* CMP */
      return emit_sub_flags(code, false, flags, dest, source);
    case 11: /* CMN */
      return emit_add_flags(code, false, flags, dest, source);
    default:
      return false;
  }
}

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

bool gpsp_rv32_emit_thumb_condition(gpsp_rv32_code *code,
    unsigned condition)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end || condition >= 14)
    return false;

  uint32_t words[4];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  if (!emit_condition(&staged, condition))
    return false;
  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return false;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return true;
}

static bool emit_conditional_branch(gpsp_rv32_code *code,
    uint16_t opcode, uint32_t pc)
{
  unsigned condition = (opcode >> 8) & 15;
  int32_t offset = (int32_t)(opcode & 0xff);
  if (offset & 0x80)
    offset -= 0x100;

  if (!emit_condition(code, condition) ||
      !emit_constant(code, A0, pc + 2))
    return false;
  uint8_t *skip = code->ptr;
  if (!gpsp_rv32_beq(code, RV_TMP0, X0, 0) ||
      !emit_constant(code, A0, pc + 4 + (uint32_t)(offset * 2)) ||
      !gpsp_rv32_patch_branch(code, skip, code->ptr) ||
      !gpsp_rv32_addi(code, A1, X0, 1))
    return false;
  return true;
}

static bool emit_high_register(gpsp_rv32_code *code, uint16_t opcode,
    uint32_t pc, unsigned flags)
{
  unsigned operation = (opcode >> 8) & 3;
  unsigned rs = (opcode >> 3) & 15;
  unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
  if (operation == 1 && !flags)
    return true;
  if (operation == 1) /* CMP */
  {
    unsigned lhs = rd == 15 ? A0 : guest_reg_map[rd];
    unsigned rhs = rs == 15 ? A1 : guest_reg_map[rs];
    return (rd != 15 || emit_constant(code, A0, pc + 4)) &&
        (rs != 15 || emit_constant(code, A1, pc + 4)) &&
        emit_sub_flags(code, false, flags, lhs, rhs);
  }

  if (rd != 15)
  {
    unsigned dest = guest_reg_map[rd];
    if (operation == 0) /* ADD, no flags */
    {
      unsigned source = rs == 15 ? RV_TMP0 : guest_reg_map[rs];
      return (rs != 15 || emit_constant(code, RV_TMP0, pc + 4)) &&
          gpsp_rv32_add(code, dest, dest, source);
    }
    if (operation == 2) /* MOV, no flags */
      return rs == 15 ? emit_constant(code, dest, pc + 4) :
          (rd == rs || gpsp_rv32_addi(code, dest, guest_reg_map[rs], 0));
  }

  if (!emit_read(code, A1, rs, pc))
    return false;
  if (operation == 3) /* BX */
    return gpsp_rv32_andi(code, A0, A1, -2) &&
        gpsp_rv32_andi(code, A1, A1, 1);

  if (operation != 2 && !emit_read(code, A0, rd, pc))
    return false;
  if (operation == 0) /* ADD, no flags */
  {
    if (!gpsp_rv32_add(code, A2, A0, A1))
      return false;
  }
  else /* MOV, no flags */
  {
    if (!gpsp_rv32_addi(code, A2, A1, 0))
      return false;
  }

  return gpsp_rv32_andi(code, A0, A2, -2) &&
      gpsp_rv32_addi(code, A1, X0, 1);
}

static bool emit_instruction(gpsp_rv32_code *code, uint16_t opcode,
    uint32_t pc, unsigned flags)
{
  if ((opcode & 0xe000) == 0) /* shifts, three-bit ADD/SUB */
  {
    unsigned rd = opcode & 7;
    unsigned rs = (opcode >> 3) & 7;
    if ((opcode & 0x1800) != 0x1800)
      return emit_shift_imm(code, (opcode >> 11) & 3,
          (opcode >> 6) & 31, flags, guest_reg_map[rs],
          guest_reg_map[rd]);

    unsigned operand = (opcode >> 6) & 7;
    bool immediate = (opcode & 0x0400) != 0;
    bool subtract = (opcode & 0x0200) != 0;
    if (immediate)
      return emit_immediate_arithmetic(code, guest_reg_map[rd],
          guest_reg_map[rs], operand, subtract, true, flags);
    unsigned lhs = guest_reg_map[rs];
    unsigned rhs = guest_reg_map[operand];
    unsigned dest = guest_reg_map[rd];
    if (!(flags & (THUMB_FLAG_C | THUMB_FLAG_V)))
      return (subtract ? gpsp_rv32_sub(code, dest, lhs, rhs) :
                         gpsp_rv32_add(code, dest, lhs, rhs)) &&
          emit_nz_reg(code, dest, flags);
    return (subtract ? emit_sub_flags(code, false, flags, lhs, rhs) :
                        emit_add_flags(code, false, flags, lhs, rhs)) &&
        emit_write(code, rd, A2);
  }

  if ((opcode & 0xe000) == 0x2000) /* immediate MOV/CMP/ADD/SUB */
  {
    unsigned operation = (opcode >> 11) & 3;
    unsigned rd = (opcode >> 8) & 7;
    unsigned immediate = opcode & 0xff;
    if (operation == 0) /* MOV: N/Z only */
    {
      unsigned dest = guest_reg_map[rd];
      return gpsp_rv32_addi(code, dest, X0, immediate) &&
          (!(flags & THUMB_FLAG_N) ||
           gpsp_rv32_addi(code, RV_FLAG_N, X0, 0)) &&
          (!(flags & THUMB_FLAG_Z) ||
           gpsp_rv32_addi(code, RV_FLAG_Z, X0, immediate == 0));
    }
    return emit_immediate_arithmetic(code, guest_reg_map[rd],
        guest_reg_map[rd], immediate, operation != 2,
        operation != 1, flags);
  }

  if ((opcode & 0xfc00) == 0x4000)
    return emit_basic_alu(code, opcode, flags);
  if ((opcode & 0xfc00) == 0x4400)
    return emit_high_register(code, opcode, pc, flags);

  if ((opcode & 0xf000) == 0xa000) /* ADD Rd,PC/SP,#imm */
  {
    unsigned rd = (opcode >> 8) & 7;
    unsigned dest = guest_reg_map[rd];
    unsigned amount = (opcode & 0xff) * 4;
    if (opcode & 0x0800)
      return gpsp_rv32_addi(code, dest, guest_reg_map[13], amount);
    return emit_constant(code, dest, ((pc + 4) & ~3u) + amount);
  }

  if ((opcode & 0xff00) == 0xb000) /* ADD/SUB SP,#imm */
  {
    int amount = (opcode & 0x7f) * 4;
    if (opcode & 0x80)
      amount = -amount;
    return amount == 0 ||
        gpsp_rv32_addi(code, RV_GUEST_SP, RV_GUEST_SP, amount);
  }

  if ((opcode & 0xf000) == 0xd000) /* Bcc, except 0xDE/DF */
    return emit_conditional_branch(code, opcode, pc);

  if ((opcode & 0xf800) == 0xe000) /* B */
  {
    int32_t offset = opcode & 0x7ff;
    if (offset & 0x400)
      offset -= 0x800;
    return emit_constant(code, A0, pc + 4 + (uint32_t)(offset * 2)) &&
        gpsp_rv32_addi(code, A1, X0, 1);
  }

  if ((opcode & 0xf800) == 0xf000) /* BL first half */
  {
    /* The second half reads this value from guest LR. */
    int32_t offset = opcode & 0x7ff;
    if (offset & 0x400)
      offset -= 0x800;
    return emit_constant(code, RV_GUEST_LR, pc + 4 + (uint32_t)(offset * 4096));
  }

  if ((opcode & 0xf800) == 0xf800) /* BL second half */
  {
    return emit_constant(code, A2, (opcode & 0x7ff) * 2) &&
        gpsp_rv32_add(code, A0, RV_GUEST_LR, A2) &&
        emit_constant(code, RV_GUEST_LR, (pc + 2) | 1) &&
        gpsp_rv32_addi(code, A1, X0, 1);
  }

  return false;
}

bool gpsp_rv32_thumb_needs_helper(uint16_t opcode)
{
  if ((opcode & 0xfc00) == 0x4400)
    return ((opcode >> 8) & 3) == 3 && (opcode & 0x87);
  if ((opcode & 0xf000) == 0xd000)
    return ((opcode >> 8) & 15) >= 14;
  return !((opcode & 0xe000) == 0 ||
           (opcode & 0xe000) == 0x2000 ||
           (opcode & 0xfc00) == 0x4000 ||
           (opcode & 0xf000) == 0xa000 ||
           (opcode & 0xff00) == 0xb000 ||
           (opcode & 0xf800) == 0xe000 ||
           (opcode & 0xf800) == 0xf000 ||
           (opcode & 0xf800) == 0xf800);
}

gpsp_rv32_thumb_result gpsp_rv32_emit_thumb(gpsp_rv32_code *code,
    uint16_t opcode, uint32_t instruction_pc, uint8_t needed_flags)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_THUMB_BUFFER_FULL;
  if (instruction_pc & 1)
    return GPSP_RV32_THUMB_NEEDS_HELPER;
  if (gpsp_rv32_thumb_needs_helper(opcode))
    return GPSP_RV32_THUMB_NEEDS_HELPER;

  gpsp_rv32_thumb_result result = GPSP_RV32_THUMB_EMITTED;
  if ((opcode & 0xfc00) == 0x4400)
  {
    unsigned operation = (opcode >> 8) & 3;
    unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
    if (operation == 3 || ((operation == 0 || operation == 2) && rd == 15))
      result = GPSP_RV32_THUMB_EXIT;
  }
  else if ((opcode & 0xf000) == 0xd000)
  {
    result = GPSP_RV32_THUMB_EXIT;
  }
  else if ((opcode & 0xf800) == 0xe000 ||
           (opcode & 0xf800) == 0xf800)
    result = GPSP_RV32_THUMB_EXIT;
  uint32_t words[128];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  if (!emit_instruction(&staged, opcode, instruction_pc, needed_flags & 15))
    return GPSP_RV32_THUMB_BUFFER_FULL;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_THUMB_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return result;
}

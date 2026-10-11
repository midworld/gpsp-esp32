/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_alu.h"
#include "rv32_register_map.h"

#include <string.h>

typedef struct
{
  unsigned condition;
  unsigned operation;
  unsigned rn;
  unsigned rd;
  unsigned rm;
  unsigned rs;
  unsigned shift_type;
  unsigned shift_amount;
  bool immediate;
  bool shift_by_register;
  bool set_flags;
  bool rotated_immediate;
  uint32_t operand_value;
} decoded_arm_alu;

static bool decode_arm_alu(uint32_t opcode, uint32_t instruction_pc,
    decoded_arm_alu *decoded)
{
  if ((instruction_pc & 3) || (opcode & 0x0c000000))
    return false;

  decoded->condition = opcode >> 28;
  decoded->operation = (opcode >> 21) & 15;
  decoded->rn = (opcode >> 16) & 15;
  decoded->rd = (opcode >> 12) & 15;
  decoded->immediate = (opcode & 0x02000000) != 0;
  decoded->set_flags = (opcode & 0x00100000) != 0;
  decoded->rotated_immediate = false;

  if (decoded->operation >= 8 && decoded->operation <= 11)
  {
    /* TST/TEQ/CMP/CMN without S overlap PSR and obsolete TEQP forms. */
    if (!decoded->set_flags || decoded->rd != 0)
      return false;
  }
  else if (decoded->rd == 15 && decoded->set_flags)
  {
    /* S with Rd=PC restores CPSR and banked registers from SPSR. */
    return false;
  }

  if ((decoded->operation == 13 || decoded->operation == 15) &&
      decoded->rn != 0)
    return false;

  if (decoded->immediate)
  {
    unsigned rotation = ((opcode >> 8) & 15) * 2;
    uint32_t imm8 = opcode & 0xff;
    decoded->operand_value = rotation ?
        (imm8 >> rotation) | (imm8 << (32 - rotation)) : imm8;
    decoded->rotated_immediate = rotation != 0;
  }
  else
  {
    decoded->shift_by_register = (opcode & 0x10) != 0;
    if (decoded->shift_by_register && (opcode & 0x80))
      /* Bit 7 must be zero in a register-specified shift. */
      return false;
    decoded->rm = opcode & 15;
    decoded->shift_type = (opcode >> 5) & 3;
    if (decoded->shift_by_register)
    {
      decoded->rs = (opcode >> 8) & 15;
      /* ARM7TDMI defines the shift count register as other than R15. */
      if (decoded->rs == 15)
        return false;
    }
    else
      decoded->shift_amount = (opcode >> 7) & 31;
  }

  return true;
}

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

static bool emit_guest_read(gpsp_rv32_code *code, unsigned rd,
    unsigned guest_reg, uint32_t instruction_pc, unsigned pc_offset,
    bool live_pc)
{
  if (guest_reg == 15)
    return live_pc ? gpsp_rv32_addi(code, rd, RV_PC, pc_offset) :
        emit_constant(code, rd, instruction_pc + pc_offset);
  return gpsp_rv32_addi(code, rd, guest_reg_map[guest_reg], 0);
}

/* Use a mapped guest register in place; only PC needs a scratch copy. */
static bool guest_operand(gpsp_rv32_code *code, unsigned scratch,
    unsigned guest_reg, uint32_t instruction_pc, unsigned pc_offset,
    bool live_pc, unsigned *reg)
{
  *reg = guest_reg == 15 ? scratch : guest_reg_map[guest_reg];
  return guest_reg != 15 || emit_guest_read(code, scratch, guest_reg,
      instruction_pc, pc_offset, live_pc);
}

static bool emit_condition_skip(gpsp_rv32_code *code, unsigned condition,
    uint8_t *skip_sites[2], unsigned *skip_count)
{
  *skip_count = 0;
#define skip_if(op, left, right) do { \
  skip_sites[(*skip_count)++] = code->ptr; \
  return gpsp_rv32_##op(code, left, right, 0); \
} while (0)
/* A taken first branch bypasses the skip branch. */
#define keep_else_skip_if(keep_op, keep_l, keep_r, op, left, right) do { \
  uint8_t *keep = code->ptr; \
  if (!gpsp_rv32_##keep_op(code, keep_l, keep_r, 0)) \
    return false; \
  skip_sites[(*skip_count)++] = code->ptr; \
  return gpsp_rv32_##op(code, left, right, 0) && \
      gpsp_rv32_patch_branch(code, keep, code->ptr); \
} while (0)
  switch (condition)
  {
    case 0:  skip_if(beq, RV_FLAG_Z, X0); /* EQ */
    case 1:  skip_if(bne, RV_FLAG_Z, X0); /* NE */
    case 2:  skip_if(beq, RV_FLAG_C, X0); /* CS */
    case 3:  skip_if(bne, RV_FLAG_C, X0); /* CC */
    case 4:  skip_if(beq, RV_FLAG_N, X0); /* MI */
    case 5:  skip_if(bne, RV_FLAG_N, X0); /* PL */
    case 6:  skip_if(beq, RV_FLAG_V, X0); /* VS */
    case 7:  skip_if(bne, RV_FLAG_V, X0); /* VC */
    case 8: /* HI: skip when !C or Z. */
      skip_sites[(*skip_count)++] = code->ptr;
      if (!gpsp_rv32_beq(code, RV_FLAG_C, X0, 0))
        return false;
      skip_if(bne, RV_FLAG_Z, X0);
    case 9: /* LS: skip when C and !Z. */
      keep_else_skip_if(beq, RV_FLAG_C, X0, beq, RV_FLAG_Z, X0);
    case 10: skip_if(bne, RV_FLAG_N, RV_FLAG_V); /* GE */
    case 11: skip_if(beq, RV_FLAG_N, RV_FLAG_V); /* LT */
    case 12: /* GT: skip when Z or N != V. */
      skip_sites[(*skip_count)++] = code->ptr;
      if (!gpsp_rv32_bne(code, RV_FLAG_Z, X0, 0))
        return false;
      skip_if(bne, RV_FLAG_N, RV_FLAG_V);
    case 13: /* LE: skip when !Z and N == V. */
      keep_else_skip_if(bne, RV_FLAG_Z, X0, beq, RV_FLAG_N, RV_FLAG_V);
    default: return false;
  }
#undef skip_if
#undef keep_else_skip_if
}

static bool emit_carry_bit(gpsp_rv32_code *code, unsigned src,
    unsigned bit)
{
#if GPSP_RV32_USE_ZBS
  if (bit != 31)
    return gpsp_rv32_bexti(code, RV_FLAG_C, src, bit);
#endif
  return gpsp_rv32_srli(code, RV_FLAG_C, src, bit) &&
      (bit == 31 || gpsp_rv32_andi(code, RV_FLAG_C, RV_FLAG_C, 1));
}

/* Shift SRC into A1 and optionally C. SRC is never A1, RV_TMP0, RV_TMP1
   or RV_FLAG_C. */
static bool emit_immediate_shift(gpsp_rv32_code *code, unsigned src,
    unsigned kind, unsigned amount, bool update_c)
{
  switch (kind)
  {
    case 0: /* LSL #0 preserves C. */
      return (!update_c || !amount || emit_carry_bit(code, src, 32 - amount)) &&
          gpsp_rv32_slli(code, A1, src, amount);

    case 1: /* LSR #0 means LSR #32. */
      if (!amount)
        return (!update_c || emit_carry_bit(code, src, 31)) &&
            gpsp_rv32_addi(code, A1, X0, 0);
      return (!update_c || emit_carry_bit(code, src, amount - 1)) &&
          gpsp_rv32_srli(code, A1, src, amount);

    case 2: /* ASR #0 means ASR #32. */
      if (!amount)
        return (!update_c || emit_carry_bit(code, src, 31)) &&
            gpsp_rv32_srai(code, A1, src, 31);
      return (!update_c || emit_carry_bit(code, src, amount - 1)) &&
          gpsp_rv32_srai(code, A1, src, amount);

    case 3: /* ROR #0 is RRX through old C. */
      if (!amount)
        return gpsp_rv32_srli(code, A1, src, 1) &&
            gpsp_rv32_slli(code, RV_TMP1, RV_FLAG_C, 31) &&
            gpsp_rv32_or(code, A1, A1, RV_TMP1) &&
            (!update_c || emit_carry_bit(code, src, 0));
#if GPSP_RV32_USE_ZBB
      return gpsp_rv32_rori(code, A1, src, amount) &&
          (!update_c || gpsp_rv32_srli(code, RV_FLAG_C, A1, 31));
#else
      return (!update_c || emit_carry_bit(code, src, amount - 1)) &&
          gpsp_rv32_srli(code, A1, src, amount) &&
          gpsp_rv32_slli(code, RV_TMP1, src, 32 - amount) &&
          gpsp_rv32_or(code, A1, A1, RV_TMP1);
#endif
  }
  return false;
}

static bool emit_register_shift(gpsp_rv32_code *code, unsigned src,
    unsigned kind, bool update_c)
{
  uint8_t *zero_site = code->ptr;
  if (!gpsp_rv32_beq(code, RV_TMP0, X0, 0))
    return false;

  if (kind == 3) /* ROR, count modulo 32 after the zero-count test. */
  {
    if (!gpsp_rv32_andi(code, RV_TMP0, RV_TMP0, 31))
      return false;
    uint8_t *multiple_site = code->ptr;
    if (!gpsp_rv32_beq(code, RV_TMP0, X0, 0))
      return false;
#if GPSP_RV32_USE_ZBB
    if (!gpsp_rv32_ror(code, A1, src, RV_TMP0) ||
        (update_c && !gpsp_rv32_srli(code, RV_FLAG_C, A1, 31)))
      return false;
#else
    if (update_c &&
        (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -1) ||
         !gpsp_rv32_srl(code, RV_TMP1, src, RV_TMP1) ||
         !gpsp_rv32_andi(code, RV_FLAG_C, RV_TMP1, 1)))
      return false;
    if (!gpsp_rv32_srl(code, A1, src, RV_TMP0) ||
        !gpsp_rv32_addi(code, RV_TMP1, X0, 32) ||
        !gpsp_rv32_sub(code, RV_TMP1, RV_TMP1, RV_TMP0) ||
        !gpsp_rv32_sll(code, RV_TMP1, src, RV_TMP1) ||
        !gpsp_rv32_or(code, A1, A1, RV_TMP1))
      return false;
#endif
    uint8_t *done_site = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_patch_branch(code, multiple_site, code->ptr) ||
        (update_c && !emit_carry_bit(code, src, 31)))
      return false;
    return gpsp_rv32_patch_branch(code, zero_site, code->ptr) &&
        gpsp_rv32_patch_branch(code, done_site, code->ptr);
  }

  if (!gpsp_rv32_sltiu(code, RV_TMP1, RV_TMP0, 32))
    return false;
  uint8_t *small_site = code->ptr;
  if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0))
    return false;

  if (kind == 2) /* ASR by >=32 sign-fills. */
  {
    if ((update_c && !emit_carry_bit(code, src, 31)) ||
        !gpsp_rv32_srai(code, A1, src, 31))
      return false;
  }
  else /* LSL/LSR by >=32 is zero; only exactly 32 carries a source bit. */
  {
    if (!gpsp_rv32_addi(code, A1, X0, 0))
      return false;
    if (update_c)
    {
      if (!gpsp_rv32_addi(code, RV_TMP1, X0, 32))
        return false;
      uint8_t *exact_site = code->ptr;
      if (!gpsp_rv32_beq(code, RV_TMP0, RV_TMP1, 0) ||
          !gpsp_rv32_addi(code, RV_FLAG_C, X0, 0))
        return false;
      uint8_t *large_done_site = code->ptr;
      if (!gpsp_rv32_jal(code, X0, 0) ||
          !gpsp_rv32_patch_branch(code, exact_site, code->ptr) ||
          !emit_carry_bit(code, src, kind == 0 ? 0 : 31) ||
          !gpsp_rv32_patch_branch(code, large_done_site, code->ptr))
        return false;
    }
  }

  uint8_t *done_site = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0) ||
      !gpsp_rv32_patch_branch(code, small_site, code->ptr))
    return false;

  if (update_c)
  {
#if GPSP_RV32_USE_ZBS
    /* bext reads the low five bits: 32 - n equals -n there. */
    if (!(kind == 0 ? gpsp_rv32_sub(code, RV_TMP1, X0, RV_TMP0) :
                      gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -1)) ||
        !gpsp_rv32_bext(code, RV_FLAG_C, src, RV_TMP1))
      return false;
#else
    if (kind == 0)
    {
      if (!gpsp_rv32_addi(code, RV_TMP1, X0, 32) ||
          !gpsp_rv32_sub(code, RV_TMP1, RV_TMP1, RV_TMP0))
        return false;
    }
    else if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -1))
      return false;
    if (!gpsp_rv32_srl(code, RV_TMP1, src, RV_TMP1) ||
        !gpsp_rv32_andi(code, RV_FLAG_C, RV_TMP1, 1))
      return false;
#endif
  }

  bool shifted = kind == 0 ? gpsp_rv32_sll(code, A1, src, RV_TMP0) :
      kind == 1 ? gpsp_rv32_srl(code, A1, src, RV_TMP0) :
                  gpsp_rv32_sra(code, A1, src, RV_TMP0);
  return shifted && gpsp_rv32_patch_branch(code, zero_site, code->ptr) &&
      gpsp_rv32_patch_branch(code, done_site, code->ptr);
}

/* Report the register holding operand 2: a mapped Rm or A1. */
static bool emit_operand2(gpsp_rv32_code *code,
    const decoded_arm_alu *decoded, uint32_t instruction_pc, bool update_c,
    bool live_pc, unsigned *operand)
{
  *operand = A1;
  if (decoded->immediate)
    return emit_constant(code, A1, decoded->operand_value) &&
        (!update_c || !decoded->rotated_immediate ||
         gpsp_rv32_addi(code, RV_FLAG_C, X0, decoded->operand_value >> 31));

  if (!decoded->shift_by_register && decoded->shift_type == 0 &&
      decoded->shift_amount == 0)
    return guest_operand(code, A1, decoded->rm, instruction_pc, 8,
        live_pc, operand);

  /* Rm reads PC+12 for a register-specified shift; Rn reads PC+8. */
  unsigned src;
  if (!guest_operand(code, A2, decoded->rm, instruction_pc,
      decoded->shift_by_register ? 12 : 8, live_pc, &src))
    return false;

  if (!decoded->shift_by_register)
    return emit_immediate_shift(code, src, decoded->shift_type,
        decoded->shift_amount, update_c);

  return gpsp_rv32_addi(code, A1, src, 0) &&
      emit_guest_read(code, RV_TMP0, decoded->rs, instruction_pc, 8, live_pc) &&
      gpsp_rv32_andi(code, RV_TMP0, RV_TMP0, 255) &&
      emit_register_shift(code, src, decoded->shift_type, update_c);
}

static bool emit_nz(gpsp_rv32_code *code, unsigned result)
{
  return gpsp_rv32_srli(code, RV_FLAG_N, result, 31) &&
      gpsp_rv32_sltiu(code, RV_FLAG_Z, result, 1);
}

static bool emit_add_overflow(gpsp_rv32_code *code, unsigned left,
    unsigned right, unsigned result)
{
#if GPSP_RV32_USE_ZBB
  if (!gpsp_rv32_xnor(code, RV_TMP0, left, right))
    return false;
#else
  if (!gpsp_rv32_xor(code, RV_TMP0, left, right) ||
      !gpsp_rv32_xori(code, RV_TMP0, RV_TMP0, -1))
    return false;
#endif
  return gpsp_rv32_xor(code, RV_TMP1, left, result) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

static bool emit_sub_overflow(gpsp_rv32_code *code, unsigned left,
    unsigned right, unsigned result)
{
  return gpsp_rv32_xor(code, RV_TMP0, left, right) &&
      gpsp_rv32_xor(code, RV_TMP1, left, result) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

/* With flags, RESULT must differ from LEFT and RIGHT. */
static bool emit_add_result(gpsp_rv32_code *code, unsigned result,
    unsigned left, unsigned right, bool with_carry, bool set_flags)
{
  if (!gpsp_rv32_add(code, result, left, right))
    return false;
  if (with_carry)
  {
    if (set_flags && !gpsp_rv32_sltu(code, RV_TMP0, result, left))
      return false;
    if (!gpsp_rv32_add(code, result, result, RV_FLAG_C))
      return false;
    if (set_flags &&
        (!gpsp_rv32_sltu(code, RV_TMP1, result, RV_FLAG_C) ||
         !gpsp_rv32_or(code, RV_FLAG_C, RV_TMP0, RV_TMP1)))
      return false;
  }
  else if (set_flags && !gpsp_rv32_sltu(code, RV_FLAG_C, result, left))
    return false;

  return !set_flags || (emit_nz(code, result) &&
      emit_add_overflow(code, left, right, result));
}

/* With flags, RESULT must differ from LEFT and RIGHT. */
static bool emit_sub_result(gpsp_rv32_code *code, unsigned result,
    unsigned left, unsigned right, bool with_carry, bool set_flags)
{
  if (!gpsp_rv32_sub(code, result, left, right))
    return false;
  if (with_carry)
  {
    if (set_flags && !gpsp_rv32_sltu(code, RV_TMP0, left, right))
      return false;
    if (!gpsp_rv32_xori(code, RV_TMP1, RV_FLAG_C, 1))
      return false;
    if (set_flags &&
        (!gpsp_rv32_sltu(code, RV_FLAG_C, result, RV_TMP1) ||
         !gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_FLAG_C) ||
         !gpsp_rv32_xori(code, RV_FLAG_C, RV_TMP0, 1)))
      return false;
    if (!gpsp_rv32_sub(code, result, result, RV_TMP1))
      return false;
  }
  else if (set_flags &&
      (!gpsp_rv32_sltu(code, RV_FLAG_C, left, right) ||
       !gpsp_rv32_xori(code, RV_FLAG_C, RV_FLAG_C, 1)))
    return false;

  return !set_flags || (emit_nz(code, result) &&
      emit_sub_overflow(code, left, right, result));
}

static bool fits_imm12(int64_t value)
{
  return value >= -2048 && value <= 2047;
}

static bool emit_simple_unflagged(gpsp_rv32_code *code,
    const decoded_arm_alu *decoded)
{
  unsigned operation = decoded->operation;
  unsigned dst = guest_reg_map[decoded->rd];
  if (decoded->immediate)
  {
    uint32_t value = decoded->operand_value;
    int64_t signed_value = (int32_t)value;
    if (operation == 13)
      return emit_constant(code, dst, value);
    if (operation == 15)
      return emit_constant(code, dst, ~value);
    if (decoded->rn == 15)
      return false;

    unsigned left = guest_reg_map[decoded->rn];
    if (dst == left && value == 0 &&
        (operation == 1 || operation == 2 || operation == 4 ||
         operation == 12 || operation == 14))
      return true;
    switch (operation)
    {
      case 0:  return fits_imm12(signed_value) &&
                       gpsp_rv32_andi(code, dst, left, signed_value);
      case 1:  return fits_imm12(signed_value) &&
                       gpsp_rv32_xori(code, dst, left, signed_value);
      case 2:  return fits_imm12(-signed_value) &&
                       gpsp_rv32_addi(code, dst, left, -signed_value);
      case 4:  return fits_imm12(signed_value) &&
                       gpsp_rv32_addi(code, dst, left, signed_value);
      case 12: return fits_imm12(signed_value) &&
                       gpsp_rv32_ori(code, dst, left, signed_value);
      case 14: return fits_imm12((int32_t)~value) &&
                       gpsp_rv32_andi(code, dst, left, (int32_t)~value);
      default: return false;
    }
  }

#if GPSP_RV32_USE_ZBA
  if (operation == 4 && !decoded->shift_by_register &&
      decoded->shift_type == 0 && decoded->shift_amount >= 1 &&
      decoded->shift_amount <= 3 && decoded->rm != 15 && decoded->rn != 15)
    return gpsp_rv32_shadd(code, decoded->shift_amount, dst,
        guest_reg_map[decoded->rm], guest_reg_map[decoded->rn]);
#endif
  if (decoded->shift_by_register || decoded->shift_type ||
      decoded->shift_amount || decoded->rm == 15)
    return false;

  unsigned right = guest_reg_map[decoded->rm];
  if (operation == 13)
    return dst == right || gpsp_rv32_addi(code, dst, right, 0);
  if (operation == 15)
    return gpsp_rv32_xori(code, dst, right, -1);
  if (decoded->rn == 15)
    return false;

  unsigned left = guest_reg_map[decoded->rn];
  if (dst == left && left == right &&
      (operation == 0 || operation == 12))
    return true;
  switch (operation)
  {
    case 0:  return gpsp_rv32_and(code, dst, left, right);
    case 1:  return gpsp_rv32_xor(code, dst, left, right);
    case 2:  return gpsp_rv32_sub(code, dst, left, right);
    case 3:  return gpsp_rv32_sub(code, dst, right, left);
    case 4:  return gpsp_rv32_add(code, dst, left, right);
    case 12: return gpsp_rv32_or(code, dst, left, right);
    case 14:
#if GPSP_RV32_USE_ZBB
      return gpsp_rv32_andn(code, dst, left, right);
#else
      return gpsp_rv32_xori(code, RV_TMP0, right, -1) &&
          gpsp_rv32_and(code, dst, left, RV_TMP0);
#endif
    default: return false;
  }
}

static bool emit_arm_alu_body(gpsp_rv32_code *code,
    const decoded_arm_alu *decoded, uint32_t instruction_pc, bool live_pc)
{
  unsigned operation = decoded->operation;
  bool logical = operation == 0 || operation == 1 || operation == 8 ||
      operation == 9 || operation == 12 || operation == 13 ||
      operation == 14 || operation == 15;
  bool test = operation >= 8 && operation <= 11;
  unsigned result = test || decoded->rd == 15 ? A2 :
      guest_reg_map[decoded->rd];

  if (!decoded->set_flags && !test && decoded->rd != 15)
  {
    gpsp_rv32_code_mark start = gpsp_rv32_code_save(code);
    if (emit_simple_unflagged(code, decoded))
      return true;
    gpsp_rv32_code_restore(code, &start);
  }

  unsigned left = A0;
  unsigned right;
  if (operation != 13 && operation != 15 &&
      !guest_operand(code, A0, decoded->rn, instruction_pc, 8, live_pc,
          &left))
    return false;
  if (!emit_operand2(code, decoded, instruction_pc,
      logical && decoded->set_flags, live_pc, &right))
    return false;
  /* Flag math reads the operands after writing the result. */
  if (decoded->set_flags && !logical && left == result)
  {
    if (!gpsp_rv32_addi(code, A0, left, 0))
      return false;
    left = A0;
  }
  if (decoded->set_flags && !logical && right == result)
  {
    if (!gpsp_rv32_addi(code, A1, right, 0))
      return false;
    right = A1;
  }

  bool emitted;
  switch (operation)
  {
    case 0: case 8:  emitted = gpsp_rv32_and(code, result, left, right); break;
    case 1: case 9:  emitted = gpsp_rv32_xor(code, result, left, right); break;
    case 2: case 10: emitted = emit_sub_result(code, result, left, right,
                           false, decoded->set_flags); break;
    case 3:          emitted = emit_sub_result(code, result, right, left,
                           false, decoded->set_flags); break;
    case 4: case 11: emitted = emit_add_result(code, result, left, right,
                           false, decoded->set_flags); break;
    case 5:          emitted = emit_add_result(code, result, left, right,
                           true, decoded->set_flags); break;
    case 6:          emitted = emit_sub_result(code, result, left, right,
                           true, decoded->set_flags); break;
    case 7:          emitted = emit_sub_result(code, result, right, left,
                           true, decoded->set_flags); break;
    case 12:         emitted = gpsp_rv32_or(code, result, left, right); break;
    case 13:         emitted = result == right ||
                         gpsp_rv32_addi(code, result, right, 0); break;
    case 14:
#if GPSP_RV32_USE_ZBB
                     emitted = gpsp_rv32_andn(code, result, left, right);
                     break;
#else
                     emitted = gpsp_rv32_xori(code, RV_TMP0, right, -1) &&
                         gpsp_rv32_and(code, result, left, RV_TMP0); break;
#endif
    case 15:         emitted = gpsp_rv32_xori(code, result, right, -1); break;
    default:         return false;
  }
  if (!emitted || (logical && decoded->set_flags && !emit_nz(code, result)))
    return false;

  return true;
}

static gpsp_rv32_arm_alu_result emit_arm_alu(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc, bool live_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_ALU_BUFFER_FULL;

  decoded_arm_alu decoded = {0};
  if (!decode_arm_alu(opcode, instruction_pc, &decoded))
    return GPSP_RV32_ARM_ALU_UNSUPPORTED;

  /* ARM7TDMI cond=1111 never executes. The caller still accounts for time. */
  if (decoded.condition == 15)
    return GPSP_RV32_ARM_ALU_EMITTED;

  uint32_t words[128];
  gpsp_rv32_code staged;
  gpsp_rv32_code_child(&staged, code, words, sizeof(words));
  gpsp_rv32_code_mark empty = gpsp_rv32_code_save(&staged);
  uint8_t *skip_sites[2] = {NULL, NULL};
  unsigned skip_count = 0;
  bool exits = decoded.rd == 15;

  if (decoded.condition != 14)
  {
    if (!emit_condition_skip(&staged, decoded.condition, skip_sites,
        &skip_count))
      return GPSP_RV32_ARM_ALU_BUFFER_FULL;
  }

  uint8_t *body_start = staged.ptr;
  if (!emit_arm_alu_body(&staged, &decoded, instruction_pc, live_pc))
    return GPSP_RV32_ARM_ALU_BUFFER_FULL;

  if (!exits && staged.ptr == body_start)
    gpsp_rv32_code_restore(&staged, &empty);
  else if (exits)
  {
    if (!gpsp_rv32_andi(&staged, A0, A2, -4) ||
        !gpsp_rv32_addi(&staged, A1, X0, 1))
      return GPSP_RV32_ARM_ALU_BUFFER_FULL;
    if (skip_count)
    {
      uint8_t *done = staged.ptr;
      if (!gpsp_rv32_jal(&staged, X0, 0))
        return GPSP_RV32_ARM_ALU_BUFFER_FULL;
      for (unsigned i = 0; i < skip_count; ++i)
        if (!gpsp_rv32_patch_branch(&staged, skip_sites[i], staged.ptr))
          return GPSP_RV32_ARM_ALU_BUFFER_FULL;
      if (!emit_constant(&staged, A0, instruction_pc + 4) ||
          !gpsp_rv32_addi(&staged, A1, X0, 0) ||
          !gpsp_rv32_patch_branch(&staged, done, staged.ptr))
        return GPSP_RV32_ARM_ALU_BUFFER_FULL;
    }
  }
  else
    for (unsigned i = 0; i < skip_count; ++i)
      if (!gpsp_rv32_patch_branch(&staged, skip_sites[i], staged.ptr))
        return GPSP_RV32_ARM_ALU_BUFFER_FULL;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)code->end - (uintptr_t)code->ptr < size)
    return GPSP_RV32_ARM_ALU_BUFFER_FULL;

  memcpy(code->ptr, words, size);
  code->ptr += size;
  return exits ? GPSP_RV32_ARM_ALU_EXIT : GPSP_RV32_ARM_ALU_EMITTED;
}

gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc)
{
  return emit_arm_alu(code, opcode, instruction_pc, false);
}

gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu_block(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc)
{
  return emit_arm_alu(code, opcode, instruction_pc, true);
}

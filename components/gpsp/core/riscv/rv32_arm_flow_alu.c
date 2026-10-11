/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_flow_alu.h"

#include "rv32_register_map.h"

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

static uint32_t immediate_value(uint32_t opcode)
{
  unsigned rotation = ((opcode >> 8) & 15) * 2;
  uint32_t value = opcode & 0xff;
  return rotation ? (value >> rotation) | (value << (32 - rotation)) : value;
}

bool gpsp_rv32_arm_flow_direct_supported(uint32_t opcode)
{
  uint32_t body = opcode & 0x0fffffffu;
  unsigned operation = (body >> 21) & 15;
  unsigned rn = (body >> 16) & 15;
  unsigned rd = (body >> 12) & 15;
  bool set_flags = (body & 0x00100000u) != 0;
  bool test = operation == 8 || operation == 9 || operation == 10;
  if ((opcode >> 28) == 15 || (body & 0x0c000000u) != 0 ||
      (body & 0x00000090u) == 0x00000090u ||
      (operation != 0 && operation != 1 && operation != 2 &&
       operation != 4 && operation != 8 && operation != 9 &&
       operation != 10 &&
       operation != 12 && operation != 13) ||
      (test ? (!set_flags || rd != 0) : rd == 15) ||
      (operation != 13 && rn == 15) ||
      (operation == 13 && rn != 0))
    return false;
  if (!(body & 0x02000000u) &&
      ((body & 0x00000ff0u) != 0 || (body & 15) == 15))
    return false;
  return true;
}

static bool emit_nz(gpsp_rv32_code *code, unsigned result)
{
  return gpsp_rv32_srli(code, RV_FLAG_N, result, 31) &&
      gpsp_rv32_sltiu(code, RV_FLAG_Z, result, 1);
}

static bool emit_add_flags(gpsp_rv32_code *code, unsigned left,
    unsigned right)
{
  if (!emit_nz(code, A2) ||
      !gpsp_rv32_sltu(code, RV_FLAG_C, A2, left))
    return false;
#if GPSP_RV32_USE_ZBB
  if (!gpsp_rv32_xnor(code, RV_TMP0, left, right))
    return false;
#else
  if (!gpsp_rv32_xor(code, RV_TMP0, left, right) ||
      !gpsp_rv32_xori(code, RV_TMP0, RV_TMP0, -1))
    return false;
#endif
  return gpsp_rv32_xor(code, RV_TMP1, left, A2) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

static bool emit_sub_flags(gpsp_rv32_code *code, unsigned left,
    unsigned right)
{
  return emit_nz(code, A2) &&
      gpsp_rv32_sltu(code, RV_FLAG_C, left, right) &&
      gpsp_rv32_xori(code, RV_FLAG_C, RV_FLAG_C, 1) &&
      gpsp_rv32_xor(code, RV_TMP0, left, right) &&
      gpsp_rv32_xor(code, RV_TMP1, left, A2) &&
      gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_srli(code, RV_FLAG_V, RV_TMP0, 31);
}

static bool emit_binary(gpsp_rv32_code *code, unsigned operation,
    unsigned dest, unsigned left, unsigned right)
{
  switch (operation)
  {
    case 0: case 8:
      return gpsp_rv32_and(code, dest, left, right);
    case 1: case 9:
      return gpsp_rv32_xor(code, dest, left, right);
    case 2: case 10:
      return gpsp_rv32_sub(code, dest, left, right);
    case 4:
      return gpsp_rv32_add(code, dest, left, right);
    case 12:
      return gpsp_rv32_or(code, dest, left, right);
    default:
      return false;
  }
}

/* A dynamic op takes its operand from A1 as a register would. */
static bool emit_direct(gpsp_rv32_code *code, uint32_t opcode, bool dynamic)
{
  unsigned operation = (opcode >> 21) & 15;
  unsigned rn = (opcode >> 16) & 15;
  unsigned rd = (opcode >> 12) & 15;
  bool set_flags = (opcode & 0x00100000u) != 0;
  bool immediate = (opcode & 0x02000000u) != 0 && !dynamic;
  unsigned left = operation == 13 ? X0 : guest_reg_map[rn];
  unsigned right = immediate || dynamic ? A1 : guest_reg_map[opcode & 15];
  uint32_t value = immediate ? immediate_value(opcode) : 0;
  unsigned dest = operation == 8 || operation == 9 ||
      operation == 10 ||
      ((operation == 2 || operation == 4) && set_flags) ?
      A2 : guest_reg_map[rd];

  bool logical = operation == 0 || operation == 1 ||
      operation == 8 || operation == 9 || operation == 12;
  bool small_immediate = immediate && (!set_flags || logical) &&
      ((operation == 4 || logical) ?
       ((int32_t)value >= -2048 && (int32_t)value <= 2047) :
       operation == 2 && value <= 2048);
  if (immediate && operation != 13 && !small_immediate &&
      !emit_constant(code, A1, value))
    return false;

  bool emitted = true;
  if (operation == 13)
    emitted = immediate ? emit_constant(code, dest, value) :
        (dest == right || gpsp_rv32_addi(code, dest, right, 0));
  else if (small_immediate && operation == 4)
    emitted = gpsp_rv32_addi(code, dest, left, (int32_t)value);
  else if (small_immediate && operation == 2)
    emitted = gpsp_rv32_addi(code, dest, left, -(int)value);
  else if (small_immediate && (operation == 0 || operation == 8))
    emitted = gpsp_rv32_andi(code, dest, left, (int32_t)value);
  else if (small_immediate && (operation == 1 || operation == 9))
    emitted = gpsp_rv32_xori(code, dest, left, (int32_t)value);
  else if (small_immediate && operation == 12)
    emitted = gpsp_rv32_ori(code, dest, left, (int32_t)value);
  else
    emitted = emit_binary(code, operation, dest, left, right);
  if (!emitted || !set_flags)
    return emitted;

  if (operation == 2 || operation == 10)
  {
    if (!emit_sub_flags(code, left, right))
      return false;
  }
  else if (operation == 4)
  {
    if (!emit_add_flags(code, left, right))
      return false;
  }
  else if (!emit_nz(code, dest))
    return false;

  if (immediate && ((opcode >> 8) & 15) != 0 &&
      operation != 2 && operation != 4 && operation != 10 &&
      !gpsp_rv32_addi(code, RV_FLAG_C, X0, value >> 31))
    return false;
  return (operation == 2 || operation == 4) ?
      gpsp_rv32_addi(code, guest_reg_map[rd], A2, 0) : true;
}

bool gpsp_rv32_arm_flow_emit_direct(gpsp_rv32_code *code, uint32_t opcode)
{
  return code && gpsp_rv32_arm_flow_direct_supported(opcode) &&
      emit_direct(code, opcode, false);
}

bool gpsp_rv32_arm_flow_dynamic_supported(uint32_t opcode)
{
  return (opcode >> 28) == 14 && (opcode & 0x02000000u) &&
      !(opcode & 0x00000f00u) && gpsp_rv32_arm_flow_direct_supported(opcode);
}

bool gpsp_rv32_arm_flow_emit_dynamic(gpsp_rv32_code *code, uint32_t opcode,
    const uint8_t *immediate)
{
  uintptr_t address = (uintptr_t)immediate;
  if (!code || !gpsp_rv32_arm_flow_dynamic_supported(opcode) ||
      address > UINT32_MAX)
    return false;
  int low = (int)(address & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = (((uint32_t)address + 0x800u) >> 12) & 0xfffff;
  return gpsp_rv32_lui(code, A1, upper) &&
      gpsp_rv32_lbu(code, A1, A1, low) &&
      emit_direct(code, opcode, true);
}

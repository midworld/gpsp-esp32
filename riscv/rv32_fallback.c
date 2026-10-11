/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_fallback.h"
#include "rv32_register_map.h"

#include <string.h>

static bool emit_address(gpsp_rv32_code *code, unsigned rd, uint32_t address)
{
  int low = (int)(address & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((address + 0x800u) >> 12) & 0xfffff;
  if (!upper)
    return gpsp_rv32_addi(code, rd, X0, low);
  return gpsp_rv32_lui(code, rd, upper) &&
      (!low || gpsp_rv32_addi(code, rd, rd, low));
}

gpsp_rv32_fallback_result gpsp_rv32_emit_interpreter_step(
    gpsp_rv32_code *code, uintptr_t step_helper, uintptr_t helper_bridge)
{
  if (!code || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) || code->ptr > code->end ||
      !step_helper || !helper_bridge ||
      step_helper > UINT32_MAX || helper_bridge > UINT32_MAX)
    return GPSP_RV32_FALLBACK_INVALID;

  uint32_t staged[16];
  gpsp_rv32_code temp;
  gpsp_rv32_code_child(&temp, code, staged, sizeof(staged));

  /* a0 = step callback, a1/a2 = unused callback arguments. The private
   * bridge returns with ra set to the stub's block-exit continuation. */
  if (!emit_address(&temp, A0, (uint32_t)step_helper) ||
      !gpsp_rv32_addi(&temp, A1, X0, 0) ||
      !gpsp_rv32_addi(&temp, A2, X0, 0) ||
      !emit_address(&temp, RV_TMP1, (uint32_t)helper_bridge) ||
      !gpsp_rv32_jalr(&temp, RA, RV_TMP1, 0) ||
      /* The step runner branches to this ret by its length. */
      !gpsp_rv32_fixed_begin(&temp) ||
      !gpsp_rv32_jalr(&temp, X0, RA, 0) ||
      !gpsp_rv32_fixed_end(&temp))
    return GPSP_RV32_FALLBACK_INVALID;

  size_t size = (size_t)(temp.ptr - temp.start);
  if ((size_t)(code->end - code->ptr) < size)
    return GPSP_RV32_FALLBACK_BUFFER_FULL;
  memcpy(code->ptr, staged, size);
  code->ptr += size;
  return GPSP_RV32_FALLBACK_EMITTED;
}

gpsp_rv32_fallback_result gpsp_rv32_emit_interpreter_step_stub(
    gpsp_rv32_code *code, uintptr_t stub_address)
{
  if (!code || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) || code->ptr > code->end ||
      !stub_address || (stub_address & 3) || stub_address > UINT32_MAX)
    return GPSP_RV32_FALLBACK_INVALID;

  uint32_t words[2];
  gpsp_rv32_code temp;
  gpsp_rv32_code_child(&temp, code, words, sizeof(words));
  uint32_t address = (uint32_t)stub_address;
  int low = (int)(address & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((address + 0x800u) >> 12) & 0xfffff;
  if (!gpsp_rv32_lui(&temp, RV_TMP1, upper) ||
      !gpsp_rv32_jalr(&temp, X0, RV_TMP1, low))
    return GPSP_RV32_FALLBACK_INVALID;

  size_t size = (size_t)(temp.ptr - temp.start);
  if ((size_t)(code->end - code->ptr) < size)
    return GPSP_RV32_FALLBACK_BUFFER_FULL;
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return GPSP_RV32_FALLBACK_EMITTED;
}

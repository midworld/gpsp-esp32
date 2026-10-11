/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_ALU_H
#define GPSP_RV32_THUMB_ALU_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_THUMB_EMITTED,
  GPSP_RV32_THUMB_EXIT,
  GPSP_RV32_THUMB_NEEDS_HELPER,
  GPSP_RV32_THUMB_BUFFER_FULL,
} gpsp_rv32_thumb_result;

/* Match the opcode-only helper boundary used by the Thumb block scanner. */
bool gpsp_rv32_thumb_needs_helper(uint16_t opcode);

/* Evaluate a branch condition into RV_TMP0. */
bool gpsp_rv32_emit_thumb_condition(gpsp_rv32_code *code,
    unsigned condition);

/* Emit one Thumb instruction. EMITTED needs a PC+2 update by the caller.
 * EXIT returns the raw next PC in a0 and next Thumb bit in a1. A standalone
 * BL second half may return an odd PC, matching gpSP.
 * The caller updates PC, CPSR, cycles, and cache state. a0-a2 and
 * RV_TMP0/RV_TMP1 are scratch. needed_flags uses bit 3=N, 2=Z, 1=C, 0=V;
 * other flags are preserved. Failure leaves the output unchanged. */
gpsp_rv32_thumb_result gpsp_rv32_emit_thumb(gpsp_rv32_code *code,
    uint16_t opcode, uint32_t instruction_pc, uint8_t needed_flags);

#endif

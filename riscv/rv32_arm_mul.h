/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_MUL_H
#define GPSP_RV32_ARM_MUL_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_ARM_MUL_EMITTED,
  GPSP_RV32_ARM_MUL_UNSUPPORTED,
  GPSP_RV32_ARM_MUL_BUFFER_FULL,
} gpsp_rv32_arm_mul_result;

/* Emit MUL/MLA/UMULL/UMLAL/SMULL/SMLAL with the fixed guest-register map.
 * The caller updates PC and cycles, appends the block exit, and publishes
 * code only after cache synchronization. A failed call changes no code bytes
 * or cursor position. ARM condition false and cond=15 emit no guest effects. */
gpsp_rv32_arm_mul_result gpsp_rv32_emit_arm_mul(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc);

#endif

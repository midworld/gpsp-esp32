/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_BRANCH_H
#define GPSP_RV32_ARM_BRANCH_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_ARM_BRANCH_EXIT,
  GPSP_RV32_ARM_BRANCH_NEEDS_HELPER,
  GPSP_RV32_ARM_BRANCH_BUFFER_FULL,
} gpsp_rv32_arm_branch_result;

typedef struct
{
  uint8_t *site[2];
  unsigned count;
} gpsp_rv32_arm_condition_skip;

/* Branch to the fallthrough path when NZCV fails the ARM condition. */
bool gpsp_rv32_emit_arm_condition_skip(gpsp_rv32_code *code,
    unsigned condition, gpsp_rv32_arm_condition_skip *skip);
bool gpsp_rv32_patch_arm_condition_skip(gpsp_rv32_code *code,
    const gpsp_rv32_arm_condition_skip *skip, uint8_t *target);

/* Emit ARM B, BL or BX. EXIT returns the next PC in a0 and Thumb state in a1;
 * the step runner handles cycles and dispatch. BX r15 needs the interpreter.
 * Failure leaves the output bytes and cursor unchanged. */
gpsp_rv32_arm_branch_result gpsp_rv32_emit_arm_branch(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc);

/* Emit only the taken target and link value; caller handles the condition. */
gpsp_rv32_arm_branch_result gpsp_rv32_emit_arm_branch_taken(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc);

#endif

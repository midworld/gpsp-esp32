/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_ALU_H
#define GPSP_RV32_ARM_ALU_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_ARM_ALU_EMITTED,
  GPSP_RV32_ARM_ALU_EXIT,
  GPSP_RV32_ARM_ALU_UNSUPPORTED,
  GPSP_RV32_ARM_ALU_BUFFER_FULL,
} gpsp_rv32_arm_alu_result;

/* Emit one ARM data-processing instruction. EXIT writes the next PC to a0
 * and a taken predicate to a1. PSR-restoring forms use a helper.
 * The caller updates PC and cycles, then syncs published code.
 * Failure leaves the output unchanged. */
gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc);

/* The block path keeps the current guest PC in RV_PC. */
gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu_block(gpsp_rv32_code *code,
    uint32_t opcode, uint32_t instruction_pc);

#endif

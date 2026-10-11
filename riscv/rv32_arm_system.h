/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_SYSTEM_H
#define GPSP_RV32_ARM_SYSTEM_H

#include "rv32_codegen.h"
#include "rv32_stub.h"

typedef enum
{
  GPSP_RV32_ARM_SYSTEM_EMITTED,
  GPSP_RV32_ARM_SYSTEM_EXIT,
  GPSP_RV32_ARM_SYSTEM_UNSUPPORTED,
  GPSP_RV32_ARM_SYSTEM_BUFFER_FULL,
} gpsp_rv32_arm_system_result;

/* MRS, SPSR MSR, flags-only CPSR MSR and Div/DivArm continue in the block.
 * Other system operations finish their PC/cycles in a helper and return. */
gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc);

/* Thumb Div/DivArm continue in the block; other SWIs enter the BIOS. */
gpsp_rv32_arm_system_result gpsp_rv32_emit_thumb_swi(
    gpsp_rv32_code *code, uint16_t opcode, uint32_t instruction_pc);

/* The block dispatcher drains SWP write alerts after the generated return. */
uint32_t gpsp_rv32_arm_system_take_alert(void);

#endif

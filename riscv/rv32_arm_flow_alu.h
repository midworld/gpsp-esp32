/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_FLOW_ALU_H
#define GPSP_RV32_ARM_FLOW_ALU_H

#include "rv32_codegen.h"

/* Common ALU forms use mapped guest registers without a step wrapper. */
bool gpsp_rv32_arm_flow_direct_supported(uint32_t opcode);
bool gpsp_rv32_arm_flow_emit_direct(gpsp_rv32_code *code, uint32_t opcode);

/* Direct ALU ops with an unrotated imm8 read it from guest memory
   at run time. */
bool gpsp_rv32_arm_flow_dynamic_supported(uint32_t opcode);
bool gpsp_rv32_arm_flow_emit_dynamic(gpsp_rv32_code *code, uint32_t opcode,
    const uint8_t *immediate);

#endif

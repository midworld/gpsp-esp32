/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_FALLBACK_H
#define GPSP_RV32_FALLBACK_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_FALLBACK_EMITTED,
  GPSP_RV32_FALLBACK_INVALID,
  GPSP_RV32_FALLBACK_BUFFER_FULL,
} gpsp_rv32_fallback_result;

/* Emit a complete one-instruction block for either ARM or Thumb state.
 * The helper executes the instruction currently at reg[PC] and requests a
 * block exit. The RV32 stub synchronizes all mapped registers, CPSR and
 * cycles around the helper call. On return, this block returns to the stub.
 *
 * Both addresses are 32-bit code addresses; pass the addresses of
 * gpsp_rv32_interpret_one_helper and gpsp_rv32_call_helper.
 * This is an interpreter fallback, not native translation of the opcode.
 * All failure results leave the buffer and cursor unchanged. */
gpsp_rv32_fallback_result gpsp_rv32_emit_interpreter_step(
    gpsp_rv32_code *code, uintptr_t step_helper, uintptr_t helper_bridge);

/* Tail-jump to the shared interpreter stub. The target is a 32-bit aligned
 * address. The stub and bridge return directly to run_block's exit. */
gpsp_rv32_fallback_result gpsp_rv32_emit_interpreter_step_stub(
    gpsp_rv32_code *code, uintptr_t stub_address);

#endif

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_RV32_THUMB_IR_EMIT_H
#define GPSP_RV32_THUMB_IR_EMIT_H

#include "rv32_codegen.h"
#include "rv32_thumb_ir.h"

typedef struct
{
  uint32_t sequential_wait_address;
  uint32_t nonsequential_wait_address;
  uint32_t interpreter_stub_address;
  uint32_t idle_loop_target_pc;
  uint8_t entry_mask;
} gpsp_rv32_thumb_ir_environment;

typedef struct
{
  uint16_t entry_offset[4];
  uint16_t exit_link_offset;
  uint16_t mov1_offset;
  uint16_t mov2_offset;
  uint16_t cold_offset;
  uint16_t code_bytes;
  uint16_t staging_bytes;
} gpsp_rv32_thumb_ir_layout;

/* Emit the first forward TST/BEQ/MOVS/MOVS region. Other shapes return false.
 * Entry offsets are relative to the emitted code, with UINT16_MAX for omitted
 * side entries. The caller owns the code cache, link, and entry publication. */
bool gpsp_rv32_thumb_ir_emit(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_ir_environment *environment,
    gpsp_rv32_code *output, gpsp_rv32_thumb_ir_layout *layout);

/* Append a side entry while this block still occupies the code-cache tail. */
bool gpsp_rv32_thumb_ir_append_entry(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_ir_environment *environment, unsigned entry,
    gpsp_rv32_code *output, gpsp_rv32_thumb_ir_layout *layout);

#endif

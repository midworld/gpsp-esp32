/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_RV32_THUMB_FLOW_EMIT_H
#define GPSP_RV32_THUMB_FLOW_EMIT_H

#include "rv32_codegen.h"
#include "rv32_thumb_ir.h"

typedef struct
{
  uint32_t sequential_wait_address;
  uint32_t nonsequential_wait_address;
  uint32_t interpreter_stub_address;
  uint32_t idle_loop_target_pc;
} gpsp_rv32_thumb_flow_environment;

typedef struct
{
  uint16_t entry_offset[GPSP_RV32_THUMB_IR_MAX_OPS];
  uint16_t exit_link_offset;
  uint16_t code_bytes;
  uint8_t block_count;
} gpsp_rv32_thumb_flow_layout;

typedef struct
{
  uint16_t link_offset[2];
  uint8_t link_target[2];
  uint8_t link_count;
  uint16_t code_bytes;
} gpsp_rv32_thumb_flow_side_layout;

/* Emit forward Thumb basic blocks. The caller publishes entries and links. */
bool gpsp_rv32_thumb_flow_emit(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment,
    gpsp_rv32_code *output, gpsp_rv32_thumb_flow_layout *layout);

/* The bit for join must name an entry published before this side block. */
bool gpsp_rv32_thumb_flow_emit_side(const gpsp_rv32_thumb_ir *ir,
    const gpsp_rv32_thumb_flow_environment *environment, unsigned entry,
    unsigned join, uint64_t published_mask,
    const gpsp_rv32_thumb_flow_layout *main_layout,
    gpsp_rv32_code *output, gpsp_rv32_thumb_flow_side_layout *layout);

#endif

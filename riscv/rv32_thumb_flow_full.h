/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_FLOW_FULL_H
#define GPSP_RV32_THUMB_FLOW_FULL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPSP_RV32_THUMB_FLOW_MAX_OPS 32
/* Set in side_total_cost when a region holds RAM stub sites. */
#define GPSP_RV32_THUMB_FLOW_MEMORY_HEAD 0x80

typedef enum
{
  GPSP_RV32_THUMB_FLOW_ALU,
  GPSP_RV32_THUMB_FLOW_SINGLE,
  GPSP_RV32_THUMB_FLOW_BCC,
  GPSP_RV32_THUMB_FLOW_B,
  GPSP_RV32_THUMB_FLOW_MEMORY,
  GPSP_RV32_THUMB_FLOW_MULTI,
  GPSP_RV32_THUMB_FLOW_DYNAMIC,
  GPSP_RV32_THUMB_FLOW_SYSTEM,
  GPSP_RV32_THUMB_FLOW_HELPER,
} gpsp_rv32_thumb_flow_kind;

typedef struct
{
  uint32_t literal_value;
  uint16_t opcode;
  uint16_t flag_data;
  uint8_t kind;
  /* Flags live at a direct branch target outside the chunk. */
  uint8_t target_live;
  uint8_t literal_constant : 1;
  uint8_t entry_required : 1;
} gpsp_rv32_thumb_flow_op;

typedef struct
{
  uint32_t pc;
  uint32_t interpreter_stub_address;
  uint8_t count;
  bool rom_region;
  /* Flags live after the last op when execution falls through. */
  uint8_t fallthrough_live;
  gpsp_rv32_thumb_flow_op ops[GPSP_RV32_THUMB_FLOW_MAX_OPS];
} gpsp_rv32_thumb_flow_full_ir;

typedef struct
{
  uint16_t entry_offset[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint16_t body_offset[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint16_t link_offset[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint32_t link_target[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint32_t unguarded_mask;
  uint8_t side_prefix[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint8_t side_remaining[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint8_t side_prefix_cost[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint8_t side_total_cost[GPSP_RV32_THUMB_FLOW_MAX_OPS];
  uint16_t code_bytes;
  uint8_t guest_ops;
  uint8_t link_count;
  uint8_t helper_ops;
  uint8_t native_ops;
  uint8_t cold_stubs;
  uint8_t region_count;
  uint8_t entry_count;
  uint8_t fused_branches;
  bool ends_block;
} gpsp_rv32_thumb_flow_full_layout;

/* Flags live before immutable code; unknown control flow keeps all live. */
uint8_t gpsp_rv32_thumb_flow_live_flags(const uint16_t *opcodes,
    size_t count);

/* Direct branch target, or UINT32_MAX when the op is not B/Bcc. */
uint32_t gpsp_rv32_thumb_flow_branch_target(
    const gpsp_rv32_thumb_flow_full_ir *ir, unsigned index);

/* Decode every scanned Thumb opcode; callers mark cross-chunk entry targets. */
bool gpsp_rv32_thumb_flow_full_decode(uint32_t pc,
    const uint16_t *opcodes, size_t count,
    gpsp_rv32_thumb_flow_full_ir *out);

/* Emit ALU regions and individual effectful instructions. */
bool gpsp_rv32_thumb_flow_full_emit(
    const gpsp_rv32_thumb_flow_full_ir *ir, uint8_t **translation_ptr,
    uint8_t *translation_limit,
    gpsp_rv32_thumb_flow_full_layout *layout);

#endif

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_RV32_THUMB_IR_H
#define GPSP_RV32_THUMB_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPSP_RV32_THUMB_IR_MAX_OPS 32

enum
{
  GPSP_RV32_THUMB_FLAG_N = 8,
  GPSP_RV32_THUMB_FLAG_Z = 4,
  GPSP_RV32_THUMB_FLAG_C = 2,
  GPSP_RV32_THUMB_FLAG_V = 1,
  GPSP_RV32_THUMB_FLAG_ALL = 15,
};

typedef enum
{
  GPSP_RV32_THUMB_IR_TST,
  GPSP_RV32_THUMB_IR_BEQ,
  GPSP_RV32_THUMB_IR_MOV_IMM,
} gpsp_rv32_thumb_ir_kind;

typedef struct
{
  uint16_t opcode;
  uint8_t kind;
  uint8_t rd;
  uint8_t rs;
  uint8_t imm;
  uint8_t fallthrough;
  uint8_t taken;
  uint8_t flags_read;
  uint8_t flags_write;
  uint8_t flags_live_in;
  uint8_t flags_live_out;
} gpsp_rv32_thumb_node;

typedef struct
{
  uint32_t pc;
  uint8_t count;
  gpsp_rv32_thumb_node nodes[GPSP_RV32_THUMB_IR_MAX_OPS];
} gpsp_rv32_thumb_ir;

/* Successor index count denotes block exit. Only forward BEQ is accepted. */
bool gpsp_rv32_thumb_ir_decode(uint32_t pc, const uint16_t *opcodes,
    size_t count, gpsp_rv32_thumb_ir *out);

#endif

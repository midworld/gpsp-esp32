/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_MEMORY_H
#define GPSP_RV32_THUMB_MEMORY_H

#include <stdbool.h>

#include "rv32_stub.h"

typedef enum
{
  GPSP_RV32_MEM_LOAD_U8,
  GPSP_RV32_MEM_LOAD_U16,
  GPSP_RV32_MEM_LOAD_U32,
  GPSP_RV32_MEM_LOAD_S8,
  GPSP_RV32_MEM_LOAD_S16,
  GPSP_RV32_MEM_STORE_U8,
  GPSP_RV32_MEM_STORE_U16,
  GPSP_RV32_MEM_STORE_U32,
  GPSP_RV32_MEM_KIND_COUNT
} gpsp_rv32_memory_kind;

extern const gpsp_rv32_helper_fn
    gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT];

/* Consume alerts from the last translated memory access. */
uint32_t gpsp_rv32_take_memory_alert(void);
void gpsp_rv32_add_memory_alert(uint32_t alert);

/* True if the aligned RAM store overlaps code tags. */
bool gpsp_rv32_store_hits_code(uint32_t address, unsigned bytes);
/* True if the store changes tagged RAM bytes that need an SMC exit.
   Call before the store. */
bool gpsp_rv32_store_changes_code(uint32_t address, unsigned bytes,
    uint32_t value);
/* True if every live unit accepts this change without invalidation. */
bool gpsp_rv32_code_write_absorbed(uint32_t address, unsigned bytes,
    uint32_t before, uint32_t after);

/* Complete a Thumb block transfer when direct RAM access is unavailable. */
uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
    uint32_t opcode, uint32_t unused);

#endif

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_BLOCK_LINK_H
#define GPSP_RV32_BLOCK_LINK_H

#include "rv32_codegen.h"

/* An unlinked slot returns to the block runner. A linked slot jumps to
 * another block without changing ra or the guest register map. */
#define GPSP_RV32_LINK_SLOT_SIZE 8

/* Pads to a four-byte boundary first; *site is the slot after padding. */
bool gpsp_rv32_emit_link_slot(gpsp_rv32_code *code, uint8_t **site);

/* The code span covers all emitted bytes, including older blocks. Publishing
 * the patched instruction range is the caller's responsibility. */
bool gpsp_rv32_patch_link_slot(gpsp_rv32_code *code, uint8_t *site,
    uintptr_t target);

/* Restores a linked or unlinked slot to the unlinked form. Publishing the
 * instruction range is the caller's responsibility. */
bool gpsp_rv32_unlink_link_slot(uint8_t *site);

#endif

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_FLOW_TIMING_H
#define GPSP_RV32_FLOW_TIMING_H

#include "rv32_codegen.h"

/* A guard returns the current native PC when the guest budget is exhausted. */
bool gpsp_rv32_flow_guard(gpsp_rv32_code *code);

/* Charge one fetch wait per instruction. The caller keeps the PC current. */
bool gpsp_rv32_flow_charge_seq(gpsp_rv32_code *code,
    uint32_t next_pc, unsigned count, bool thumb);
unsigned gpsp_rv32_flow_fetch_bound(uint32_t next_pc, bool thumb);
bool gpsp_rv32_flow_literal_constant(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, uint32_t value, bool guard_entry);
bool gpsp_rv32_flow_literal_body(gpsp_rv32_code *code,
    uint16_t opcode, uint32_t value);

/* Guard, check the full fetch cost, and debit it on the fast path.
 * Patch cold_site to a one-instruction interpreter stub after emission. */
bool gpsp_rv32_flow_preflight_seq(gpsp_rv32_code *code,
    uint32_t next_pc, unsigned count, bool thumb, uint8_t **cold_site);

/* Advance RV_PC after a region whose fetches were already charged. */
bool gpsp_rv32_flow_advance_pc(gpsp_rv32_code *code,
    uint32_t pc, unsigned count, bool thumb);
bool gpsp_rv32_flow_advance(gpsp_rv32_code *code,
    uint32_t pc, bool thumb);
bool gpsp_rv32_flow_branch_advance(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target, bool thumb);
bool gpsp_rv32_flow_bl_pair_advance(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target);
bool gpsp_rv32_flow_set_branch_pc(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target);

/* The instruction lowerer supplies the target in A0 and a predicate in A1. */
bool gpsp_rv32_flow_dynamic_thumb_exit(gpsp_rv32_code *code,
    uint16_t opcode, bool target_ready);
bool gpsp_rv32_flow_dynamic_arm_bx(gpsp_rv32_code *code);
/* With fall_through, a not-taken conditional write goes on to the next
   op instead of returning to the dispatcher. */
bool gpsp_rv32_flow_dynamic_arm_pc_exit(gpsp_rv32_code *code,
    uint32_t pc, bool fall_through);

#endif

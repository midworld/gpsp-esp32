/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_BLOCK_EMIT_H
#define GPSP_RV32_BLOCK_EMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "rv32_codegen.h"

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
struct gpsp_rv32_emit_byte_stats
{
  uint64_t guard;
  uint64_t body;
  uint64_t timing;
  uint64_t batch_preflight;
  uint64_t batch_slow;
  uint64_t link_exit;
};

void gpsp_rv32_emit_get_byte_stats(struct gpsp_rv32_emit_byte_stats *stats);
#endif

typedef struct
{
  uint8_t *site;
  uint32_t target;
  bool thumb;
} gpsp_rv32_block_link;

/* Native static branches report link slots; helper exits end the block. */
bool gpsp_rv32_thumb_batch_alu(uint16_t opcode);
bool gpsp_rv32_thumb_rom_literal_address(uint32_t pc, uint16_t opcode,
    uint32_t gamepak_size, uint32_t *address);
bool gpsp_rv32_emit_thumb_literal_constant(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uint32_t value);
bool gpsp_rv32_thumb_batch_pc_sensitive(uint16_t opcode);
bool gpsp_rv32_thumb_multi_native(uint16_t opcode);
bool gpsp_rv32_emit_block_instruction(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint32_t opcode, bool thumb,
    bool allow_native, uint8_t needed_flags, bool batch_timing, bool *ends_block,
    gpsp_rv32_block_link *link);
/* Caller proves a single-entry Thumb span with no interior side effects. */
bool gpsp_rv32_emit_thumb_preflight(uint8_t **translation_ptr,
    uint8_t *translation_limit, uintptr_t fetch_wait_addr,
    unsigned instruction_count, uint8_t **slow_site);
bool gpsp_rv32_emit_thumb_batch_slow(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode);
bool gpsp_rv32_emit_thumb_batch_finish(uint8_t **translation_ptr,
    uint8_t *translation_limit, unsigned instruction_count);
bool gpsp_rv32_emit_thumb_side_entry(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uintptr_t fetch_wait_addr, unsigned prefix_count,
    unsigned remaining_count, uint8_t *body,
    uintptr_t preflight_stub, uint8_t **slow_literal);
bool gpsp_rv32_emit_block_return(uint8_t **translation_ptr,
    uint8_t *translation_limit);

/* Instruction bodies reused by the region emitter. */
bool gpsp_rv32_emit_thumb_memory_body(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, bool resume_on_budget,
    bool *advanced);
/* A RAM stub site whose fetch wait the enclosing region charges. */
bool gpsp_rv32_emit_thumb_memory_head(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode);
bool gpsp_rv32_emit_thumb_multi_body(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, bool *exits);

/* The RV32 emitter owns instruction timing and exits. */
#define block_prologue_size 0
#define generate_block_extra_vars_arm() ((void)&flag_status)
#define generate_block_extra_vars_thumb() ((void)&flag_status)
#define generate_block_prologue() ((void)0)
#define generate_cycle_update() ((void)cycle_count)
#define generate_branch_patch_conditional(source, target) \
  ((void)(source), (void)(target))
#define generate_branch_patch_unconditional(source, target) \
  ((void)(source), (void)(target))
#define arm_process_cheats() ((void)0)
#define thumb_process_cheats() ((void)0)

#define generate_translation_gate(type)                                     \
  do                                                                        \
  {                                                                         \
    if(!gpsp_rv32_emit_block_return(&translation_ptr,                      \
         translation_cache_limit))                                          \
    {                                                                       \
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;                \
      return false;                                                         \
    }                                                                       \
  } while(0)

/* VRAM uses the interpreter path. Never publish an empty VRAM block. */
#define generate_vram_interpreter(type)                                     \
  do                                                                        \
  {                                                                         \
    translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;                  \
    return false;                                                           \
  } while(0)

void init_emitter(bool must_swap);

#endif

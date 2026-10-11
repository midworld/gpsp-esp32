/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_BLOCK_RUNTIME_H
#define GPSP_RV32_BLOCK_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gpsp_rv32_block_init(void);
void gpsp_rv32_block_deinit(void);
bool gpsp_rv32_block_sync(void *begin, void *end);

#if defined(GPSP_BENCHMARK)
struct gpsp_rv32_site_stats
{
  uint64_t arm_ops;
  uint64_t thumb_ops;
  uint64_t thumb_loads;
  uint64_t thumb_stores;
  uint64_t thumb_literals;
  uint64_t thumb_nonmem_fallbacks;
  uint64_t thumb_push;
  uint64_t thumb_pop;
  uint64_t thumb_stmia;
  uint64_t thumb_ldmia;
};

struct gpsp_rv32_compile_stats
{
  uint64_t blocks;
  uint64_t guest_ops;
  uint64_t code_bytes;
  uint64_t flow_blocks;
  uint64_t flow_arm_blocks;
  uint64_t flow_thumb_blocks;
  uint64_t flow_ram_blocks;
  uint64_t flow_legacy_blocks;
  uint64_t flow_guest_ops;
  uint64_t flow_helper_ops;
  uint64_t flow_regions;
  uint64_t flow_entries;
  uint64_t flow_code_bytes;
  uint64_t bundled_guest_ops;
  uint64_t flushed_bytes;
  uint32_t max_guest_ops;
  uint32_t max_thumb_guest_ops;
  uint32_t max_block_bytes;
  uint32_t max_flush_bytes;
  uint32_t span_count[4];
};

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
struct gpsp_rv32_rom_byte_stats
{
  uint64_t total;
  uint64_t guard;
  uint64_t body;
  uint64_t timing;
  uint64_t batch_preflight;
  uint64_t batch_slow;
  uint64_t link_exit;
  uint64_t residual;
  uint64_t overcount_blocks;
};

struct gpsp_rv32_overlap_byte_stats
{
  uint64_t new_blocks;
  uint64_t new_bytes;
  uint64_t repeated_blocks;
  uint64_t repeated_bytes;
  uint64_t safe_blocks;
  uint64_t safe_bytes;
  uint64_t batch_blocks;
  uint64_t batch_bytes;
  uint64_t unknown_blocks;
  uint64_t unknown_bytes;
  uint64_t alias_hits;
  uint64_t alias_alloc_failures;
  uint64_t joins;
  uint64_t side_entry_created;
  uint64_t side_entry_bytes;
  uint64_t fused_branch_regions;
  uint64_t branch_side_created;
  uint64_t branch_side_bytes;
  uint64_t side_entry_hits;
  uint64_t side_entry_failures;
  uint64_t slow_rom_hits;
  uint64_t slow_ram_hits;
  uint64_t slow_rom_emitted;
  uint64_t slow_ram_emitted;
  uint64_t slow_rom_saved_bytes;
  uint64_t slow_ram_saved_bytes;
  uint64_t slow_rom_replacements;
  uint64_t slow_ram_replacements;
  uint32_t alias_entries;
  uint32_t alias_peak_entries;
  uint32_t alias_table_bytes;
  uint32_t side_pending;
  uint32_t slow_rom_table_bytes;
  uint32_t slow_ram_table_bytes;
};

void gpsp_rv32_block_get_rom_byte_stats(
    struct gpsp_rv32_rom_byte_stats *stats);
void gpsp_rv32_block_get_overlap_byte_stats(
    struct gpsp_rv32_overlap_byte_stats *stats);
#endif

struct gpsp_rv32_phase_cycles
{
  uint64_t total;
  uint64_t lookup;
  uint64_t block;
  uint64_t scalar;
};

struct gpsp_rv32_cache_stats
{
  uint32_t rom_used_bytes;
  uint32_t ram_used_bytes;
  uint32_t rom_capacity_bytes;
  uint32_t ram_capacity_bytes;
  uint32_t rom_flushes;
  uint32_t ram_flushes;
  uint64_t rom_discarded_bytes;
  uint64_t ram_discarded_bytes;
  uint32_t chunk_retries;
  uint64_t chunk_tail_bytes;
};

#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
struct gpsp_rv32_lookup_region_stats
{
  uint64_t hits;
  uint64_t misses;
  uint64_t hash_probes;
  uint64_t translate_calls;
  uint64_t translate_failures;
  uint64_t translate_cycles;
  uint64_t repeated_misses;
  uint64_t overlap_misses;
  uint64_t new_misses;
  uint64_t unknown_misses;
  uint64_t overlap_resume[9];
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  uint64_t overlap_safe_misses;
  uint64_t overlap_batch_misses;
  uint64_t overlap_unknown_misses;
#endif
};

struct gpsp_rv32_lookup_stats
{
  struct gpsp_rv32_lookup_region_stats bios;
  struct gpsp_rv32_lookup_region_stats rom;
  struct gpsp_rv32_lookup_region_stats ram;
  uint64_t top_hit_calls;
  uint64_t top_miss_calls;
  uint64_t top_hit_cycles;
  uint64_t top_miss_cycles;
  uint64_t side_create_calls;
  uint64_t side_create_failures;
  uint64_t side_create_cycles;
  uint64_t side_page_cycles;
  uint64_t side_emit_cycles;
  uint64_t side_publish_cycles;
  uint64_t side_publish_pending;
  uint32_t side_publish_max_pending;
  uint32_t rom_capacity_flushes;
  uint32_t ram_capacity_flushes;
  uint64_t guard_exits;
  uint64_t guard_exits_bios;
  uint64_t guard_lookups;
  uint64_t guard_lookups_bios;
  uint64_t miss_classify_cycles;
  uint64_t overlap_classify_cycles;
  uint64_t phase_join_cycles;
  uint64_t phase_scan_cycles;
  uint64_t phase_emit_cycles;
  uint64_t phase_record_cycles;
  uint64_t phase_publish_cycles;
  uint64_t phase_sync_cycles;
  uint64_t phase_patch_syncs;
};

void gpsp_rv32_block_get_lookup_stats(struct gpsp_rv32_lookup_stats *stats);
void gpsp_rv32_block_set_lookup_clock(uint64_t (*now)(void));
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
enum gpsp_rv32_lookup_resume_reason
{
  GPSP_RV32_LOOKUP_RESUME_INITIAL,
  GPSP_RV32_LOOKUP_RESUME_UPDATE_BUDGET,
  GPSP_RV32_LOOKUP_RESUME_UPDATE_PC_CHANGE,
  GPSP_RV32_LOOKUP_RESUME_READY,
  GPSP_RV32_LOOKUP_RESUME_IRQ,
  GPSP_RV32_LOOKUP_RESUME_SMC,
  GPSP_RV32_LOOKUP_RESUME_UPDATE_HALT,
  GPSP_RV32_LOOKUP_RESUME_OTHER,
  GPSP_RV32_LOOKUP_RESUME_GUARD
};
void gpsp_rv32_block_set_lookup_resume(
    enum gpsp_rv32_lookup_resume_reason reason);
void gpsp_rv32_block_note_guard_exit(uint32_t pc);
void gpsp_rv32_block_note_guard_lookup(uint32_t pc);
#endif
#endif

struct gpsp_rv32_interpreter_stats
{
  uint64_t generated_steps;
  uint64_t generated_arm;
  uint64_t generated_thumb;
  uint64_t generated_bios;
  uint64_t generated_ewram;
  uint64_t generated_iwram;
  uint64_t generated_rom;
  uint64_t generated_other;
  uint64_t direct_steps;
  uint64_t bulk_runs;
  uint64_t bulk_steps;
  uint64_t bulk_bios;
  uint64_t bulk_ewram;
  uint64_t bulk_iwram;
  uint64_t bulk_rom;
  uint64_t bulk_other;
};

struct gpsp_rv32_block_region_stats
{
  uint64_t bios;
  uint64_t ewram;
  uint64_t iwram;
  uint64_t rom;
  uint64_t other;
};

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
struct gpsp_rv32_hot_site
{
  uint32_t pc;
  uint32_t samples;
  uint64_t cycles;
};

struct gpsp_rv32_hot_sites
{
  uint64_t samples;
  uint64_t overflow;
  struct gpsp_rv32_hot_site top[4];
};

struct gpsp_rv32_jit_detail_stats
{
  uint64_t code_cycles;
  uint64_t helper_cycles;
  uint64_t helper_calls;
  uint64_t ram_load_fast;
  uint64_t ram_load_slow;
  uint64_t ram_store_fast;
  uint64_t ram_store_slow;
  uint64_t multi_fast;
  uint64_t multi_slow;
  uint64_t memory_cycles;
  uint64_t memory_calls;
  uint64_t memory_rom_loads;
  uint64_t memory_ram_loads;
  uint64_t memory_io_loads;
  uint64_t memory_other_loads;
  uint64_t memory_stores;
  uint64_t arm_memory_calls;
  uint64_t arm_block_calls;
  uint64_t arm_memory_cycles;
  uint64_t arm_block_cycles;
  uint64_t arm_memory_ram;
  uint64_t arm_memory_io;
  uint64_t arm_memory_video;
  uint64_t arm_memory_rom;
  uint64_t arm_memory_save;
  uint64_t arm_memory_other;
  uint64_t arm_memory_stores;
  uint64_t system_calls;
  uint64_t rom_read_body_cycles;
  uint64_t rom_read_body_calls;
  uint64_t rom_read_page_misses;
  uint64_t rom_read_bridge_cycles;
  uint64_t rom_read_bridge_calls;
  struct gpsp_rv32_block_region_stats block_cycles;
  uint64_t block_ready_exits;
  uint64_t block_budget_exits;
  uint64_t block_halt_exits;
  uint64_t block_irq_alerts;
  uint64_t block_smc_alerts;
  uint64_t resume_candidates;
  uint64_t resume_attempts;
  uint64_t resume_successes;
  uint64_t resume_rejections;
};

void gpsp_rv32_block_get_jit_detail(
    struct gpsp_rv32_jit_detail_stats *stats);
void gpsp_rv32_block_take_hot_sites(struct gpsp_rv32_hot_sites *stats);
#endif

struct gpsp_rv32_fallback_bucket
{
  uint8_t opcode_high_byte;
  uint32_t steps;
};

struct gpsp_rv32_fallback_histogram
{
  struct gpsp_rv32_fallback_bucket thumb[8];
  struct gpsp_rv32_fallback_bucket arm[8];
  uint32_t unknown_thumb;
  uint32_t unknown_arm;
};

void gpsp_rv32_block_get_stats(
    uint64_t *blocks, uint64_t *scalar_steps, uint32_t *cache_flushes,
    uint32_t *code_bytes, bool *ready);
uint64_t gpsp_rv32_block_get_interpreter_runs(void);
void gpsp_rv32_block_get_compile_stats(struct gpsp_rv32_compile_stats *stats);
void gpsp_rv32_block_get_site_stats(struct gpsp_rv32_site_stats *stats);
void gpsp_rv32_block_get_phase_cycles(struct gpsp_rv32_phase_cycles *cycles);
void gpsp_rv32_block_get_interpreter_stats(
    struct gpsp_rv32_interpreter_stats *stats);
void gpsp_rv32_block_reset_interpreter_stats(void);
void gpsp_rv32_block_note_direct_step(void);
void gpsp_rv32_block_get_region_stats(
    struct gpsp_rv32_block_region_stats *stats);
void gpsp_rv32_block_get_cache_stats(struct gpsp_rv32_cache_stats *stats);
uint32_t gpsp_rv32_block_get_ram_code_bytes(void);
uint32_t gpsp_rv32_block_get_ram_flushes(void);
void gpsp_rv32_block_get_fallback_histogram(
    struct gpsp_rv32_fallback_histogram *stats);
#endif

#ifdef __cplusplus
}
#endif

#endif

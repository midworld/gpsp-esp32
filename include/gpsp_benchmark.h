/* gpSP frame and JIT statistics for a benchmark frontend.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BENCHMARK_H
#define GPSP_BENCHMARK_H

#include <stdbool.h>
#include <stdint.h>

#include "gpsp_benchmark_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct gpsp_benchmark_frame
{
    uint32_t execute_us;
    uint32_t audio_us;
    uint32_t video_us;
    struct gpsp_benchmark_timing cpu;
#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
    struct gpsp_benchmark_video_detail video_detail;
#endif
};

struct gpsp_benchmark_jit_sites
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

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
struct gpsp_benchmark_jit_code_bytes
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

struct gpsp_benchmark_jit_overlap_bytes
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
#endif

struct gpsp_benchmark_jit_region
{
    uint64_t bios;
    uint64_t ewram;
    uint64_t iwram;
    uint64_t rom;
    uint64_t other;
};

struct gpsp_benchmark_jit_fallback_bucket
{
    uint8_t opcode_high_byte;
    uint32_t steps;
};

struct gpsp_benchmark_jit_detail
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
    struct gpsp_benchmark_jit_region block_cycles;
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

struct gpsp_benchmark_jit_hot_site
{
    uint32_t pc;
    uint32_t samples;
    uint64_t cycles;
};

struct gpsp_benchmark_jit_hot_sites
{
    uint64_t samples;
    uint64_t overflow;
    struct gpsp_benchmark_jit_hot_site top[4];
};

struct gpsp_benchmark_jit_lookup_region
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

struct gpsp_benchmark_jit_lookup
{
    struct gpsp_benchmark_jit_lookup_region bios;
    struct gpsp_benchmark_jit_lookup_region rom;
    struct gpsp_benchmark_jit_lookup_region ram;
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

struct gpsp_benchmark_jit_lookup_frame
{
    uint64_t hit_calls;
    uint64_t miss_calls;
    uint64_t hit_cycles;
    uint64_t miss_cycles;
};

struct gpsp_benchmark_arm_video_memory
{
    uint64_t calls[3][2][3];
    uint64_t cycles[3][2][3];
};

struct gpsp_benchmark_arm_video_source
{
    uint64_t bios_store_calls[3];
    uint64_t bios_store_cycles[3];
    uint64_t pc_7b4_store_calls[3];
    uint64_t pc_7b4_store_cycles[3];
};

struct gpsp_benchmark_bios_site
{
    uint32_t pc;
    uint32_t calls;
};

struct gpsp_benchmark_bios_probe
{
    uint64_t block_calls;
    uint64_t block_to_bios;
    uint64_t block_bios_to_bios;
    uint64_t block_bios_to_bios_ready;
    uint64_t block_bios_to_other;
    uint64_t block_bios_to_other_ready;
    uint64_t update_to_bios;
    uint64_t update_from_bios;
    uint64_t irq_to_bios;
    uint64_t irq_from_bios;
    uint64_t interpreter_to_bios;
    uint64_t interpreter_from_bios;
    struct gpsp_benchmark_bios_site top_blocks[8];
    uint64_t fastset_outer_from_7a8_ready;
    uint64_t fastset_outer_from_7b8_ready;
    uint64_t fastset_inner_from_7b8_ready;
    uint64_t swi_entry_to_64_ready;
};

struct gpsp_benchmark_engine
{
    uint64_t blocks;
    uint64_t scalar_steps;
    uint64_t scalar_runs;
    uint64_t total_cycles;
    uint64_t lookup_cycles;
    uint64_t block_cycles;
    uint64_t scalar_cycles;
    uint32_t cache_flushes;
    uint32_t ram_flushes;
    uint32_t code_bytes;
    uint32_t ram_code_bytes;
    uint32_t rom_code_capacity_bytes;
    uint32_t ram_code_capacity_bytes;
    uint64_t rom_discarded_bytes;
    uint64_t ram_discarded_bytes;
    uint32_t chunk_retries;
    uint64_t chunk_tail_bytes;
    uint64_t translated_blocks;
    uint64_t translated_guest_ops;
    uint64_t translated_code_bytes;
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
    uint64_t flushed_code_bytes;
    uint32_t max_guest_ops;
    uint32_t max_thumb_guest_ops;
    uint32_t max_block_bytes;
    uint32_t max_flush_bytes;
    uint32_t thumb_span_count[4];
    struct gpsp_benchmark_jit_sites jit_sites;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    struct gpsp_benchmark_jit_code_bytes jit_code_bytes;
    struct gpsp_benchmark_jit_overlap_bytes jit_overlap_bytes;
#endif
    uint64_t generated_steps;
    uint64_t generated_arm;
    uint64_t generated_thumb;
    uint64_t direct_steps;
    uint64_t bulk_runs;
    uint64_t bulk_steps;
    struct gpsp_benchmark_jit_region generated_region;
    struct gpsp_benchmark_jit_region bulk_region;
    struct gpsp_benchmark_jit_region block_region;
    struct gpsp_benchmark_jit_detail jit_detail;
    struct gpsp_benchmark_jit_hot_sites jit_hot;
    struct gpsp_benchmark_jit_lookup jit_lookup;
    struct gpsp_benchmark_arm_video_memory arm_video_memory;
    struct gpsp_benchmark_arm_video_source arm_video_source;
    struct gpsp_benchmark_jit_fallback_bucket fallback_thumb_top[8];
    struct gpsp_benchmark_jit_fallback_bucket fallback_arm_top[8];
    bool ready;
};

/* Call on the frontend task after retro_run() returns. */
void gpsp_benchmark_get_last_frame(struct gpsp_benchmark_frame *frame);
void gpsp_benchmark_get_engine(struct gpsp_benchmark_engine *engine);
void gpsp_benchmark_take_hot_sites(
        struct gpsp_benchmark_jit_hot_sites *sites);
void gpsp_benchmark_get_lookup_totals(
        struct gpsp_benchmark_jit_lookup_frame *totals);
void gpsp_benchmark_reset_bios_probe(void);
void gpsp_benchmark_get_bios_probe(struct gpsp_benchmark_bios_probe *probe);
#if defined(GPSP_BENCHMARK_RAM_CODE)
/* Block JIT benchmarks only; totals since boot. */
void gpsp_rv32_smc_bench_report(void);
#endif
/* The platform supplies a retired-instruction counter. */
void gpsp_benchmark_set_instret_counter(uint32_t (*now)(void));
#ifdef __cplusplus
}
#endif

#endif /* GPSP_BENCHMARK_H */

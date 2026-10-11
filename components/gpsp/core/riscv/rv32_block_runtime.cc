/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

extern "C" {
#include "../common.h"
#include "rv32_block_runtime.h"
#include "rv32_bios_probe.h"
#include "rv32_arm_system.h"
#include "rv32_cycle_budget.h"
#include "rv32_stub.h"
#include "rv32_thumb_memory.h"
#include "gpsp_rv32_platform.h"
}

#include <stdint.h>
#include <string.h>

#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
#define rv32_note_lookup_resume(reason) \
  gpsp_rv32_block_set_lookup_resume(reason)
static bool guard_pending;
static u32 guard_pc;
static u32 guard_mode;
static u32 guard_rom_flushes;
static u32 guard_ram_flushes;
#else
#define rv32_note_lookup_resume(reason) ((void)0)
#endif

static const char *const TAG = "gpsp_rv32_block";
static const size_t CODE_ALIGNMENT = 64;

static_assert(!(GPSP_RV32_CHUNK_BYTES & (GPSP_RV32_CHUNK_BYTES - 1)),
    "code cache chunks must be a power of two");
static_assert(ROM_TRANSLATION_CACHE_SIZE % GPSP_RV32_CHUNK_BYTES == 0 &&
    RAM_TRANSLATION_CACHE_SIZE % GPSP_RV32_CHUNK_BYTES == 0,
    "code cache limits must be whole chunks");
static_assert(ROM_TRANSLATION_CACHE_SIZE / GPSP_RV32_CHUNK_BYTES <=
    GPSP_RV32_ARENA_MAX && RAM_TRANSLATION_CACHE_SIZE /
    GPSP_RV32_CHUNK_BYTES <= GPSP_RV32_ARENA_MAX,
    "code cache limits need too many chunks");

extern "C" {
gpsp_rv32_arena gpsp_rv32_rom_arena;
gpsp_rv32_arena gpsp_rv32_ram_arena;
extern u32 rv32_rom_cache_generation;
extern u32 rv32_ram_cache_generation;
extern u32 gpsp_rv32_branch_cache[];
}

static bool block_ready;
static uint64_t executed_blocks;
static uint64_t interpreted_steps;
#if defined(GPSP_BENCHMARK)
static uint64_t interpreted_runs;
static struct gpsp_rv32_phase_cycles phase_cycles;
static struct gpsp_rv32_block_region_stats block_region_stats;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
static struct gpsp_rv32_jit_detail_stats block_exit_stats;
#define RV32_HOT_TABLE_SIZE 1024
static struct gpsp_rv32_hot_site hot_table[RV32_HOT_TABLE_SIZE];
static uint64_t hot_samples;
static uint64_t hot_overflow;

static struct gpsp_rv32_hot_site *note_hot_pc(u32 pc, bool thumb)
{
  if (executed_blocks & 63)
    return NULL;

  u32 key = pc | (u32)thumb;
  u32 slot = (key * 2654435761U) & (RV32_HOT_TABLE_SIZE - 1);
  hot_samples++;
  for (unsigned probe = 0; probe < 16; probe++)
  {
    struct gpsp_rv32_hot_site *site = &hot_table[slot];
    if (!site->samples)
      site->pc = key;
    if (!site->samples || site->pc == key)
    {
      site->samples++;
      return site;
    }
    slot = (slot + 1) & (RV32_HOT_TABLE_SIZE - 1);
  }
  hot_overflow++;
  return NULL;
}

extern "C" {
uint64_t gpsp_rv32_jit_code_cycles;
uint64_t gpsp_rv32_jit_helper_cycles;
uint64_t gpsp_rv32_jit_helper_calls;
uint64_t gpsp_rv32_jit_ram_load_fast;
uint64_t gpsp_rv32_jit_ram_load_slow;
uint64_t gpsp_rv32_jit_ram_store_fast;
uint64_t gpsp_rv32_jit_ram_store_slow;
uint64_t gpsp_rv32_jit_multi_fast;
uint64_t gpsp_rv32_jit_multi_slow;
uint64_t gpsp_rv32_jit_memory_cycles;
uint64_t gpsp_rv32_jit_memory_calls;
uint64_t gpsp_rv32_jit_memory_rom_loads;
uint64_t gpsp_rv32_jit_memory_ram_loads;
uint64_t gpsp_rv32_jit_memory_io_loads;
uint64_t gpsp_rv32_jit_memory_other_loads;
uint64_t gpsp_rv32_jit_memory_stores;
uint64_t gpsp_rv32_jit_arm_memory_calls;
uint64_t gpsp_rv32_jit_arm_block_calls;
uint64_t gpsp_rv32_jit_arm_memory_cycles;
uint64_t gpsp_rv32_jit_arm_block_cycles;
uint64_t gpsp_rv32_jit_arm_memory_ram;
uint64_t gpsp_rv32_jit_arm_memory_io;
uint64_t gpsp_rv32_jit_arm_memory_video;
uint64_t gpsp_rv32_jit_arm_memory_rom;
uint64_t gpsp_rv32_jit_arm_memory_save;
uint64_t gpsp_rv32_jit_arm_memory_other;
uint64_t gpsp_rv32_jit_arm_memory_stores;
uint64_t gpsp_rv32_jit_system_calls;
uint64_t gpsp_rv32_jit_rom_read_body_cycles;
uint64_t gpsp_rv32_jit_rom_read_body_calls;
uint64_t gpsp_rv32_jit_rom_read_page_misses;
uint64_t gpsp_rv32_jit_rom_read_bridge_cycles;
uint64_t gpsp_rv32_jit_rom_read_bridge_calls;
}
static struct gpsp_rv32_block_region_stats block_region_cycles;
static struct gpsp_rv32_bios_probe_stats bios_probe;
/* BIOS block PCs are halfword-aligned; direct indexing avoids a hot-path hash. */
static uint32_t bios_block_sources[0x4000 / 2];

extern "C" void gpsp_rv32_bios_probe_reset(void)
{
  memset(&bios_probe, 0, sizeof(bios_probe));
  memset(bios_block_sources, 0, sizeof(bios_block_sources));
}

extern "C" void gpsp_rv32_bios_probe_get_stats(
    struct gpsp_rv32_bios_probe_stats *stats)
{
  *stats = bios_probe;
  for (unsigned i = 0; i < 0x4000 / 2; i++)
  {
    uint32_t calls = bios_block_sources[i];
    if (!calls || calls <= stats->top_blocks[7].calls)
      continue;
    unsigned at = 7;
    while (at && calls > stats->top_blocks[at - 1].calls)
    {
      stats->top_blocks[at] = stats->top_blocks[at - 1];
      at--;
    }
    stats->top_blocks[at].pc = i * 2;
    stats->top_blocks[at].calls = calls;
  }
}

static inline void bios_probe_transition(uint32_t before, uint32_t after,
    uint64_t *to_bios, uint64_t *from_bios)
{
  if (before >= 0x4000 && after < 0x4000)
    ++*to_bios;
  else if (before < 0x4000 && after >= 0x4000)
    ++*from_bios;
}

static void reset_jit_detail(void)
{
  memset(&block_exit_stats, 0, sizeof(block_exit_stats));
  memset(hot_table, 0, sizeof(hot_table));
  hot_samples = 0;
  hot_overflow = 0;
  gpsp_rv32_jit_code_cycles = 0;
  gpsp_rv32_jit_helper_cycles = 0;
  gpsp_rv32_jit_helper_calls = 0;
  gpsp_rv32_jit_ram_load_fast = 0;
  gpsp_rv32_jit_ram_load_slow = 0;
  gpsp_rv32_jit_ram_store_fast = 0;
  gpsp_rv32_jit_ram_store_slow = 0;
  gpsp_rv32_jit_multi_fast = 0;
  gpsp_rv32_jit_multi_slow = 0;
  gpsp_rv32_jit_memory_cycles = 0;
  gpsp_rv32_jit_memory_calls = 0;
  gpsp_rv32_jit_memory_rom_loads = 0;
  gpsp_rv32_jit_memory_ram_loads = 0;
  gpsp_rv32_jit_memory_io_loads = 0;
  gpsp_rv32_jit_memory_other_loads = 0;
  gpsp_rv32_jit_memory_stores = 0;
  gpsp_rv32_jit_arm_memory_calls = 0;
  gpsp_rv32_jit_arm_block_calls = 0;
  gpsp_rv32_jit_arm_memory_cycles = 0;
  gpsp_rv32_jit_arm_block_cycles = 0;
  gpsp_rv32_jit_arm_memory_ram = 0;
  gpsp_rv32_jit_arm_memory_io = 0;
  gpsp_rv32_jit_arm_memory_video = 0;
  gpsp_rv32_jit_arm_memory_rom = 0;
  gpsp_rv32_jit_arm_memory_save = 0;
  gpsp_rv32_jit_arm_memory_other = 0;
  gpsp_rv32_jit_arm_memory_stores = 0;
  gpsp_rv32_jit_system_calls = 0;
  gpsp_rv32_jit_rom_read_body_cycles = 0;
  gpsp_rv32_jit_rom_read_body_calls = 0;
  gpsp_rv32_jit_rom_read_page_misses = 0;
  gpsp_rv32_jit_rom_read_bridge_cycles = 0;
  gpsp_rv32_jit_rom_read_bridge_calls = 0;
  memset(&block_region_cycles, 0, sizeof(block_region_cycles));
  gpsp_rv32_bios_probe_reset();
}
#endif

static inline uint32_t rv32_cycles_now(void)
{
  uint32_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles) :: "memory");
  return cycles;
}

static inline uint32_t rv32_cycles_since(uint32_t start)
{
  return rv32_cycles_now() - start;
}
#endif
static uint32_t frame_calls;

/* The stub's lean path also counts updates for progress checks. */
extern "C" uint32_t gpsp_rv32_native_budget_updates;
uint32_t gpsp_rv32_native_budget_updates;
#define native_budget_updates gpsp_rv32_native_budget_updates
#if defined(GPSP_BENCHMARK)
extern "C" uint64_t gpsp_rv32_native_budget_cycles;
uint64_t gpsp_rv32_native_budget_cycles;
#endif

extern "C" uint64_t gpsp_rv32_native_budget_update(
    uint32_t *regs, int32_t remaining)
{
  u32 pc = regs[REG_PC];
  u32 cpsr = regs[REG_CPSR];
  u32 cpu_mode = regs[CPU_MODE];
  u32 rom_generation = rv32_rom_cache_generation;
  u32 ram_generation = rv32_ram_cache_generation;
#if defined(GPSP_BENCHMARK)
  u32 start = rv32_cycles_now();
#endif
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  block_exit_stats.block_budget_exits++;
  block_exit_stats.resume_candidates++;
#endif
  native_budget_updates++;
  u32 next = gpsp_rv32_update_cycles(remaining, update_gba,
      &execute_cycles, &gpsp_rv32_frame_cycle_debt);
  rv32_note_lookup_resume(regs[REG_PC] != pc ?
      GPSP_RV32_LOOKUP_RESUME_UPDATE_PC_CHANGE :
      GPSP_RV32_LOOKUP_RESUME_UPDATE_BUDGET);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  bios_probe_transition(pc, regs[REG_PC],
      &bios_probe.update_to_bios, &bios_probe.update_from_bios);
#endif
  u32 alert = gpsp_rv32_take_memory_alert();
  if (alert & CPU_ALERT_SMC)
    invalidate_translation_cache_ram();

  bool attempt = !(next & 0xc0000000u) && !alert;
  bool resume = attempt && regs[CPU_HALT_STATE] == CPU_ACTIVE &&
      regs[REG_PC] == pc && regs[REG_CPSR] == cpsr &&
      regs[CPU_MODE] == cpu_mode && pc != cheat_master_hook &&
      rv32_rom_cache_generation == rom_generation &&
      rv32_ram_cache_generation == ram_generation;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  if (attempt && !(next & 0x80000000u))
  {
    block_exit_stats.resume_attempts++;
    if (resume)
      block_exit_stats.resume_successes++;
    else
      block_exit_stats.resume_rejections++;
  }
#endif
#if defined(GPSP_BENCHMARK)
  gpsp_rv32_native_budget_cycles += rv32_cycles_since(start);
#endif
  return (uint64_t)next | ((uint64_t)resume << 32);
}

/* Lean stub path after update_gba(remaining >= -64) could not resume. */
extern "C" uint32_t gpsp_rv32_native_budget_finish(uint32_t next)
{
  native_budget_updates++;
  u32 alert = gpsp_rv32_take_memory_alert();
  if (alert & CPU_ALERT_SMC)
    invalidate_translation_cache_ram();
  if (next & 0x80000000u)
    gpsp_rv32_frame_cycle_debt = execute_cycles - (next & 0x7fffu);
  return next;
}

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
static inline u32 rv32_check_and_raise_interrupts(void)
{
  u32 before = reg[REG_PC];
  u32 raised = check_and_raise_interrupts();
  if (raised)
    bios_probe_transition(before, reg[REG_PC],
        &bios_probe.irq_to_bios, &bios_probe.irq_from_bios);
  return raised;
}
#else
#define rv32_check_and_raise_interrupts() check_and_raise_interrupts()
#endif

#if defined(GPSP_BENCHMARK)
extern "C" uint32_t rv32_rom_cache_flush_count;
extern "C" uint32_t rv32_ram_cache_flush_count;
extern "C" uint64_t rv32_rom_cache_discarded_bytes;
extern "C" uint64_t rv32_ram_cache_discarded_bytes;
extern "C" uint32_t rv32_chunk_retries;
extern "C" uint64_t rv32_chunk_tail_bytes;
extern "C" void gpsp_rv32_block_get_stats(
    uint64_t *blocks, uint64_t *scalar_steps, uint32_t *cache_flushes,
    uint32_t *code_bytes, bool *ready)
{
  *blocks = executed_blocks;
  *scalar_steps = interpreted_steps;
  *cache_flushes = rv32_rom_cache_flush_count;
  *code_bytes = rom_translation_ptr ?
      gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena, rom_translation_ptr) : 0;
  *ready = block_ready;
}

extern "C" uint64_t gpsp_rv32_block_get_interpreter_runs(void)
{
  return interpreted_runs;
}

extern "C" uint32_t gpsp_rv32_block_get_ram_code_bytes(void)
{
  return ram_translation_ptr ?
      gpsp_rv32_arena_offset(&gpsp_rv32_ram_arena, ram_translation_ptr) : 0;
}

extern "C" uint32_t gpsp_rv32_block_get_ram_flushes(void)
{
  return rv32_ram_cache_flush_count;
}

extern "C" void gpsp_rv32_block_get_cache_stats(
    struct gpsp_rv32_cache_stats *stats)
{
  /* Offsets count every chunk before the current one as used. */
  stats->rom_used_bytes = rom_translation_ptr ?
      gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena, rom_translation_ptr) : 0;
  stats->ram_used_bytes = ram_translation_ptr ?
      gpsp_rv32_arena_offset(&gpsp_rv32_ram_arena, ram_translation_ptr) : 0;
  stats->rom_capacity_bytes = gpsp_rv32_arena_capacity(&gpsp_rv32_rom_arena);
  stats->ram_capacity_bytes = gpsp_rv32_arena_capacity(&gpsp_rv32_ram_arena);
  stats->rom_flushes = rv32_rom_cache_flush_count;
  stats->ram_flushes = rv32_ram_cache_flush_count;
  stats->rom_discarded_bytes = rv32_rom_cache_discarded_bytes;
  stats->ram_discarded_bytes = rv32_ram_cache_discarded_bytes;
  stats->chunk_retries = rv32_chunk_retries;
  stats->chunk_tail_bytes = rv32_chunk_tail_bytes;
}

extern "C" void gpsp_rv32_block_get_phase_cycles(
    struct gpsp_rv32_phase_cycles *cycles)
{
  *cycles = phase_cycles;
}

extern "C" void gpsp_rv32_block_get_region_stats(
    struct gpsp_rv32_block_region_stats *stats)
{
  *stats = block_region_stats;
}

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
extern "C" void gpsp_rv32_block_get_jit_detail(
    struct gpsp_rv32_jit_detail_stats *stats)
{
  stats->code_cycles = gpsp_rv32_jit_code_cycles;
  stats->helper_cycles = gpsp_rv32_jit_helper_cycles;
  stats->helper_calls = gpsp_rv32_jit_helper_calls;
  stats->ram_load_fast = gpsp_rv32_jit_ram_load_fast;
  stats->ram_load_slow = gpsp_rv32_jit_ram_load_slow;
  stats->ram_store_fast = gpsp_rv32_jit_ram_store_fast;
  stats->ram_store_slow = gpsp_rv32_jit_ram_store_slow;
  stats->multi_fast = gpsp_rv32_jit_multi_fast;
  stats->multi_slow = gpsp_rv32_jit_multi_slow;
  stats->memory_cycles = gpsp_rv32_jit_memory_cycles;
  stats->memory_calls = gpsp_rv32_jit_memory_calls;
  stats->memory_rom_loads = gpsp_rv32_jit_memory_rom_loads;
  stats->memory_ram_loads = gpsp_rv32_jit_memory_ram_loads;
  stats->memory_io_loads = gpsp_rv32_jit_memory_io_loads;
  stats->memory_other_loads = gpsp_rv32_jit_memory_other_loads;
  stats->memory_stores = gpsp_rv32_jit_memory_stores;
  stats->arm_memory_calls = gpsp_rv32_jit_arm_memory_calls;
  stats->arm_block_calls = gpsp_rv32_jit_arm_block_calls;
  stats->arm_memory_cycles = gpsp_rv32_jit_arm_memory_cycles;
  stats->arm_block_cycles = gpsp_rv32_jit_arm_block_cycles;
  stats->arm_memory_ram = gpsp_rv32_jit_arm_memory_ram;
  stats->arm_memory_io = gpsp_rv32_jit_arm_memory_io;
  stats->arm_memory_video = gpsp_rv32_jit_arm_memory_video;
  stats->arm_memory_rom = gpsp_rv32_jit_arm_memory_rom;
  stats->arm_memory_save = gpsp_rv32_jit_arm_memory_save;
  stats->arm_memory_other = gpsp_rv32_jit_arm_memory_other;
  stats->arm_memory_stores = gpsp_rv32_jit_arm_memory_stores;
  stats->system_calls = gpsp_rv32_jit_system_calls;
  stats->rom_read_body_cycles = gpsp_rv32_jit_rom_read_body_cycles;
  stats->rom_read_body_calls = gpsp_rv32_jit_rom_read_body_calls;
  stats->rom_read_page_misses = gpsp_rv32_jit_rom_read_page_misses;
  stats->rom_read_bridge_cycles = gpsp_rv32_jit_rom_read_bridge_cycles;
  stats->rom_read_bridge_calls = gpsp_rv32_jit_rom_read_bridge_calls;
  stats->block_cycles = block_region_cycles;
  stats->block_ready_exits = block_exit_stats.block_ready_exits;
  stats->block_budget_exits = block_exit_stats.block_budget_exits;
  stats->block_halt_exits = block_exit_stats.block_halt_exits;
  stats->block_irq_alerts = block_exit_stats.block_irq_alerts;
  stats->block_smc_alerts = block_exit_stats.block_smc_alerts;
  stats->resume_candidates = block_exit_stats.resume_candidates;
  stats->resume_attempts = block_exit_stats.resume_attempts;
  stats->resume_successes = block_exit_stats.resume_successes;
  stats->resume_rejections = block_exit_stats.resume_rejections;
}

extern "C" void gpsp_rv32_block_take_hot_sites(
    struct gpsp_rv32_hot_sites *stats)
{
  memset(stats, 0, sizeof(*stats));
  stats->samples = hot_samples;
  stats->overflow = hot_overflow;
  for (unsigned i = 0; i < RV32_HOT_TABLE_SIZE; i++)
  {
    struct gpsp_rv32_hot_site site = hot_table[i];
    if (!site.samples || site.cycles <= stats->top[3].cycles)
      continue;
    unsigned rank = 3;
    while (rank && site.cycles > stats->top[rank - 1].cycles)
    {
      stats->top[rank] = stats->top[rank - 1];
      rank--;
    }
    stats->top[rank] = site;
  }
  memset(hot_table, 0, sizeof(hot_table));
  hot_samples = 0;
  hot_overflow = 0;
}
#endif
#endif

static const char *arena_name(const gpsp_rv32_arena *arena)
{
  return arena == &gpsp_rv32_rom_arena ? "ROM" : "RAM";
}

extern "C" bool gpsp_rv32_arena_grow(gpsp_rv32_arena *arena)
{
  if (arena->count >= arena->max)
    return false;
  u8 *chunk = (u8 *)gpsp_rv32_exec_alloc(CODE_ALIGNMENT,
      GPSP_RV32_CHUNK_BYTES);
  if (!chunk)
  {
    /* Stay at this size instead of retrying for every block. */
    arena->max = arena->count;
    gpsp_rv32_log(GPSP_RV32_LOG_WARN, TAG,
        "%s code cache stays at %u KiB: allocation failed",
        arena_name(arena), (unsigned)(gpsp_rv32_arena_capacity(arena) / 1024));
    return false;
  }
  memset(chunk, 0, GPSP_RV32_CHUNK_BYTES);
  arena->base[arena->count++] = chunk;
  gpsp_rv32_log(GPSP_RV32_LOG_INFO, TAG, "%s code cache: %u of %u KiB",
      arena_name(arena), (unsigned)(gpsp_rv32_arena_capacity(arena) / 1024),
      (unsigned)(arena->max * GPSP_RV32_CHUNK_BYTES / 1024));
  return true;
}

static void arena_free(gpsp_rv32_arena *arena)
{
  for (unsigned i = 0; i < arena->count; i++)
    gpsp_rv32_exec_free(arena->base[i]);
  memset(arena, 0, sizeof(*arena));
}

extern "C" bool gpsp_rv32_block_init(void)
{
  if (block_ready)
    return true;

  if (gpsp_rv32_rom_arena.count || gpsp_rv32_ram_arena.count)
    gpsp_rv32_block_deinit();

  gpsp_rv32_rom_arena.max = ROM_TRANSLATION_CACHE_SIZE / GPSP_RV32_CHUNK_BYTES;
  gpsp_rv32_ram_arena.max = RAM_TRANSLATION_CACHE_SIZE / GPSP_RV32_CHUNK_BYTES;
  if (!gpsp_rv32_arena_grow(&gpsp_rv32_rom_arena) ||
      !gpsp_rv32_arena_grow(&gpsp_rv32_ram_arena) || !rv32_ram_tags_alloc())
  {
    gpsp_rv32_log(GPSP_RV32_LOG_WARN, TAG,
        "executable block cache allocation unavailable");
    gpsp_rv32_block_deinit();
    return false;
  }

  block_ready = true;
  executed_blocks = 0;
  interpreted_steps = 0;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
  guard_pending = false;
#endif
#if defined(GPSP_BENCHMARK)
  interpreted_runs = 0;
  memset(&phase_cycles, 0, sizeof(phase_cycles));
  memset(&block_region_stats, 0, sizeof(block_region_stats));
  gpsp_rv32_native_budget_cycles = 0;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  reset_jit_detail();
#endif
  gpsp_rv32_block_reset_interpreter_stats();
#endif
  frame_calls = 0;
  native_budget_updates = 0;
#if defined(GPSP_BENCHMARK)
  rv32_rom_cache_flush_count = 0;
  rv32_ram_cache_flush_count = 0;
#endif
  return true;
}

extern "C" void gpsp_rv32_block_deinit(void)
{
  block_ready = false;
  executed_blocks = 0;
  interpreted_steps = 0;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
  guard_pending = false;
#endif
#if defined(GPSP_BENCHMARK)
  interpreted_runs = 0;
  memset(&phase_cycles, 0, sizeof(phase_cycles));
  memset(&block_region_stats, 0, sizeof(block_region_stats));
  gpsp_rv32_native_budget_cycles = 0;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  reset_jit_detail();
#endif
#endif
  frame_calls = 0;
  native_budget_updates = 0;
  arena_free(&gpsp_rv32_rom_arena);
  arena_free(&gpsp_rv32_ram_arena);
  rv32_ram_tags_free();
  rv32_tables_free();
  rom_translation_ptr = NULL;
  ram_translation_ptr = NULL;
}

#if defined(GPSP_RV32_TEST_SYNC_FAULT)
#if !defined(GPSP_RV32_TEST_RAM_SMC) || \
    (GPSP_RV32_TEST_SYNC_FAULT != 1 && GPSP_RV32_TEST_SYNC_FAULT != 2)
#error "Sync faults require a RAM SMC test build and mode 1 or 2"
#endif
/* Test builds: syncs left to fail; ~0u fails all. */
extern "C" { u32 gpsp_rv32_test_sync_faults; }
#endif

extern "C" bool gpsp_rv32_block_sync(void *begin, void *end)
{
#if defined(GPSP_RV32_TEST_SYNC_FAULT)
  if (gpsp_rv32_test_sync_faults)
  {
    if (gpsp_rv32_test_sync_faults != ~0u)
      gpsp_rv32_test_sync_faults--;
    return false;
  }
#endif
  uintptr_t first = (uintptr_t)begin;
  uintptr_t limit = (uintptr_t)end;
  if (!block_ready || first > limit)
    return false;
  if (first == limit)
    return true;

  if (!gpsp_rv32_arena_holds(&gpsp_rv32_rom_arena, begin, end) &&
      !gpsp_rv32_arena_holds(&gpsp_rv32_ram_arena, begin, end))
    return false;
  return gpsp_rv32_exec_publish(begin, limit - first);
}

extern "C" u32 execute_arm_translate(u32 cycles)
{
#if defined(GPSP_BENCHMARK)
  uint32_t total_start = rv32_cycles_now();
#endif
  clear_gamepak_stickybits();
  s32 remaining = gpsp_rv32_take_cycles(cycles, &gpsp_rv32_frame_cycle_debt);
  if (!block_ready)
  {
#if defined(GPSP_BENCHMARK)
    uint32_t scalar_start = rv32_cycles_now();
#endif
    u32 next = 0;
    while (remaining <= 0)
    {
      next = gpsp_rv32_update_cycles(remaining, update_gba,
          &execute_cycles, &gpsp_rv32_frame_cycle_debt);
      if (completed_frame(next))
        break;
      remaining = (s32)cycles_to_run(next);
    }
    if (!completed_frame(next))
    {
      execute_arm((u32)remaining);
      next = 0;
    }
#if defined(GPSP_BENCHMARK)
    phase_cycles.scalar += rv32_cycles_since(scalar_start);
    phase_cycles.total += rv32_cycles_since(total_start);
#endif
    return next;
  }

  u8 *resume = NULL;
  u32 resume_pc = 0;
  u32 resume_mode = 0;
  u32 resume_cpu_mode = 0;
  u32 resume_rom_generation = 0;
  u32 resume_ram_generation = 0;
  for (;;)
  {
    if (remaining <= 0 || reg[CPU_HALT_STATE] != CPU_ACTIVE)
    {
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
      u32 update_start_pc = reg[REG_PC];
      bool budget_update = remaining <= 0;
#endif
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
      u32 before = reg[REG_PC];
#endif
      u32 next = gpsp_rv32_update_cycles(remaining, update_gba,
          &execute_cycles, &gpsp_rv32_frame_cycle_debt);
      if (next & 0x40000000)
        resume = NULL;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
      rv32_note_lookup_resume(reg[REG_PC] != update_start_pc ?
          GPSP_RV32_LOOKUP_RESUME_UPDATE_PC_CHANGE : budget_update ?
          GPSP_RV32_LOOKUP_RESUME_UPDATE_BUDGET :
          GPSP_RV32_LOOKUP_RESUME_UPDATE_HALT);
#endif
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
      bios_probe_transition(before, reg[REG_PC],
          &bios_probe.update_to_bios, &bios_probe.update_from_bios);
#endif
      u32 update_alert = gpsp_rv32_take_memory_alert();
      if (update_alert & CPU_ALERT_SMC)
        invalidate_translation_cache_ram();
      if (update_alert)
      {
        resume = NULL;
      }
      if (completed_frame(next))
      {
        frame_calls++;
#if !defined(GPSP_BENCHMARK)
        if (frame_calls == 1 || frame_calls % 60 == 0)
          gpsp_rv32_log(GPSP_RV32_LOG_INFO, TAG,
              "JIT stats: frames=%u blocks=%llu scalar=%llu "
              "code=%u", (unsigned)frame_calls,
              (unsigned long long)executed_blocks,
              (unsigned long long)interpreted_steps,
              (unsigned)gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena,
                rom_translation_ptr));
#endif
#if defined(GPSP_BENCHMARK)
        phase_cycles.total += rv32_cycles_since(total_start);
#endif
        return next;
      }
      remaining = (s32)cycles_to_run(next);
      continue;
    }

    u32 raw_pc = reg[REG_PC];
    u32 region = raw_pc >> 24;
    if (raw_pc == cheat_master_hook)
    {
      resume = NULL;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
      guard_pending = false;
#endif
      interpreted_steps++;
#if defined(GPSP_BENCHMARK)
      gpsp_rv32_block_note_direct_step();
      interpreted_runs++;
      uint32_t scalar_start = rv32_cycles_now();
#endif
      remaining = execute_arm_one(remaining);
      rv32_note_lookup_resume(GPSP_RV32_LOOKUP_RESUME_OTHER);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
      bios_probe_transition(raw_pc, reg[REG_PC],
          &bios_probe.interpreter_to_bios,
          &bios_probe.interpreter_from_bios);
#endif
      u32 alert = gpsp_rv32_take_memory_alert();
      if (alert & CPU_ALERT_SMC)
        invalidate_translation_cache_ram();
      if ((alert & CPU_ALERT_IRQ) && rv32_check_and_raise_interrupts())
        remaining -= (s32)take_irq_entry_cycles();
#if defined(GPSP_BENCHMARK)
      phase_cycles.scalar += rv32_cycles_since(scalar_start);
#endif
      continue;
    }
    if (raw_pc >= 0x4000 && (region != 0x02 && region != 0x03) &&
        (region < 0x08 || region > 0x0d))
    {
      resume = NULL;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
      guard_pending = false;
#endif
#if defined(GPSP_BENCHMARK)
      interpreted_runs++;
      uint32_t scalar_start = rv32_cycles_now();
#endif
      remaining = execute_arm_until_jit(remaining, &interpreted_steps);
      rv32_note_lookup_resume(GPSP_RV32_LOOKUP_RESUME_OTHER);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
      bios_probe_transition(raw_pc, reg[REG_PC],
          &bios_probe.interpreter_to_bios,
          &bios_probe.interpreter_from_bios);
#endif
#if defined(GPSP_BENCHMARK)
      phase_cycles.scalar += rv32_cycles_since(scalar_start);
#endif
      continue;
    }

    bool thumb = (reg[REG_CPSR] & 0x20) != 0;
    u32 pc = raw_pc & (thumb ? ~1u : ~3u);
    if (raw_pc != pc)
    {
      resume = NULL;
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
      guard_pending = false;
#endif
      /* The interpreter tests the cheat hook before aligning the fetch PC. */
      interpreted_steps++;
#if defined(GPSP_BENCHMARK)
      gpsp_rv32_block_note_direct_step();
      interpreted_runs++;
      uint32_t scalar_start = rv32_cycles_now();
#endif
      remaining = execute_arm_one(remaining);
      rv32_note_lookup_resume(GPSP_RV32_LOOKUP_RESUME_OTHER);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
      bios_probe_transition(raw_pc, reg[REG_PC],
          &bios_probe.interpreter_to_bios,
          &bios_probe.interpreter_from_bios);
#endif
      u32 alert = gpsp_rv32_take_memory_alert();
      if (alert & CPU_ALERT_SMC)
        invalidate_translation_cache_ram();
      if ((alert & CPU_ALERT_IRQ) && rv32_check_and_raise_interrupts())
        remaining -= (s32)take_irq_entry_cycles();
#if defined(GPSP_BENCHMARK)
      phase_cycles.scalar += rv32_cycles_since(scalar_start);
#endif
      continue;
    }
    reg[REG_PC] = pc;
    /* Scheduler work may change the fetch state or invalidate native code. */
    bool use_resume = resume && pc == resume_pc &&
        (reg[REG_CPSR] & 0x3f) == resume_mode &&
        reg[CPU_MODE] == resume_cpu_mode &&
        rv32_rom_cache_generation == resume_rom_generation &&
        rv32_ram_cache_generation == resume_ram_generation;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    if (resume)
    {
      block_exit_stats.resume_attempts++;
      if (use_resume)
        block_exit_stats.resume_successes++;
      else
        block_exit_stats.resume_rejections++;
    }
#endif
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
    if (guard_pending)
    {
      if (!use_resume && pc == guard_pc &&
          (reg[REG_CPSR] & 0x3f) == guard_mode &&
          rv32_rom_cache_flush_count == guard_rom_flushes &&
          rv32_ram_cache_flush_count == guard_ram_flushes)
      {
        gpsp_rv32_block_note_guard_lookup(pc);
        rv32_note_lookup_resume(GPSP_RV32_LOOKUP_RESUME_GUARD);
      }
      guard_pending = false;
    }
#endif
    u8 *block = use_resume ? resume : NULL;
    resume = NULL;
    if (!block)
    {
#if defined(GPSP_BENCHMARK)
      uint32_t lookup_start = rv32_cycles_now();
#endif
      /* Full flushes clear this cache; partial invalidation drops only
         targets in dead units. A hit skips the lookup tables. */
      u32 *entry = &gpsp_rv32_branch_cache[
          ((pc >> 1) & (GPSP_RV32_BRANCH_CACHE_SIZE - 1)) * 2];
      if (entry[0] == (pc | (u32)thumb) && entry[1])
        block = (u8 *)(uintptr_t)entry[1];
      else
      {
        block = thumb ? block_lookup_address_thumb(pc) :
            block_lookup_address_arm(pc);
        if (block && block != (u8 *)(uintptr_t)~0u &&
            (region <= 0x03 || (region >= 0x08 && region <= 0x0d)))
        {
          entry[0] = pc | (u32)thumb;
          entry[1] = (u32)(uintptr_t)block;
        }
      }
#if defined(GPSP_BENCHMARK)
      phase_cycles.lookup += rv32_cycles_since(lookup_start);
#endif
    }
    if (!block || block == (u8 *)(uintptr_t)~0u)
    {
      /* The scanner rejected a block or executable cache publication. */
      gpsp_rv32_log(GPSP_RV32_LOG_WARN, TAG,
          "block lookup failed at %08x; using interpreter", pc);
      block_ready = false;
#if defined(GPSP_BENCHMARK)
      uint32_t scalar_start = rv32_cycles_now();
#endif
      execute_arm((u32)remaining);
#if defined(GPSP_BENCHMARK)
      phase_cycles.scalar += rv32_cycles_since(scalar_start);
      phase_cycles.total += rv32_cycles_since(total_start);
#endif
      return 0;
    }

    s32 prior = remaining;
    uint32_t prior_budget_updates = native_budget_updates;
    executed_blocks++;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    struct gpsp_rv32_hot_site *sampled_site = note_hot_pc(pc, thumb);
    if (pc < 0x4000)
    {
      bios_probe.block_calls++;
      bios_block_sources[pc >> 1]++;
    }
#endif
#if defined(GPSP_BENCHMARK)
    if (pc < 0x4000)
      block_region_stats.bios++;
    else if (region == 0x02)
      block_region_stats.ewram++;
    else if (region == 0x03)
      block_region_stats.iwram++;
    else if (region >= 0x08 && region <= 0x0d)
      block_region_stats.rom++;
    else
      block_region_stats.other++;
    uint32_t block_start = rv32_cycles_now();
    uint32_t budget_start = (uint32_t)gpsp_rv32_native_budget_cycles;
#endif
    uint64_t block_result = gpsp_rv32_run_block((uint32_t *)(void *)reg,
        (u32)remaining, (void (*)(void))(uintptr_t)block);
#if defined(GPSP_BENCHMARK)
    uint32_t block_elapsed = rv32_cycles_since(block_start);
    block_elapsed -= (uint32_t)gpsp_rv32_native_budget_cycles - budget_start;
#endif
    u32 native_return = (u32)(block_result >> 32);
    bool native_dispatch =
        native_return == GPSP_RV32_BUDGET_DISPATCH_RETURN;
    bool native_frame =
        native_return == GPSP_RV32_BUDGET_FRAME_RETURN;
    remaining = (s32)(u32)block_result;
    resume = !native_dispatch && !native_frame && remaining <= 0 ?
        (u8 *)(uintptr_t)(u32)(block_result >> 32) : NULL;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    if (resume)
      block_exit_stats.resume_candidates++;
#endif
    if (resume)
    {
      resume_pc = reg[REG_PC];
      resume_mode = reg[REG_CPSR] & 0x3f;
      resume_cpu_mode = reg[CPU_MODE];
      resume_rom_generation = rv32_rom_cache_generation;
      resume_ram_generation = rv32_ram_cache_generation;
    }
#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
    guard_pending = resume != NULL;
    if (guard_pending)
    {
      guard_pc = reg[REG_PC];
      guard_mode = reg[REG_CPSR] & 0x3f;
      guard_rom_flushes = rv32_rom_cache_flush_count;
      guard_ram_flushes = rv32_ram_cache_flush_count;
      gpsp_rv32_block_note_guard_exit(pc);
    }
#endif
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    u32 next_pc = reg[REG_PC];
    bool ready = remaining > 0 && reg[CPU_HALT_STATE] == CPU_ACTIVE;
    if (pc < 0x4000)
    {
      if (next_pc < 0x4000)
      {
        bios_probe.block_bios_to_bios++;
        if (ready)
          bios_probe.block_bios_to_bios_ready++;
      }
      else
      {
        bios_probe.block_bios_to_other++;
        if (ready)
          bios_probe.block_bios_to_other_ready++;
      }
    }
    else if (next_pc < 0x4000)
      bios_probe.block_to_bios++;
#endif
#if defined(GPSP_BENCHMARK)
    phase_cycles.block += block_elapsed;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    if (sampled_site)
      sampled_site->cycles += block_elapsed;
    if (pc < 0x4000)
      block_region_cycles.bios += block_elapsed;
    else if (region == 0x02)
      block_region_cycles.ewram += block_elapsed;
    else if (region == 0x03)
      block_region_cycles.iwram += block_elapsed;
    else if (region >= 0x08 && region <= 0x0d)
      block_region_cycles.rom += block_elapsed;
    else
      block_region_cycles.other += block_elapsed;
#endif
#endif
    if (native_frame)
    {
      frame_calls++;
#if !defined(GPSP_BENCHMARK)
      if (frame_calls == 1 || frame_calls % 60 == 0)
        gpsp_rv32_log(GPSP_RV32_LOG_INFO, TAG,
            "JIT stats: frames=%u blocks=%llu scalar=%llu "
            "code=%u", (unsigned)frame_calls,
            (unsigned long long)executed_blocks,
            (unsigned long long)interpreted_steps,
            (unsigned)gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena,
                rom_translation_ptr));
#endif
#if defined(GPSP_BENCHMARK)
      phase_cycles.total += rv32_cycles_since(total_start);
#endif
      return (u32)block_result;
    }
    u32 alert = gpsp_rv32_take_memory_alert() |
        gpsp_rv32_arm_system_take_alert();
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    if (!native_dispatch && remaining > 0 &&
        reg[CPU_HALT_STATE] == CPU_ACTIVE && !alert)
      block_exit_stats.block_ready_exits++;
    if (!native_dispatch && remaining <= 0)
      block_exit_stats.block_budget_exits++;
    if (!native_dispatch && reg[CPU_HALT_STATE] != CPU_ACTIVE)
      block_exit_stats.block_halt_exits++;
    if (alert & CPU_ALERT_IRQ)
      block_exit_stats.block_irq_alerts++;
    if (alert & CPU_ALERT_SMC)
      block_exit_stats.block_smc_alerts++;
#endif
    if (alert)
      resume = NULL;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    if (pc < 0x4000 && ready && !alert)
    {
      if (next_pc == 0x7a4)
      {
        bios_probe.fastset_outer_from_7a8_ready += pc == 0x7a8;
        bios_probe.fastset_outer_from_7b8_ready += pc == 0x7b8;
      }
      bios_probe.fastset_inner_from_7b8_ready +=
          pc == 0x7b8 && next_pc == 0x7a8;
      bios_probe.swi_entry_to_64_ready +=
          pc == 8 && next_pc == 0x64;
    }
#endif
    if (!native_dispatch)
      rv32_note_lookup_resume((alert & CPU_ALERT_SMC) ?
          GPSP_RV32_LOOKUP_RESUME_SMC : (alert & CPU_ALERT_IRQ) ?
          GPSP_RV32_LOOKUP_RESUME_IRQ :
          (remaining > 0 && reg[CPU_HALT_STATE] == CPU_ACTIVE) ?
          GPSP_RV32_LOOKUP_RESUME_READY : GPSP_RV32_LOOKUP_RESUME_OTHER);
    if (alert & CPU_ALERT_SMC)
      invalidate_translation_cache_ram();
    if (alert & CPU_ALERT_IRQ)
    {
      if (rv32_check_and_raise_interrupts())
        remaining -= (s32)take_irq_entry_cycles();
      continue;
    }
    if (remaining == prior && reg[REG_PC] == pc
        && native_budget_updates == prior_budget_updates
        )
    {
      gpsp_rv32_log(GPSP_RV32_LOG_WARN, TAG,
          "block made no progress at %08x; using interpreter", pc);
      block_ready = false;
#if defined(GPSP_BENCHMARK)
      uint32_t scalar_start = rv32_cycles_now();
#endif
      execute_arm((u32)remaining);
#if defined(GPSP_BENCHMARK)
      phase_cycles.scalar += rv32_cycles_since(scalar_start);
      phase_cycles.total += rv32_cycles_since(total_start);
#endif
      return 0;
    }
  }
}

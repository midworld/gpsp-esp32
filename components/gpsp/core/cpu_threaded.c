/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// Not-so-important todo:
// - stm reglist writeback when base is in the list needs adjustment
// - block memory needs psr swapping and user mode reg swapping

#include "common.h"
#include "memmap.h"
#include "gpsp_frame_stats.h"
#if defined(VITA)
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#elif defined(PS2)
#include <kernel.h>
#endif

u8 *last_rom_translation_ptr = NULL;
u8 *last_ram_translation_ptr = NULL;

#if defined(GPSP_RV32_JIT)
u8 *rom_translation_ptr = NULL;
u8 *ram_translation_ptr = NULL;
u32 rv32_rom_cache_generation;
/* Direct-mapped (key, native entry) pairs for ROM, BIOS and RAM targets.
   Shared exits probe it. Empty entries enter a return stub, so an
   unaligned BX target that matches the empty key stays safe. */
u32 gpsp_rv32_branch_cache[GPSP_RV32_BRANCH_CACHE_SIZE * 2]
    __attribute__((aligned(8)));
extern void gpsp_rv32_branch_cache_miss(void);

static void rv32_branch_cache_clear(void)
{
  u32 miss = (u32)(uintptr_t)gpsp_rv32_branch_cache_miss;
  for (u32 i = 0; i < GPSP_RV32_BRANCH_CACHE_SIZE; i++)
  {
    gpsp_rv32_branch_cache[i * 2] = ~0u;
    gpsp_rv32_branch_cache[i * 2 + 1] = miss;
  }
}

u32 rv32_ram_cache_generation;
#if defined(GPSP_BENCHMARK)
u32 rv32_rom_cache_flush_count;
u32 rv32_ram_cache_flush_count;
#endif
#elif defined(MMAP_JIT_CACHE)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
#elif defined(VITA)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
int sceBlock;
#elif defined(_3DS)
u8* rom_translation_cache_ptr;
u8* ram_translation_cache_ptr;
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#else
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#endif
/* Note, see stub files for more cache definitions */

u32 iwram_code_min = ~0U;
u32 iwram_code_max =  0U;
u32 ewram_code_min = ~0U;
u32 ewram_code_max =  0U;

#define INITIAL_ROM_WATERMARK   16   // To avoid NULL aliasing
#if defined(GPSP_RV32_JIT)
#define INITIAL_RAM_WATERMARK   16   // Offset zero means untranslated.
#else
#define INITIAL_RAM_WATERMARK   0
#endif
u32 rom_cache_watermark = INITIAL_ROM_WATERMARK;

u8 *bios_swi_entrypoint = NULL;

// Contains an offset table to rom_translation cache area
// It features a chaining linked list for collisions
// The rom area has a small header section that contains:
//  - PC value for the entry
//  - Offset to the next entry (if any)
typedef struct
{
  u32 pc_value;
  u32 next_entry;
} hashhdr_type;

u32 rom_branch_hash[ROM_BRANCH_HASH_SIZE];

typedef struct
{
  u8 *block_offset;
  u16 flag_data;
  u8 condition;
  u8 update_cycles;
} block_data_type;

typedef struct
{
  u32 branch_target;
  u8 *branch_source;
} block_exit_type;

typedef enum
{
  TRANSLATION_FAILURE_NONE,
  TRANSLATION_FAILURE_CACHE_FULL,
  TRANSLATION_FAILURE_UNSUPPORTED,
  TRANSLATION_FAILURE_OUT_OF_MEMORY,
  TRANSLATION_FAILURE_LOOKUP,
  /* Native state was left half published; flush and translate again. */
  TRANSLATION_FAILURE_RETRY
} translation_failure_type;

static translation_failure_type translation_failure = TRANSLATION_FAILURE_NONE;

// Div (6) and DivArm (7)
#define is_div_swi(swinum) (((swinum) & 0xFE) == 0x06)

#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_psr_imm(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F                                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F)                       \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF                                                \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF                                              \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)((u32)(opcode << 8))) >> 6                               \

#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF                                                     \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07)                           \

#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F                                                     \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF                                                \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \

/* Include the right emitter headers */
#if defined(MIPS_ARCH)
  #include "mips/mips_emit.h"
#elif defined(ARM_ARCH)
  #include "arm/arm_emit.h"
#elif defined(ARM64_ARCH)
  #include "arm/arm64_emit.h"
#elif defined(GPSP_RV32_JIT)
  #include "riscv/rv32_block_emit.h"
  #include "riscv/rv32_block_link.h"
  #include "riscv/rv32_block_runtime.h"
  #include "riscv/rv32_entry_pack.h"
  #include "riscv/rv32_ram_units.h"
  #include "gpsp_rv32_platform.h"
  #include "riscv/rv32_stub.h"
  #include "riscv/rv32_thumb_alu.h"
  #include "riscv/rv32_arm_flow_alu.h"
  #include "riscv/rv32_arm_flow_full.h"
  #include "riscv/rv32_thumb_flow_full.h"
  #include "riscv/rv32_flow_timing.h"
  #include "riscv/rv32_register_map.h"
#elif defined(__riscv)
  #error "RISC-V dynarec requires a RISC-V emitter and execution stub"
#else
  #include "x86/x86_emit.h"
#endif

#if defined(GPSP_RV32_JIT)
#define RV32_ROM_CACHE_THUMB_BLOCK_LIMIT 32
#endif

#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
static struct gpsp_rv32_compile_stats rv32_compile_stats;
static struct gpsp_rv32_site_stats rv32_site_stats;
static struct gpsp_rv32_site_stats rv32_pending_site_stats;

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
static struct gpsp_rv32_rom_byte_stats rv32_rom_byte_stats;

void gpsp_rv32_block_get_rom_byte_stats(
    struct gpsp_rv32_rom_byte_stats *stats)
{
  *stats = rv32_rom_byte_stats;
}

static void rv32_record_rom_emitted_bytes(
    const struct gpsp_rv32_emit_byte_stats *before, size_t code_bytes)
{
  struct gpsp_rv32_emit_byte_stats after;
  gpsp_rv32_emit_get_byte_stats(&after);
  u64 guard = after.guard - before->guard;
  u64 body = after.body - before->body;
  u64 timing = after.timing - before->timing;
  u64 preflight = after.batch_preflight - before->batch_preflight;
  u64 slow = after.batch_slow - before->batch_slow;
  u64 link_exit = after.link_exit - before->link_exit;
  u64 emitted = guard + body + timing + preflight + slow + link_exit;

  rv32_rom_byte_stats.total += code_bytes;
  rv32_rom_byte_stats.guard += guard;
  rv32_rom_byte_stats.body += body;
  rv32_rom_byte_stats.timing += timing;
  rv32_rom_byte_stats.batch_preflight += preflight;
  rv32_rom_byte_stats.batch_slow += slow;
  rv32_rom_byte_stats.link_exit += link_exit;
  if (emitted <= code_bytes)
    rv32_rom_byte_stats.residual += code_bytes - emitted;
  else
    rv32_rom_byte_stats.overcount_blocks++;
}
#endif

void gpsp_rv32_block_get_site_stats(struct gpsp_rv32_site_stats *sites)
{
  *sites = rv32_site_stats;
}

static void rv32_count_thumb_site(struct gpsp_rv32_site_stats *sites,
    u16 opcode, bool allow_native)
{
  sites->thumb_ops++;
  if ((opcode & 0xfe00) == 0xb400)
    sites->thumb_push++;
  else if ((opcode & 0xfe00) == 0xbc00)
    sites->thumb_pop++;
  else if ((opcode & 0xf800) == 0xc000)
    sites->thumb_stmia++;
  else if ((opcode & 0xf800) == 0xc800)
    sites->thumb_ldmia++;
  if (!allow_native)
  {
    if (!((opcode & 0xf800) == 0x4800 ||
          (opcode >= 0x5000 && opcode < 0xa000) ||
          (opcode & 0xfe00) == 0xb400 ||
          (opcode & 0xfe00) == 0xbc00 ||
          (opcode & 0xf000) == 0xc000))
      sites->thumb_nonmem_fallbacks++;
    return;
  }

  if ((opcode & 0xf800) == 0x4800)
    sites->thumb_literals++;
  else if ((opcode & 0xf000) == 0x5000)
  {
    if (((opcode >> 9) & 7) < 3)
      sites->thumb_stores++;
    else
      sites->thumb_loads++;
  }
  else if ((opcode & 0xe000) == 0x6000)
  {
    if ((opcode & 0x0800) == 0)
      sites->thumb_stores++;
    else
      sites->thumb_loads++;
  }
  else if ((opcode & 0xe000) == 0x8000)
  {
    if ((opcode & 0x0800) == 0)
      sites->thumb_stores++;
    else
      sites->thumb_loads++;
  }
  else if (gpsp_rv32_thumb_needs_helper(opcode) &&
      (opcode & 0xfe00) != 0xb400 &&
      (opcode & 0xfe00) != 0xbc00 &&
      (opcode & 0xf000) != 0xc000)
    sites->thumb_nonmem_fallbacks++;
}

static void rv32_record_site_stats(const struct gpsp_rv32_site_stats *sites)
{
  rv32_site_stats.arm_ops += sites->arm_ops;
  rv32_site_stats.thumb_ops += sites->thumb_ops;
  rv32_site_stats.thumb_loads += sites->thumb_loads;
  rv32_site_stats.thumb_stores += sites->thumb_stores;
  rv32_site_stats.thumb_literals += sites->thumb_literals;
  rv32_site_stats.thumb_nonmem_fallbacks += sites->thumb_nonmem_fallbacks;
  rv32_site_stats.thumb_push += sites->thumb_push;
  rv32_site_stats.thumb_pop += sites->thumb_pop;
  rv32_site_stats.thumb_stmia += sites->thumb_stmia;
  rv32_site_stats.thumb_ldmia += sites->thumb_ldmia;
}

void gpsp_rv32_block_get_compile_stats(struct gpsp_rv32_compile_stats *stats)
{
  *stats = rv32_compile_stats;
}

static void rv32_record_rom_block(unsigned guest_ops, size_t code_bytes,
    bool thumb, const u32 *span_count, unsigned bundled_guest_ops)
{
  rv32_compile_stats.blocks++;
  rv32_compile_stats.guest_ops += guest_ops;
  rv32_compile_stats.code_bytes += code_bytes;
  rv32_compile_stats.bundled_guest_ops += bundled_guest_ops;
  if (guest_ops > rv32_compile_stats.max_guest_ops)
    rv32_compile_stats.max_guest_ops = guest_ops;
  if (thumb && guest_ops > rv32_compile_stats.max_thumb_guest_ops)
    rv32_compile_stats.max_thumb_guest_ops = guest_ops;
  if (code_bytes > rv32_compile_stats.max_block_bytes)
    rv32_compile_stats.max_block_bytes = code_bytes;
  if (span_count)
  {
    for (unsigned i = 0; i < 4; i++)
      rv32_compile_stats.span_count[i] += span_count[i];
  }
}
#define rv32_benchmark_arm_site() (rv32_block_sites.arm_ops++)
#define rv32_benchmark_thumb_site(opcode) \
  rv32_count_thumb_site(&rv32_block_sites, (opcode), pc != cheat_master_hook)
#define rv32_benchmark_publish_sites(result) \
  do { if ((result) && rv32_rom_pc(pc)) \
    rv32_record_site_stats(&rv32_pending_site_stats); } while(0)
#else
#define rv32_benchmark_arm_site() ((void)0)
#define rv32_benchmark_thumb_site(opcode) ((void)0)
#define rv32_benchmark_publish_sites(result) ((void)0)
#endif

#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK_JIT_LOOKUP)
static struct gpsp_rv32_lookup_stats rv32_lookup_stats;
static uint64_t (*rv32_lookup_now)(void);
static u32 rv32_lookup_cycle_now(void);

#if defined(GPSP_BENCHMARK_JIT_MISS_CLASS)
typedef struct
{
  u32 start;
  u32 end;
  u8 thumb;
  u8 region;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  u32 safe_mask[(RV32_ROM_CACHE_THUMB_BLOCK_LIMIT + 31) / 32];
#endif
} rv32_miss_range_type;

static rv32_miss_range_type *rv32_miss_ranges;
static size_t rv32_miss_range_count;
static size_t rv32_miss_range_capacity;
static bool rv32_miss_ranges_complete;
static enum gpsp_rv32_lookup_resume_reason rv32_lookup_resume;

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
static struct gpsp_rv32_overlap_byte_stats rv32_overlap_byte_stats;

typedef enum
{
  RV32_OVERLAP_NEW,
  RV32_OVERLAP_REPEATED,
  RV32_OVERLAP_SAFE,
  RV32_OVERLAP_BATCH,
  RV32_OVERLAP_UNKNOWN,
  RV32_OVERLAP_UNAVAILABLE
} rv32_overlap_kind;
#endif

void gpsp_rv32_block_set_lookup_resume(
    enum gpsp_rv32_lookup_resume_reason reason)
{
  rv32_lookup_resume = reason;
}

void gpsp_rv32_block_note_guard_exit(uint32_t pc)
{
  rv32_lookup_stats.guard_exits++;
  rv32_lookup_stats.guard_exits_bios += pc < 0x4000;
}

void gpsp_rv32_block_note_guard_lookup(uint32_t pc)
{
  rv32_lookup_stats.guard_lookups++;
  rv32_lookup_stats.guard_lookups_bios += pc < 0x4000;
}

static u8 rv32_miss_region(u32 pc)
{
  u8 region = pc >> 24;
  return region == 0 ? 0 : (region == 2 || region == 3) ? 2 : 1;
}

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
static rv32_overlap_kind rv32_overlap_classify_pc(u32 pc, u8 thumb)
{
  if (!rv32_miss_ranges_complete)
    return RV32_OVERLAP_UNAVAILABLE;

  u8 region = rv32_miss_region(pc);
  rv32_overlap_kind kind = RV32_OVERLAP_NEW;
  for (size_t i = rv32_miss_range_count; i; i--)
  {
    const rv32_miss_range_type *range = &rv32_miss_ranges[i - 1];
    if (range->region != region || range->thumb != thumb)
      continue;
    if (range->start == pc)
      return RV32_OVERLAP_REPEATED;
    if (range->start >= pc || pc >= range->end)
      continue;
    if (!thumb)
    {
      kind = RV32_OVERLAP_SAFE;
      continue;
    }
    /* Only ROM Thumb ranges carry a safe-entry mask. */
    if (region != 1)
    {
      kind = RV32_OVERLAP_UNKNOWN;
      continue;
    }
    u32 index = (pc - range->start) / 2;
    if (index < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT &&
        (range->safe_mask[index >> 5] & (1U << (index & 31))))
      kind = RV32_OVERLAP_SAFE;
    else if (index < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT &&
        kind != RV32_OVERLAP_SAFE)
      kind = RV32_OVERLAP_BATCH;
    else if (kind == RV32_OVERLAP_NEW)
      kind = RV32_OVERLAP_UNKNOWN;
  }
  return kind;
}

static void rv32_record_rom_overlap(u32 pc, u8 thumb, size_t code_bytes)
{
  u32 classify_start = rv32_lookup_cycle_now();
  switch (rv32_overlap_classify_pc(pc, thumb))
  {
    case RV32_OVERLAP_NEW:
      rv32_overlap_byte_stats.new_blocks++;
      rv32_overlap_byte_stats.new_bytes += code_bytes;
      break;
    case RV32_OVERLAP_REPEATED:
      rv32_overlap_byte_stats.repeated_blocks++;
      rv32_overlap_byte_stats.repeated_bytes += code_bytes;
      break;
    case RV32_OVERLAP_SAFE:
      rv32_overlap_byte_stats.safe_blocks++;
      rv32_overlap_byte_stats.safe_bytes += code_bytes;
      break;
    case RV32_OVERLAP_BATCH:
      rv32_overlap_byte_stats.batch_blocks++;
      rv32_overlap_byte_stats.batch_bytes += code_bytes;
      break;
    case RV32_OVERLAP_UNKNOWN:
    case RV32_OVERLAP_UNAVAILABLE:
      rv32_overlap_byte_stats.unknown_blocks++;
      rv32_overlap_byte_stats.unknown_bytes += code_bytes;
      break;
  }
  rv32_lookup_stats.overlap_classify_cycles +=
      rv32_lookup_cycle_now() - classify_start;
}
#endif

static void rv32_miss_classify(
    struct gpsp_rv32_lookup_region_stats *stats, u32 pc, u8 thumb)
{
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  switch (rv32_overlap_classify_pc(pc, thumb))
  {
    case RV32_OVERLAP_NEW:
      stats->new_misses++;
      break;
    case RV32_OVERLAP_REPEATED:
      stats->repeated_misses++;
      break;
    case RV32_OVERLAP_SAFE:
      stats->overlap_safe_misses++;
      stats->overlap_misses++;
      stats->overlap_resume[rv32_lookup_resume]++;
      break;
    case RV32_OVERLAP_BATCH:
      stats->overlap_batch_misses++;
      stats->overlap_misses++;
      stats->overlap_resume[rv32_lookup_resume]++;
      break;
    case RV32_OVERLAP_UNKNOWN:
      stats->overlap_unknown_misses++;
      stats->overlap_misses++;
      stats->overlap_resume[rv32_lookup_resume]++;
      break;
    case RV32_OVERLAP_UNAVAILABLE:
      stats->unknown_misses++;
      break;
  }
#else
  if (!rv32_miss_ranges_complete)
  {
    stats->unknown_misses++;
    return;
  }

  u8 region = rv32_miss_region(pc);
  bool overlap = false;
  for (size_t i = rv32_miss_range_count; i; i--)
  {
    const rv32_miss_range_type *range = &rv32_miss_ranges[i - 1];
    if (range->region != region || range->thumb != thumb)
      continue;
    if (range->start == pc)
    {
      stats->repeated_misses++;
      return;
    }
    if (range->start < pc && pc < range->end)
      overlap = true;
  }
  if (overlap)
  {
    stats->overlap_misses++;
    stats->overlap_resume[rv32_lookup_resume]++;
  }
  else
    stats->new_misses++;
#endif
}

static void rv32_miss_record_range(u32 start, u32 end, u8 thumb,
    const u32 *safe_mask)
{
#if !defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  (void)safe_mask;
#endif
  if (!rv32_miss_ranges_complete || end <= start)
    return;
  if (rv32_miss_range_count == rv32_miss_range_capacity)
  {
    size_t capacity = rv32_miss_range_capacity ?
        rv32_miss_range_capacity * 2 : 256;
    rv32_miss_range_type *ranges = realloc(rv32_miss_ranges,
        capacity * sizeof(*ranges));
    if (!ranges)
    {
      rv32_miss_ranges_complete = false;
      return;
    }
    rv32_miss_ranges = ranges;
    rv32_miss_range_capacity = capacity;
  }
  rv32_miss_range_type *range =
      &rv32_miss_ranges[rv32_miss_range_count++];
  *range = (rv32_miss_range_type){
    .start = start, .end = end, .thumb = thumb,
    .region = rv32_miss_region(start)
  };
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  if (safe_mask)
    memcpy(range->safe_mask, safe_mask, sizeof(range->safe_mask));
#endif
}

static void rv32_miss_clear_ranges(u8 region)
{
  size_t kept = 0;
  for (size_t i = 0; i < rv32_miss_range_count; i++)
  {
    const rv32_miss_range_type *range = &rv32_miss_ranges[i];
    if (region == 2 ? range->region != 2 :
        range->region == 2 || (range->region == 0 && range->start == 8 &&
        bios_swi_entrypoint))
      rv32_miss_ranges[kept++] = *range;
  }
  rv32_miss_range_count = kept;
}

#define rv32_lookup_classify_miss() do { \
  u32 classify_start = rv32_lookup_cycle_now(); \
  rv32_miss_classify(lookup_region, pc, thumb); \
  rv32_lookup_stats.miss_classify_cycles += \
      rv32_lookup_cycle_now() - classify_start; \
} while(0)
#define rv32_lookup_record_range(start, end, thumb, safe_mask) \
  rv32_miss_record_range(start, end, thumb, safe_mask)
#define rv32_lookup_clear_ram_ranges() rv32_miss_clear_ranges(2)
#define rv32_lookup_clear_rom_ranges() rv32_miss_clear_ranges(1)
#define rv32_lookup_init_ranges() do { \
  free(rv32_miss_ranges); \
  rv32_miss_ranges = NULL; \
  rv32_miss_range_count = 0; \
  rv32_miss_range_capacity = 0; \
  rv32_miss_ranges_complete = true; \
  rv32_lookup_resume = GPSP_RV32_LOOKUP_RESUME_INITIAL; \
} while(0)
#else
#define rv32_lookup_classify_miss() ((void)0)
#define rv32_lookup_record_range(start, end, thumb, safe_mask) ((void)0)
#define rv32_lookup_clear_ram_ranges() ((void)0)
#define rv32_lookup_clear_rom_ranges() ((void)0)
#define rv32_lookup_init_ranges() ((void)0)
#endif

void gpsp_rv32_block_get_lookup_stats(struct gpsp_rv32_lookup_stats *stats)
{
  *stats = rv32_lookup_stats;
}

void gpsp_rv32_block_set_lookup_clock(uint64_t (*now)(void))
{
  rv32_lookup_now = now;
}

static struct gpsp_rv32_lookup_region_stats *rv32_lookup_region(u8 region)
{
  if (region == 0)
    return &rv32_lookup_stats.bios;
  if (region == 2 || region == 3)
    return &rv32_lookup_stats.ram;
  return &rv32_lookup_stats.rom;
}

static u32 rv32_lookup_cycle_now(void)
{
  return rv32_lookup_now ? (u32)rv32_lookup_now() : 0;
}

static u64 rv32_lookup_misses(void)
{
  return rv32_lookup_stats.bios.misses + rv32_lookup_stats.rom.misses +
      rv32_lookup_stats.ram.misses;
}

static void rv32_lookup_note_translation(
    struct gpsp_rv32_lookup_region_stats *region, u32 start, bool result)
{
  region->translate_calls++;
  region->translate_failures += !result;
  if (rv32_lookup_now)
    region->translate_cycles += rv32_lookup_cycle_now() - start;
}

static void rv32_lookup_note_top(u64 misses, u32 start)
{
  /* A deferred target miss makes the whole outer lookup a miss. */
  u32 cycles = rv32_lookup_now ? rv32_lookup_cycle_now() - start : 0;
  if (rv32_lookup_misses() == misses)
  {
    rv32_lookup_stats.top_hit_calls++;
    rv32_lookup_stats.top_hit_cycles += cycles;
  }
  else
  {
    rv32_lookup_stats.top_miss_calls++;
    rv32_lookup_stats.top_miss_cycles += cycles;
  }
}

#define rv32_lookup_region_var(region) \
  struct gpsp_rv32_lookup_region_stats *lookup_region = \
      rv32_lookup_region(region)
#define rv32_lookup_hit() (lookup_region->hits++)
#define rv32_lookup_miss() do { \
  lookup_region->misses++; \
  rv32_lookup_classify_miss(); \
} while(0)
#define rv32_lookup_hash_probe() (lookup_region->hash_probes++)
#define rv32_lookup_translate_begin() u32 lookup_translate_start = rv32_lookup_cycle_now()
#define rv32_lookup_translate_end(result) \
  rv32_lookup_note_translation(lookup_region, lookup_translate_start, result)
#define rv32_lookup_top_begin() \
  u64 lookup_top_misses = rv32_lookup_misses(); \
  u32 lookup_top_start = rv32_lookup_cycle_now()
#define rv32_lookup_top_end() \
  rv32_lookup_note_top(lookup_top_misses, lookup_top_start)
#define rv32_lookup_rom_capacity_flush() (rv32_lookup_stats.rom_capacity_flushes++)
#define rv32_lookup_ram_capacity_flush() (rv32_lookup_stats.ram_capacity_flushes++)
#define rv32_phase_start() rv32_lookup_cycle_now()
#define rv32_phase_add(field, start) \
  (rv32_lookup_stats.field += rv32_lookup_cycle_now() - (start))
#define rv32_phase_count(field) (rv32_lookup_stats.field++)
#else
#define rv32_phase_start() 0
#define rv32_phase_add(field, start) ((void)(start))
#define rv32_phase_count(field) ((void)0)
#define rv32_lookup_region_var(region) ((void)0)
#define rv32_lookup_hit() ((void)0)
#define rv32_lookup_miss() ((void)0)
#define rv32_lookup_hash_probe() ((void)0)
#if defined(GPSP_RV32_JIT) && defined(__riscv)
/* Always counted: the frontend logs translation work in slow frames. Only
   the outermost translation adds cycles; nested ones run inside it. */
static u32 rv32_translations;
static u32 rv32_translate_cycles;
static inline u32 rv32_cycle_count(void)
{
  u32 cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}
#define rv32_lookup_translate_begin() \
  u32 lookup_translate_start = rv32_cycle_count()
#define rv32_lookup_translate_end(result) do { \
  rv32_translations++; \
  if (translate_depth == 1) \
    rv32_translate_cycles += rv32_cycle_count() - lookup_translate_start; \
} while (0)
#else
#define rv32_lookup_translate_begin() ((void)0)
#define rv32_lookup_translate_end(result) ((void)0)
#endif
#define rv32_lookup_top_begin() ((void)0)
#define rv32_lookup_top_end() ((void)0)
#define rv32_lookup_rom_capacity_flush() ((void)0)
#define rv32_lookup_ram_capacity_flush() ((void)0)
#define rv32_lookup_record_range(start, end, thumb, safe_mask) ((void)0)
#define rv32_lookup_clear_ram_ranges() ((void)0)
#define rv32_lookup_clear_rom_ranges() ((void)0)
#define rv32_lookup_init_ranges() ((void)0)
#endif

/* Cache invalidation */

#if defined(PSP)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelDcacheWritebackRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
    sceKernelIcacheInvalidateRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
  }
#elif defined(PS2)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    FlushCache(0);   // Dcache flush
    FlushCache(2);   // Icache invalidate
  }
#elif defined(VITA)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelSyncVMDomain(sceBlock, baseaddr, ((char*)endptr) - ((char*)baseaddr) + 64);
  }
#elif defined(_3DS)
  #include "3ds/3ds_utils.h"
  void platform_cache_sync(void *baseaddr, void *endptr) {
    ctr_flush_invalidate_cache();
  }
#elif defined(ARM_ARCH) || defined(ARM64_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __clear_cache(baseaddr, endptr);
  }
#elif defined(MIPS_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __builtin___clear_cache(baseaddr, endptr);
  }
#elif defined(GPSP_RV32_JIT)
  static bool rv32_cache_sync_ok = true;
  void platform_cache_sync(void *baseaddr, void *endptr) {
    if (!gpsp_rv32_block_sync(baseaddr, endptr))
      rv32_cache_sync_ok = false;
  }
#else
  /* x86 CPUs have icache consistency checks */
  void platform_cache_sync(void *baseaddr, void *endptr) {}
#endif

void translate_icache_sync() {
    // Cache emitted code can only grow
    if (last_rom_translation_ptr < rom_translation_ptr) {
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
        last_rom_translation_ptr = rom_translation_ptr;
    }
    if (last_ram_translation_ptr < ram_translation_ptr) {
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
}

/* End of Cache invalidation */

#if defined(GPSP_RV32_JIT)
static inline u8 *rv32_rom_at(u32 offset)
{
  return gpsp_rv32_arena_at(&gpsp_rv32_rom_arena, offset);
}

static inline u8 *rv32_ram_at(u32 offset)
{
  return gpsp_rv32_arena_at(&gpsp_rv32_ram_arena, offset);
}

static inline u32 rv32_rom_offset(const u8 *code)
{
  return gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena, code);
}

static inline u32 rv32_ram_offset(const u8 *code)
{
  return gpsp_rv32_arena_offset(&gpsp_rv32_ram_arena, code);
}

/* Highest translation_ptr for one more instruction in the current chunk. */
static inline u8 *rv32_rom_limit(void)
{
  return gpsp_rv32_arena_end(&gpsp_rv32_rom_arena) -
      TRANSLATION_CACHE_LIMIT_THRESHOLD;
}

static inline u8 *rv32_ram_limit(void)
{
  return gpsp_rv32_arena_end(&gpsp_rv32_ram_arena) -
      TRANSLATION_CACHE_LIMIT_THRESHOLD;
}

/* Room left for a new block. A block that reaches the chunk limit is
   translated again in the next chunk. */
#define RV32_BLOCK_RESERVE (TRANSLATION_CACHE_LIMIT_THRESHOLD + 1024)

#if defined(GPSP_BENCHMARK)
u32 rv32_chunk_retries;
u64 rv32_chunk_tail_bytes;
#endif

/* Use byte gaps only where execution cannot fall through. */
static inline u8 *rv32_skip_to_word(u8 *ptr)
{
  return (u8 *)(((uintptr_t)ptr + 3) & ~(uintptr_t)3);
}

/* Executed padding must be a c.nop allowed by the code policy. */
static bool rv32_pad_to_word(u8 **ptr, u8 *limit)
{
  if (*ptr > limit)
    return false;
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, *ptr, (size_t)(limit - *ptr));
  if (!gpsp_rv32_align4(&code))
    return false;
  *ptr = code.ptr;
  return true;
}

/* Moves to the next chunk, adding one if needed. The old chunk is synced
   first: translate_icache_sync() covers one chunk. */
static bool rv32_arena_next(gpsp_rv32_arena *arena, u8 **ptr, u8 **last)
{
  if (arena->current + 1 >= arena->count && !gpsp_rv32_arena_grow(arena))
    return false;
  if (*last < *ptr)
    platform_cache_sync(*last, *ptr);
#if defined(GPSP_BENCHMARK)
  rv32_chunk_tail_bytes += (u64)(gpsp_rv32_arena_end(arena) - *ptr);
#endif
  arena->current++;
  *ptr = *last = arena->base[arena->current];
  return true;
}

static void rv32_arena_reserve(gpsp_rv32_arena *arena, u8 **ptr, u8 **last)
{
  *ptr = rv32_skip_to_word(*ptr);
  if (gpsp_rv32_arena_end(arena) - *ptr < RV32_BLOCK_RESERVE)
    rv32_arena_next(arena, ptr, last);
}

/* Moves a block that reached the limit of a used chunk to the next chunk.
   A failed translation publishes nothing, so its output is left unused.
   head is the first free offset of chunk 0. */
static bool rv32_arena_retry(gpsp_rv32_arena *arena, u8 **ptr, u8 **last,
    u8 *start, u32 head)
{
  u8 *first = arena->base[arena->current] + (arena->current ? 0 : head);
  if (translation_failure != TRANSLATION_FAILURE_CACHE_FULL ||
      start <= first)
    return false;
  u8 *failed = *ptr;
  *ptr = start;
  if (!rv32_arena_next(arena, ptr, last))
  {
    *ptr = failed;
    return false;
  }
#if defined(GPSP_BENCHMARK)
  rv32_chunk_retries++;
#endif
#if defined(GPSP_RV32_TEST_RAM_SMC)
  if (arena == &gpsp_rv32_ram_arena)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram retry: start=%u free=%u",
        (unsigned)gpsp_rv32_arena_offset(arena, start),
        (unsigned)(arena->base[arena->current - 1] + GPSP_RV32_CHUNK_BYTES -
            start));
#endif
  return true;
}

#define rv32_reserve_rom() \
  rv32_arena_reserve(&gpsp_rv32_rom_arena, &rom_translation_ptr, \
      &last_rom_translation_ptr)
#define rv32_reserve_ram() \
  rv32_arena_reserve(&gpsp_rv32_ram_arena, &ram_translation_ptr, \
      &last_ram_translation_ptr)
#define rv32_retry_rom(result, start) \
  (!(result) && rv32_arena_retry(&gpsp_rv32_rom_arena, &rom_translation_ptr, \
      &last_rom_translation_ptr, (u8 *)(start), rom_cache_watermark))
#define rv32_retry_ram(result, start) \
  (!(result) && rv32_arena_retry(&gpsp_rv32_ram_arena, &ram_translation_ptr, \
      &last_ram_translation_ptr, (start), INITIAL_RAM_WATERMARK))
#else
#define rv32_rom_at(offset) (rom_translation_cache + (offset))
#define rv32_ram_at(offset) (ram_translation_cache + (offset))
#define rv32_rom_offset(code) ((u32)((code) - rom_translation_cache))
#define rv32_ram_offset(code) ((u32)((code) - ram_translation_cache))
#define rv32_rom_limit() (rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE \
    - TRANSLATION_CACHE_LIMIT_THRESHOLD)
#define rv32_reserve_rom() ((void)0)
#define rv32_reserve_ram() ((void)0)
#define rv32_retry_rom(result, start) false
#define rv32_retry_ram(result, start) false
#endif


/* A ROM page load during translation can evict or remap the current page,
   so the map entry is checked again even within the same region. */
#define check_pc_region(pc)                                                   \
  new_pc_region = (pc >> 15);                                                 \
  if(new_pc_region != pc_region ||                                            \
     memory_map_read[new_pc_region] != pc_address_block)                      \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    pc_address_block = memory_map_read[new_pc_region];                        \
                                                                              \
    if(!pc_address_block)                                                     \
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);                \
  }                                                                           \

#define translate_arm_instruction()                                           \
  check_pc_region(pc);                                                        \
  opcode = readaddress32(pc_address_block, (pc & 0x7FFF));                    \
  condition = block_data[block_data_position].condition;                      \
                                                                              \
  if((condition != last_condition) || (condition >= 0x20))                    \
  {                                                                           \
    if((last_condition & 0x0F) != 0x0E)                                       \
    {                                                                         \
      generate_branch_patch_conditional(backpatch_address, translation_ptr);  \
    }                                                                         \
                                                                              \
    last_condition = condition;                                               \
                                                                              \
    condition &= 0x0F;                                                        \
                                                                              \
    if(condition != 0x0E)                                                     \
    {                                                                         \
      arm_conditional_block_header();                                         \
    }                                                                         \
  }                                                                           \
  emit_trace_arm_instruction(pc);                                             \
                                                                              \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x00:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MUL rd, rm, rs */                                                \
          arm_multiply(no, no);                                               \
          cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* AND rd, rn, reg_op */                                              \
        arm_data_proc(and, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x01:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MULS rd, rm, rs */                                             \
            arm_multiply(no, yes);                                            \
            cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ANDS rd, rn, reg_op */                                             \
        arm_data_proc(ands, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x02:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MLA rd, rm, rs, rn */                                            \
          arm_multiply(yes, no);                                              \
          cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EOR rd, rn, reg_op */                                              \
        arm_data_proc(eor, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MLAS rd, rm, rs, rn */                                         \
            arm_multiply(yes, yes);                                           \
            cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EORS rd, rn, reg_op */                                             \
        arm_data_proc(eors, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUB rd, rn, reg_op */                                              \
        arm_data_proc(sub, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUBS rd, rn, reg_op */                                             \
        arm_data_proc(subs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSB rd, rn, reg_op */                                              \
        arm_data_proc(rsb, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSBS rd, rn, reg_op */                                             \
        arm_data_proc(rsbs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMULL rd, rm, rs */                                              \
          arm_multiply_long(u64, no, no);                                     \
          cycle_count += 3;  /* this is an aproximation :P */                 \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD rd, rn, reg_op */                                              \
        arm_data_proc(add, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x09:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64, no, yes);                                  \
            cycle_count += 3;  /* this is an aproximation :P */               \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADDS rd, rn, reg_op */                                             \
        arm_data_proc(adds, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMLAL rd, rm, rs */                                              \
          arm_multiply_long(u64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADC rd, rn, reg_op */                                              \
        arm_data_proc(adc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADCS rd, rn, reg_op */                                             \
        arm_data_proc(adcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMULL rd, rm, rs */                                              \
          arm_multiply_long(s64, no, no);                                     \
          cycle_count += 2;  /* Between 1 and 4 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBC rd, rn, reg_op */                                              \
        arm_data_proc(sbc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64, no, yes);                                  \
            cycle_count += 2;  /* Between 1 and 4 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBCS rd, rn, reg_op */                                             \
        arm_data_proc(sbcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMLAL rd, rm, rs */                                              \
          arm_multiply_long(s64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSC rd, rn, reg_op */                                              \
        arm_data_proc(rsc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSCS rd, rn, reg_op */                                             \
        arm_data_proc(rscs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x10:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - rm] */                                            \
          arm_access_memory(store, down, pre, u16, half_reg);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWP rd, rm, [rn] */                                              \
          arm_swap(u32);                                                      \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, cpsr */                                                    \
        arm_psr(reg, read, cpsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x11:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm] */                                          \
            arm_access_memory(load, down, pre, u16, half_reg);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s8, half_reg);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s16, half_reg);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TST rd, rn, reg_op */                                              \
        arm_data_proc_test(tst, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - rm]! */                                             \
        arm_access_memory(store, down, pre_wb, u16, half_reg);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(opcode & 0x10)                                                     \
        {                                                                     \
          /* BX rn */                                                         \
          arm_bx();                                                           \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MSR cpsr, rm */                                                  \
          arm_psr(reg, store, cpsr);                                          \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x13:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm]! */                                         \
            arm_access_memory(load, down, pre_wb, u16, half_reg);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s8, half_reg);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s16, half_reg);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TEQ rd, rn, reg_op */                                              \
        arm_data_proc_test(teq, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x14:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - imm] */                                           \
          arm_access_memory(store, down, pre, u16, half_imm);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWPB rd, rm, [rn] */                                             \
          arm_swap(u8);                                                       \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, spsr */                                                    \
        arm_psr(reg, read, spsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x15:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm] */                                         \
            arm_access_memory(load, down, pre, u16, half_imm);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s8, half_imm);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s16, half_imm);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMP rn, reg_op */                                                  \
        arm_data_proc_test(cmp, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x16:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - imm]! */                                            \
        arm_access_memory(store, down, pre_wb, u16, half_imm);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MSR spsr, rm */                                                    \
        arm_psr(reg, store, spsr);                                            \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x17:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm]! */                                        \
            arm_access_memory(load, down, pre_wb, u16, half_imm);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s8, half_imm);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s16, half_imm);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMN rd, rn, reg_op */                                              \
        arm_data_proc_test(cmn, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x18:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm] */                                              \
        arm_access_memory(store, up, pre, u16, half_reg);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORR rd, rn, reg_op */                                              \
        arm_data_proc(orr, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x19:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm] */                                          \
            arm_access_memory(load, up, pre, u16, half_reg);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s8, half_reg);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s16, half_reg);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORRS rd, rn, reg_op */                                             \
        arm_data_proc(orrs, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm]! */                                             \
        arm_access_memory(store, up, pre_wb, u16, half_reg);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOV rd, reg_op */                                                  \
        arm_data_proc_unary(mov, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm]! */                                         \
            arm_access_memory(load, up, pre_wb, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOVS rd, reg_op */                                                 \
        arm_data_proc_unary(movs, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm] */                                             \
        arm_access_memory(store, up, pre, u16, half_imm);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BIC rd, rn, reg_op */                                              \
        arm_data_proc(bic, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm] */                                         \
            arm_access_memory(load, up, pre, u16, half_imm);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s8, half_imm);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s16, half_imm);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BICS rd, rn, reg_op */                                             \
        arm_data_proc(bics, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm]! */                                            \
        arm_access_memory(store, up, pre_wb, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVN rd, reg_op */                                                  \
        arm_data_proc_unary(mvn, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm]! */                                        \
            arm_access_memory(load, up, pre_wb, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVNS rd, rn, reg_op */                                             \
        arm_data_proc_unary(mvns, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x20:                                                                \
      /* AND rd, rn, imm */                                                   \
      arm_data_proc(and, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
      /* ANDS rd, rn, imm */                                                  \
      arm_data_proc(ands, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x22:                                                                \
      /* EOR rd, rn, imm */                                                   \
      arm_data_proc(eor, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x23:                                                                \
      /* EORS rd, rn, imm */                                                  \
      arm_data_proc(eors, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x24:                                                                \
      /* SUB rd, rn, imm */                                                   \
      arm_data_proc(sub, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x25:                                                                \
      /* SUBS rd, rn, imm */                                                  \
      arm_data_proc(subs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x26:                                                                \
      /* RSB rd, rn, imm */                                                   \
      arm_data_proc(rsb, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x27:                                                                \
      /* RSBS rd, rn, imm */                                                  \
      arm_data_proc(rsbs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x28:                                                                \
      /* ADD rd, rn, imm */                                                   \
      arm_data_proc(add, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x29:                                                                \
      /* ADDS rd, rn, imm */                                                  \
      arm_data_proc(adds, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2A:                                                                \
      /* ADC rd, rn, imm */                                                   \
      arm_data_proc(adc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2B:                                                                \
      /* ADCS rd, rn, imm */                                                  \
      arm_data_proc(adcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2C:                                                                \
      /* SBC rd, rn, imm */                                                   \
      arm_data_proc(sbc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2D:                                                                \
      /* SBCS rd, rn, imm */                                                  \
      arm_data_proc(sbcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2E:                                                                \
      /* RSC rd, rn, imm */                                                   \
      arm_data_proc(rsc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2F:                                                                \
      /* RSCS rd, rn, imm */                                                  \
      arm_data_proc(rscs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x30 ... 0x31:                                                       \
      /* TST rn, imm */                                                       \
      arm_data_proc_test(tst, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x32:                                                                \
      /* MSR cpsr, imm */                                                     \
      arm_psr(imm, store, cpsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x33:                                                                \
      /* TEQ rn, imm */                                                       \
      arm_data_proc_test(teq, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x34 ... 0x35:                                                       \
      /* CMP rn, imm */                                                       \
      arm_data_proc_test(cmp, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x36:                                                                \
      /* MSR spsr, imm */                                                     \
      arm_psr(imm, store, spsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x37:                                                                \
      /* CMN rn, imm */                                                       \
      arm_data_proc_test(cmn, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x38:                                                                \
      /* ORR rd, rn, imm */                                                   \
      arm_data_proc(orr, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x39:                                                                \
      /* ORRS rd, rn, imm */                                                  \
      arm_data_proc(orrs, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3A:                                                                \
      /* MOV rd, imm */                                                       \
      arm_data_proc_unary(mov, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3B:                                                                \
      /* MOVS rd, imm */                                                      \
      arm_data_proc_unary(movs, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x3C:                                                                \
      /* BIC rd, rn, imm */                                                   \
      arm_data_proc(bic, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x3D:                                                                \
      /* BICS rd, rn, imm */                                                  \
      arm_data_proc(bics, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3E:                                                                \
      /* MVN rd, imm */                                                       \
      arm_data_proc_unary(mvn, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3F:                                                                \
      /* MVNS rd, imm */                                                      \
      arm_data_proc_unary(mvns, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      /* STR rd, [rn], -imm */                                                \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      /* LDR rd, [rn], -imm */                                                \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      /* STRT rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      /* LDRT rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* STRB rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* LDRB rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* STRBT rd, [rn], -imm */                                              \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* LDRBT rd, [rn], -imm */                                              \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x48:                                                                \
      /* STR rd, [rn], +imm */                                                \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x49:                                                                \
      /* LDR rd, [rn], +imm */                                                \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4A:                                                                \
      /* STRT rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x4B:                                                                \
      /* LDRT rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4C:                                                                \
      /* STRB rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4D:                                                                \
      /* LDRB rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x4E:                                                                \
      /* STRBT rd, [rn], +imm */                                              \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4F:                                                                \
      /* LDRBT rd, [rn], +imm */                                              \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x50:                                                                \
      /* STR rd, [rn - imm] */                                                \
      arm_access_memory(store, down, pre, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x51:                                                                \
      /* LDR rd, [rn - imm] */                                                \
      arm_access_memory(load, down, pre, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x52:                                                                \
      /* STR rd, [rn - imm]! */                                               \
      arm_access_memory(store, down, pre_wb, u32, imm);                       \
      break;                                                                  \
                                                                              \
    case 0x53:                                                                \
      /* LDR rd, [rn - imm]! */                                               \
      arm_access_memory(load, down, pre_wb, u32, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x54:                                                                \
      /* STRB rd, [rn - imm] */                                               \
      arm_access_memory(store, down, pre, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x55:                                                                \
      /* LDRB rd, [rn - imm] */                                               \
      arm_access_memory(load, down, pre, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x56:                                                                \
      /* STRB rd, [rn - imm]! */                                              \
      arm_access_memory(store, down, pre_wb, u8, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x57:                                                                \
      /* LDRB rd, [rn - imm]! */                                              \
      arm_access_memory(load, down, pre_wb, u8, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x58:                                                                \
      /* STR rd, [rn + imm] */                                                \
      arm_access_memory(store, up, pre, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x59:                                                                \
      /* LDR rd, [rn + imm] */                                                \
      arm_access_memory(load, up, pre, u32, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5A:                                                                \
      /* STR rd, [rn + imm]! */                                               \
      arm_access_memory(store, up, pre_wb, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x5B:                                                                \
      /* LDR rd, [rn + imm]! */                                               \
      arm_access_memory(load, up, pre_wb, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5C:                                                                \
      /* STRB rd, [rn + imm] */                                               \
      arm_access_memory(store, up, pre, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5D:                                                                \
      /* LDRB rd, [rn + imm] */                                               \
      arm_access_memory(load, up, pre, u8, imm);                              \
      break;                                                                  \
                                                                              \
    case 0x5E:                                                                \
      /* STRB rd, [rn + imm]! */                                              \
      arm_access_memory(store, up, pre_wb, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5F:                                                                \
      /* LDRBT rd, [rn + imm]! */                                             \
      arm_access_memory(load, up, pre_wb, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x60:                                                                \
      /* STR rd, [rn], -rm */                                                 \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x61:                                                                \
      /* LDR rd, [rn], -rm */                                                 \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x62:                                                                \
      /* STRT rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x63:                                                                \
      /* LDRT rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x64:                                                                \
      /* STRB rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x65:                                                                \
      /* LDRB rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x66:                                                                \
      /* STRBT rd, [rn], -rm */                                               \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x67:                                                                \
      /* LDRBT rd, [rn], -rm */                                               \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x68:                                                                \
      /* STR rd, [rn], +rm */                                                 \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x69:                                                                \
      /* LDR rd, [rn], +rm */                                                 \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6A:                                                                \
      /* STRT rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x6B:                                                                \
      /* LDRT rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6C:                                                                \
      /* STRB rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6D:                                                                \
      /* LDRB rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x6E:                                                                \
      /* STRBT rd, [rn], +rm */                                               \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6F:                                                                \
      /* LDRBT rd, [rn], +rm */                                               \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x70:                                                                \
      /* STR rd, [rn - rm] */                                                 \
      arm_access_memory(store, down, pre, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x71:                                                                \
      /* LDR rd, [rn - rm] */                                                 \
      arm_access_memory(load, down, pre, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x72:                                                                \
      /* STR rd, [rn - rm]! */                                                \
      arm_access_memory(store, down, pre_wb, u32, reg);                       \
      break;                                                                  \
                                                                              \
    case 0x73:                                                                \
      /* LDR rd, [rn - rm]! */                                                \
      arm_access_memory(load, down, pre_wb, u32, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x74:                                                                \
      /* STRB rd, [rn - rm] */                                                \
      arm_access_memory(store, down, pre, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x75:                                                                \
      /* LDRB rd, [rn - rm] */                                                \
      arm_access_memory(load, down, pre, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x76:                                                                \
      /* STRB rd, [rn - rm]! */                                               \
      arm_access_memory(store, down, pre_wb, u8, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x77:                                                                \
      /* LDRB rd, [rn - rm]! */                                               \
      arm_access_memory(load, down, pre_wb, u8, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x78:                                                                \
      /* STR rd, [rn + rm] */                                                 \
      arm_access_memory(store, up, pre, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x79:                                                                \
      /* LDR rd, [rn + rm] */                                                 \
      arm_access_memory(load, up, pre, u32, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7A:                                                                \
      /* STR rd, [rn + rm]! */                                                \
      arm_access_memory(store, up, pre_wb, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x7B:                                                                \
      /* LDR rd, [rn + rm]! */                                                \
      arm_access_memory(load, up, pre_wb, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7C:                                                                \
      /* STRB rd, [rn + rm] */                                                \
      arm_access_memory(store, up, pre, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7D:                                                                \
      /* LDRB rd, [rn + rm] */                                                \
      arm_access_memory(load, up, pre, u8, reg);                              \
      break;                                                                  \
                                                                              \
    case 0x7E:                                                                \
      /* STRB rd, [rn + rm]! */                                               \
      arm_access_memory(store, up, pre_wb, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7F:                                                                \
      /* LDRBT rd, [rn + rm]! */                                              \
      arm_access_memory(load, up, pre_wb, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x80:                                                                \
      /* STMDA rn, rlist */                                                   \
      arm_block_memory(store, down_a, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x81:                                                                \
      /* LDMDA rn, rlist */                                                   \
      arm_block_memory(load, down_a, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x82:                                                                \
      /* STMDA rn!, rlist */                                                  \
      arm_block_memory(store, down_a, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x83:                                                                \
      /* LDMDA rn!, rlist */                                                  \
      arm_block_memory(load, down_a, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x84:                                                                \
      /* STMDA rn, rlist^ */                                                  \
      arm_block_memory(store, down_a, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x85:                                                                \
      /* LDMDA rn, rlist^ */                                                  \
      arm_block_memory(load, down_a, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x86:                                                                \
      /* STMDA rn!, rlist^ */                                                 \
      arm_block_memory(store, down_a, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x87:                                                                \
      /* LDMDA rn!, rlist^ */                                                 \
      arm_block_memory(load, down_a, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x88:                                                                \
      /* STMIA rn, rlist */                                                   \
      arm_block_memory(store, no, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x89:                                                                \
      /* LDMIA rn, rlist */                                                   \
      arm_block_memory(load, no, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8A:                                                                \
      /* STMIA rn!, rlist */                                                  \
      arm_block_memory(store, no, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x8B:                                                                \
      /* LDMIA rn!, rlist */                                                  \
      arm_block_memory(load, no, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8C:                                                                \
      /* STMIA rn, rlist^ */                                                  \
      arm_block_memory(store, no, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8D:                                                                \
      /* LDMIA rn, rlist^ */                                                  \
      arm_block_memory(load, no, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x8E:                                                                \
      /* STMIA rn!, rlist^ */                                                 \
      arm_block_memory(store, no, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8F:                                                                \
      /* LDMIA rn!, rlist^ */                                                 \
      arm_block_memory(load, no, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x90:                                                                \
      /* STMDB rn, rlist */                                                   \
      arm_block_memory(store, down_b, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x91:                                                                \
      /* LDMDB rn, rlist */                                                   \
      arm_block_memory(load, down_b, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x92:                                                                \
      /* STMDB rn!, rlist */                                                  \
      arm_block_memory(store, down_b, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x93:                                                                \
      /* LDMDB rn!, rlist */                                                  \
      arm_block_memory(load, down_b, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x94:                                                                \
      /* STMDB rn, rlist^ */                                                  \
      arm_block_memory(store, down_b, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x95:                                                                \
      /* LDMDB rn, rlist^ */                                                  \
      arm_block_memory(load, down_b, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x96:                                                                \
      /* STMDB rn!, rlist^ */                                                 \
      arm_block_memory(store, down_b, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x97:                                                                \
      /* LDMDB rn!, rlist^ */                                                 \
      arm_block_memory(load, down_b, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x98:                                                                \
      /* STMIB rn, rlist */                                                   \
      arm_block_memory(store, up, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x99:                                                                \
      /* LDMIB rn, rlist */                                                   \
      arm_block_memory(load, up, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9A:                                                                \
      /* STMIB rn!, rlist */                                                  \
      arm_block_memory(store, up, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x9B:                                                                \
      /* LDMIB rn!, rlist */                                                  \
      arm_block_memory(load, up, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9C:                                                                \
      /* STMIB rn, rlist^ */                                                  \
      arm_block_memory(store, up, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9D:                                                                \
      /* LDMIB rn, rlist^ */                                                  \
      arm_block_memory(load, up, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x9E:                                                                \
      /* STMIB rn!, rlist^ */                                                 \
      arm_block_memory(store, up, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9F:                                                                \
      /* LDMIB rn!, rlist^ */                                                 \
      arm_block_memory(load, up, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0xA0 ... 0xAF:                                                       \
    {                                                                         \
      /* B offset */                                                          \
      arm_b();                                                                \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xB0 ... 0xBF:                                                       \
    {                                                                         \
      /* BL offset */                                                         \
      arm_bl();                                                               \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xFF:                                                       \
    {                                                                         \
      u32 swinum = (opcode >> 16) & 0xFF;                                     \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(arm);                                                     \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(arm);                                                 \
      }                                                                       \
      else {                                                                  \
        arm_swi();                                                            \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 4                                                                     \

#define arm_flag_status()                                                     \

#define translate_thumb_instruction()                                         \
  flag_status = block_data[block_data_position].flag_data;                    \
  check_pc_region(pc);                                                        \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (pc & 0x7FFF));                    \
  emit_trace_thumb_instruction(pc);                                           \
  u8 hiop = opcode >> 8;                                                      \
                                                                              \
  switch(hiop)                                                                \
  {                                                                           \
    case 0x00 ... 0x07:                                                       \
      /* LSL rd, rs, imm */                                                   \
      thumb_shift(shift, lsl, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x08 ... 0x0F:                                                       \
      /* LSR rd, rs, imm */                                                   \
      thumb_shift(shift, lsr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x10 ... 0x17:                                                       \
      /* ASR rd, rs, imm */                                                   \
      thumb_shift(shift, asr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x18 ... 0x19:                                                       \
      /* ADD rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, adds, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1A ... 0x1B:                                                       \
      /* SUB rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, subs, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1C ... 0x1D:                                                       \
      /* ADD rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, adds, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    case 0x1E ... 0x1F:                                                       \
      /* SUB rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, subs, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    /* MOV r0..7, imm */                                                      \
    case 0x20: thumb_data_proc_unary(imm, movs, imm, 0, imm); break;          \
    case 0x21: thumb_data_proc_unary(imm, movs, imm, 1, imm); break;          \
    case 0x22: thumb_data_proc_unary(imm, movs, imm, 2, imm); break;          \
    case 0x23: thumb_data_proc_unary(imm, movs, imm, 3, imm); break;          \
    case 0x24: thumb_data_proc_unary(imm, movs, imm, 4, imm); break;          \
    case 0x25: thumb_data_proc_unary(imm, movs, imm, 5, imm); break;          \
    case 0x26: thumb_data_proc_unary(imm, movs, imm, 6, imm); break;          \
    case 0x27: thumb_data_proc_unary(imm, movs, imm, 7, imm); break;          \
                                                                              \
    /* CMP r0, imm */                                                         \
    case 0x28: thumb_data_proc_test(imm, cmp, imm, 0, imm); break;            \
    case 0x29: thumb_data_proc_test(imm, cmp, imm, 1, imm); break;            \
    case 0x2A: thumb_data_proc_test(imm, cmp, imm, 2, imm); break;            \
    case 0x2B: thumb_data_proc_test(imm, cmp, imm, 3, imm); break;            \
    case 0x2C: thumb_data_proc_test(imm, cmp, imm, 4, imm); break;            \
    case 0x2D: thumb_data_proc_test(imm, cmp, imm, 5, imm); break;            \
    case 0x2E: thumb_data_proc_test(imm, cmp, imm, 6, imm); break;            \
    case 0x2F: thumb_data_proc_test(imm, cmp, imm, 7, imm); break;            \
                                                                              \
    /* ADD r0..7, imm */                                                      \
    case 0x30: thumb_data_proc(imm, adds, imm, 0, 0, imm); break;             \
    case 0x31: thumb_data_proc(imm, adds, imm, 1, 1, imm); break;             \
    case 0x32: thumb_data_proc(imm, adds, imm, 2, 2, imm); break;             \
    case 0x33: thumb_data_proc(imm, adds, imm, 3, 3, imm); break;             \
    case 0x34: thumb_data_proc(imm, adds, imm, 4, 4, imm); break;             \
    case 0x35: thumb_data_proc(imm, adds, imm, 5, 5, imm); break;             \
    case 0x36: thumb_data_proc(imm, adds, imm, 6, 6, imm); break;             \
    case 0x37: thumb_data_proc(imm, adds, imm, 7, 7, imm); break;             \
                                                                              \
    /* SUB r0..7, imm */                                                      \
    case 0x38: thumb_data_proc(imm, subs, imm, 0, 0, imm); break;             \
    case 0x39: thumb_data_proc(imm, subs, imm, 1, 1, imm); break;             \
    case 0x3A: thumb_data_proc(imm, subs, imm, 2, 2, imm); break;             \
    case 0x3B: thumb_data_proc(imm, subs, imm, 3, 3, imm); break;             \
    case 0x3C: thumb_data_proc(imm, subs, imm, 4, 4, imm); break;             \
    case 0x3D: thumb_data_proc(imm, subs, imm, 5, 5, imm); break;             \
    case 0x3E: thumb_data_proc(imm, subs, imm, 6, 6, imm); break;             \
    case 0x3F: thumb_data_proc(imm, subs, imm, 7, 7, imm); break;             \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_data_proc(alu_op, ands, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_data_proc(alu_op, eors, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_shift(alu_op, lsl, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_shift(alu_op, lsr, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_shift(alu_op, asr, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_data_proc(alu_op, adcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_data_proc(alu_op, sbcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_shift(alu_op, ror, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* TST rd, rs */                                                    \
          thumb_data_proc_test(alu_op, tst, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* NEG rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, neg, reg, rd, rs);                    \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* CMP rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmp, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* CMN rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmn, reg, rd, rs);                     \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ORR rd, rs */                                                    \
          thumb_data_proc(alu_op, orrs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* MUL rd, rs */                                                    \
          thumb_data_proc(alu_op, muls, reg, rd, rs, rd);                     \
          cycle_count += 2;  /* Between 1 and 4 extra cycles */               \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* BIC rd, rs */                                                    \
          thumb_data_proc(alu_op, bics, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* MVN rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, mvns, reg, rd, rs);                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* ADD rd, rs */                                                        \
      thumb_data_proc_hi(add);                                                \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_data_proc_test_hi(cmp);                                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* MOV rd, rs */                                                        \
      thumb_data_proc_mov_hi();                                               \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* BX rs */                                                             \
      thumb_bx();                                                             \
      break;                                                                  \
                                                                              \
    case 0x48 ... 0x4F:                                                       \
      /* LDR r0..7, [pc + imm] */                                             \
      {                                                                       \
        thumb_decode_imm();                                                   \
        u32 rdreg = (hiop & 7);                                               \
        u32 aoff = (pc & ~2) + (imm*4) + 4;                                   \
        /* ROM + same page -> optimize as const load */                       \
        if (!ram_region && (((aoff + 4) >> 15) == (pc >> 15))) {              \
          u32 value = readaddress32(pc_address_block, (aoff & 0x7FFF));       \
          thumb_load_pc_pool_const(rdreg, value);                             \
        } else {                                                              \
          thumb_access_memory(load, imm, rdreg, 0, 0, pc_relative, aoff, u32);\
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x50 ... 0x51:                                                       \
      /* STR rd, [rb + ro] */                                                 \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u32);       \
      break;                                                                  \
                                                                              \
    case 0x52 ... 0x53:                                                       \
      /* STRH rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u16);       \
      break;                                                                  \
                                                                              \
    case 0x54 ... 0x55:                                                       \
      /* STRB rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u8);        \
      break;                                                                  \
                                                                              \
    case 0x56 ... 0x57:                                                       \
      /* LDSB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s8);         \
      break;                                                                  \
                                                                              \
    case 0x58 ... 0x59:                                                       \
      /* LDR rd, [rb + ro] */                                                 \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u32);        \
      break;                                                                  \
                                                                              \
    case 0x5A ... 0x5B:                                                       \
      /* LDRH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u16);        \
      break;                                                                  \
                                                                              \
    case 0x5C ... 0x5D:                                                       \
      /* LDRB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u8);         \
      break;                                                                  \
                                                                              \
    case 0x5E ... 0x5F:                                                       \
      /* LDSH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s16);        \
      break;                                                                  \
                                                                              \
    case 0x60 ... 0x67:                                                       \
      /* STR rd, [rb + imm] */                                                \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, (imm * 4),      \
       u32);                                                                  \
      break;                                                                  \
                                                                              \
    case 0x68 ... 0x6F:                                                       \
      /* LDR rd, [rb + imm] */                                                \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 4), u32); \
      break;                                                                  \
                                                                              \
    case 0x70 ... 0x77:                                                       \
      /* STRB rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, imm, u8);       \
      break;                                                                  \
                                                                              \
    case 0x78 ... 0x7F:                                                       \
      /* LDRB rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, imm, u8);        \
      break;                                                                  \
                                                                              \
    case 0x80 ... 0x87:                                                       \
      /* STRH rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm,                 \
       (imm * 2), u16);                                                       \
      break;                                                                  \
                                                                              \
    case 0x88 ... 0x8F:                                                       \
      /* LDRH rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 2), u16); \
      break;                                                                  \
                                                                              \
    /* STR r0..7, [sp + imm] */                                               \
    case 0x90:                                                                \
      thumb_access_memory(store, imm, 0, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x91:                                                                \
      thumb_access_memory(store, imm, 1, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x92:                                                                \
      thumb_access_memory(store, imm, 2, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x93:                                                                \
      thumb_access_memory(store, imm, 3, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x94:                                                                \
      thumb_access_memory(store, imm, 4, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x95:                                                                \
      thumb_access_memory(store, imm, 5, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x96:                                                                \
      thumb_access_memory(store, imm, 6, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x97:                                                                \
      thumb_access_memory(store, imm, 7, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
                                                                              \
    /* LDR r0..7, [sp + imm] */                                               \
    case 0x98:                                                                \
      thumb_access_memory(load, imm, 0, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x99:                                                                \
      thumb_access_memory(load, imm, 1, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9A:                                                                \
      thumb_access_memory(load, imm, 2, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9B:                                                                \
      thumb_access_memory(load, imm, 3, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9C:                                                                \
      thumb_access_memory(load, imm, 4, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9D:                                                                \
      thumb_access_memory(load, imm, 5, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9E:                                                                \
      thumb_access_memory(load, imm, 6, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9F:                                                                \
      thumb_access_memory(load, imm, 7, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
                                                                              \
    /* ADD r0..7, pc, +imm */                                                 \
    case 0xA0: thumb_load_pc(0); break;                                       \
    case 0xA1: thumb_load_pc(1); break;                                       \
    case 0xA2: thumb_load_pc(2); break;                                       \
    case 0xA3: thumb_load_pc(3); break;                                       \
    case 0xA4: thumb_load_pc(4); break;                                       \
    case 0xA5: thumb_load_pc(5); break;                                       \
    case 0xA6: thumb_load_pc(6); break;                                       \
    case 0xA7: thumb_load_pc(7); break;                                       \
                                                                              \
    /* ADD r0..7, sp, +imm */                                                 \
    case 0xA8: thumb_load_sp(0); break;                                       \
    case 0xA9: thumb_load_sp(1); break;                                       \
    case 0xAA: thumb_load_sp(2); break;                                       \
    case 0xAB: thumb_load_sp(3); break;                                       \
    case 0xAC: thumb_load_sp(4); break;                                       \
    case 0xAD: thumb_load_sp(5); break;                                       \
    case 0xAE: thumb_load_sp(6); break;                                       \
    case 0xAF: thumb_load_sp(7); break;                                       \
                                                                              \
    case 0xB0 ... 0xB3:                                                       \
      if((opcode >> 7) & 0x01)                                                \
      {                                                                       \
        /* ADD sp, -imm */                                                    \
        thumb_adjust_sp(down);                                                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD sp, +imm */                                                    \
        thumb_adjust_sp(up);                                                  \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0xB4:                                                                \
      /* PUSH rlist */                                                        \
      thumb_block_memory(store, down, no, 13);                                \
      break;                                                                  \
                                                                              \
    case 0xB5:                                                                \
      /* PUSH rlist, lr */                                                    \
      thumb_block_memory(store, push_lr, push_lr, 13);                        \
      break;                                                                  \
                                                                              \
    case 0xBC:                                                                \
      /* POP rlist */                                                         \
      thumb_block_memory(load, no, up, 13);                                   \
      break;                                                                  \
                                                                              \
    case 0xBD:                                                                \
      /* POP rlist, pc */                                                     \
      thumb_block_memory(load, no, pop_pc, 13);                               \
      break;                                                                  \
                                                                              \
    case 0xC0:                                                                \
      /* STMIA r0!, rlist */                                                  \
      thumb_block_memory(store, no, up, 0);                                   \
      break;                                                                  \
                                                                              \
    case 0xC1:                                                                \
      /* STMIA r1!, rlist */                                                  \
      thumb_block_memory(store, no, up, 1);                                   \
      break;                                                                  \
                                                                              \
    case 0xC2:                                                                \
      /* STMIA r2!, rlist */                                                  \
      thumb_block_memory(store, no, up, 2);                                   \
      break;                                                                  \
                                                                              \
    case 0xC3:                                                                \
      /* STMIA r3!, rlist */                                                  \
      thumb_block_memory(store, no, up, 3);                                   \
      break;                                                                  \
                                                                              \
    case 0xC4:                                                                \
      /* STMIA r4!, rlist */                                                  \
      thumb_block_memory(store, no, up, 4);                                   \
      break;                                                                  \
                                                                              \
    case 0xC5:                                                                \
      /* STMIA r5!, rlist */                                                  \
      thumb_block_memory(store, no, up, 5);                                   \
      break;                                                                  \
                                                                              \
    case 0xC6:                                                                \
      /* STMIA r6!, rlist */                                                  \
      thumb_block_memory(store, no, up, 6);                                   \
      break;                                                                  \
                                                                              \
    case 0xC7:                                                                \
      /* STMIA r7!, rlist */                                                  \
      thumb_block_memory(store, no, up, 7);                                   \
      break;                                                                  \
                                                                              \
    case 0xC8:                                                                \
      /* LDMIA r0!, rlist */                                                  \
      thumb_block_memory(load, no, up, 0);                                    \
      break;                                                                  \
                                                                              \
    case 0xC9:                                                                \
      /* LDMIA r1!, rlist */                                                  \
      thumb_block_memory(load, no, up, 1);                                    \
      break;                                                                  \
                                                                              \
    case 0xCA:                                                                \
      /* LDMIA r2!, rlist */                                                  \
      thumb_block_memory(load, no, up, 2);                                    \
      break;                                                                  \
                                                                              \
    case 0xCB:                                                                \
      /* LDMIA r3!, rlist */                                                  \
      thumb_block_memory(load, no, up, 3);                                    \
      break;                                                                  \
                                                                              \
    case 0xCC:                                                                \
      /* LDMIA r4!, rlist */                                                  \
      thumb_block_memory(load, no, up, 4);                                    \
      break;                                                                  \
                                                                              \
    case 0xCD:                                                                \
      /* LDMIA r5!, rlist */                                                  \
      thumb_block_memory(load, no, up, 5);                                    \
      break;                                                                  \
                                                                              \
    case 0xCE:                                                                \
      /* LDMIA r6!, rlist */                                                  \
      thumb_block_memory(load, no, up, 6);                                    \
      break;                                                                  \
                                                                              \
    case 0xCF:                                                                \
      /* LDMIA r7!, rlist */                                                  \
      thumb_block_memory(load, no, up, 7);                                    \
      break;                                                                  \
                                                                              \
    case 0xD0:                                                                \
      /* BEQ label */                                                         \
      thumb_conditional_branch(eq);                                           \
      break;                                                                  \
                                                                              \
    case 0xD1:                                                                \
      /* BNE label */                                                         \
      thumb_conditional_branch(ne);                                           \
      break;                                                                  \
                                                                              \
    case 0xD2:                                                                \
      /* BCS label */                                                         \
      thumb_conditional_branch(cs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD3:                                                                \
      /* BCC label */                                                         \
      thumb_conditional_branch(cc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD4:                                                                \
      /* BMI label */                                                         \
      thumb_conditional_branch(mi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD5:                                                                \
      /* BPL label */                                                         \
      thumb_conditional_branch(pl);                                           \
      break;                                                                  \
                                                                              \
    case 0xD6:                                                                \
      /* BVS label */                                                         \
      thumb_conditional_branch(vs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD7:                                                                \
      /* BVC label */                                                         \
      thumb_conditional_branch(vc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD8:                                                                \
      /* BHI label */                                                         \
      thumb_conditional_branch(hi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD9:                                                                \
      /* BLS label */                                                         \
      thumb_conditional_branch(ls);                                           \
      break;                                                                  \
                                                                              \
    case 0xDA:                                                                \
      /* BGE label */                                                         \
      thumb_conditional_branch(ge);                                           \
      break;                                                                  \
                                                                              \
    case 0xDB:                                                                \
      /* BLT label */                                                         \
      thumb_conditional_branch(lt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDC:                                                                \
      /* BGT label */                                                         \
      thumb_conditional_branch(gt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDD:                                                                \
      /* BLE label */                                                         \
      thumb_conditional_branch(le);                                           \
      break;                                                                  \
                                                                              \
    case 0xDF:                                                                \
    {                                                                         \
      u32 swinum = opcode & 0xFF;                                             \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(thumb);                                                   \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(thumb);                                               \
      }                                                                       \
      else {                                                                  \
        thumb_swi();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xE0 ... 0xE7:                                                       \
    {                                                                         \
      /* B label */                                                           \
      thumb_b();                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xF7:                                                       \
    {                                                                         \
      /* (low word) BL label */                                               \
      /* This should possibly generate code if not in conjunction with a BLH  \
         next, but I don't think anyone will do that. */                      \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF8 ... 0xFF:                                                       \
    {                                                                         \
      /* (high word) BL label */                                              \
      /* This might not be preceeding a BL low word (Golden Sun 2), if so     \
         it must be handled like an indirect branch. */                       \
      if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                   \
      {                                                                       \
        thumb_bl();                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_blh();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 2                                                                     \

#define thumb_flag_modifies_all()                                             \
  flag_status |= 0xFF                                                         \

#define thumb_flag_modifies_zn()                                              \
  flag_status |= 0xCC                                                         \

#define thumb_flag_modifies_znc()                                             \
  flag_status |= 0xEE                                                         \

#define thumb_flag_modifies_zn_maybe_c()                                      \
  flag_status |= 0xCE                                                         \

#define thumb_flag_modifies_c()                                               \
  flag_status |= 0x22                                                         \

#define thumb_flag_requires_c()                                               \
  flag_status |= 0x200                                                        \

#define thumb_flag_requires_all()                                             \
  flag_status |= 0xF00                                                        \

#define thumb_flag_status()                                                   \
{                                                                             \
  u16 flag_status = 0;                                                        \
  switch((opcode >> 8) & 0xFF)                                                \
  {                                                                           \
    /* left shift by imm */                                                   \
    case 0x00 ... 0x07:                                                       \
      thumb_flag_modifies_zn();                                               \
      if(((opcode >> 6) & 0x1F) != 0)                                         \
      {                                                                       \
        thumb_flag_modifies_c();                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* right shift by imm */                                                  \
    case 0x08 ... 0x17:                                                       \
      thumb_flag_modifies_znc();                                              \
      break;                                                                  \
                                                                              \
    /* add, subtract */                                                       \
    case 0x18 ... 0x1F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov reg, imm */                                                        \
    case 0x20 ... 0x27:                                                       \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    /* cmp reg, imm; add, subtract */                                         \
    case 0x28 ... 0x3F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* TST, NEG, CMP, CMN */                                                  \
    case 0x42:                                                                \
      if(((opcode >> 6) & 0x03) == 0)                                         \
        thumb_flag_modifies_zn();                                             \
      else                                                                    \
        thumb_flag_modifies_all();                                            \
      break;                                                                  \
                                                                              \
    /* ORR, MUL, BIC, MVN */                                                  \
    case 0x43:                                                                \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov might change PC (fall through if so) */                            \
    case 0x46:                                                                \
      if((opcode & 0xFF87) != 0x4687)                                         \
        break;                                                                \
      /* fall through */                                                      \
                                                                              \
    /* branches (can change PC) */                                            \
    case 0x47:                                                                \
    case 0xBD:                                                                \
    case 0xD0 ... 0xE7:                                                       \
    case 0xF0 ... 0xFF:                                                       \
      thumb_flag_requires_all();                                              \
      break;                                                                  \
  }                                                                           \
  block_data[block_data_position].flag_data = flag_status;                    \
}                                                                             \

#if defined(GPSP_RV32_JIT)
/* Keep gpSP's scanner and lookup. The RV32 emitter owns the execution body. */
#undef translate_arm_instruction
#define translate_arm_instruction()                                           \
{                                                                             \
  bool ends_block = false;                                                    \
  gpsp_rv32_block_link link;                                                  \
  check_pc_region(pc);                                                        \
  opcode = readaddress32(pc_address_block, (pc & 0x7FFF));                    \
  /* Staged link slots keep their alignment when copied. */                  \
  if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit) ||         \
      !gpsp_rv32_emit_block_instruction(&translation_ptr,                     \
       translation_cache_limit, pc, opcode, false,                            \
       pc != cheat_master_hook, 0xF, false, &ends_block, &link))               \
  {                                                                           \
    translation_failure = TRANSLATION_FAILURE_CACHE_FULL;                    \
    return false;                                                             \
  }                                                                           \
  rv32_benchmark_arm_site();                                                  \
  if (link.site)                                                              \
  {                                                                           \
    if (link.thumb || block_exit_position == MAX_EXITS)                       \
    {                                                                         \
      translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;                 \
      return false;                                                           \
    }                                                                         \
    block_exits[block_exit_position].branch_source = link.site;              \
    block_exits[block_exit_position].branch_target = link.target;             \
    block_exit_position++;                                                    \
  }                                                                           \
  pc += 4;                                                                    \
  if (ends_block)                                                             \
    block_end_pc = pc;                                                        \
}

#undef translate_thumb_instruction
#define translate_thumb_instruction()                                         \
{                                                                             \
  bool ends_block = false;                                                    \
  gpsp_rv32_block_link link = {0};                                            \
  check_pc_region(pc);                                                        \
  opcode = readaddress16(pc_address_block, (pc & 0x7FFF));                    \
  u32 literal_value;                                                          \
  bool literal_constant = pc != cheat_master_hook &&                          \
      rv32_thumb_literal_value(pc, (u16)opcode, pc_address_block,             \
          &literal_value);                                                    \
  /* Staged link slots keep their alignment when copied. */                  \
  bool emitted = rv32_pad_to_word(&translation_ptr,                           \
      translation_cache_limit) && (literal_constant ?                         \
      gpsp_rv32_emit_thumb_literal_constant(&translation_ptr,                \
          translation_cache_limit, pc, (u16)opcode, literal_value) :          \
      gpsp_rv32_emit_block_instruction(&translation_ptr,                     \
       translation_cache_limit, pc, opcode, true,                             \
       pc != cheat_master_hook,                                               \
       rv32_thumb_batch_remaining ?                                          \
           block_data[block_data_position].flag_data & 0xF : 0xF,            \
       rv32_thumb_batch_remaining != 0, &ends_block, &link));                 \
  if (!emitted)                                                               \
  {                                                                           \
    translation_failure = TRANSLATION_FAILURE_CACHE_FULL;                    \
    return false;                                                             \
  }                                                                           \
  rv32_benchmark_thumb_site((u16)opcode);                                     \
  if (link.site)                                                              \
  {                                                                           \
    if (!link.thumb || block_exit_position == MAX_EXITS)                      \
    {                                                                         \
      translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;                 \
      return false;                                                           \
    }                                                                         \
    block_exits[block_exit_position].branch_source = link.site;              \
    block_exits[block_exit_position].branch_target = link.target;             \
    block_exit_position++;                                                    \
  }                                                                           \
  pc += 2;                                                                    \
  rv32_thumb_fallthrough = !ends_block;                                       \
  if (ends_block)                                                             \
    block_end_pc = pc;                                                        \
}
#endif

// I/EWRAM memory tagging
// Code emitted in the RAM cache has tags (16 bit values) in the mirror tag ram
// that indicate that the address contains code. The following values are used:
// 0x0000 : this is just data (never translated)
// 0x00XX : not used (since first byte is zero)
// 0x0101 : this is code that is not the start of a translated block
// 0xXXXX : this is the start of a translated block, starting from 0xFFFF downwards
//          LSB is always set (we decrement by two) to ensure both bytes != 0
//
// Tags index ramtag_type entries, starting at 0xFFFF and counting down.
// RV32 uses a separate table; other backends store it at the RAM cache end.

#define LAST_TAG_NUM       0x0101
#define INITIAL_TOP_TAG    0xFFFF
#define CODE_TAG_BLOCK16   0x0101

#define VALID_TAG(tagn) (tagn > LAST_TAG_NUM)

#define allocate_tag_arm(location) {   \
  location[0] = ram_block_tag;         \
  /* Could be another thumb inst */    \
  if (!location[1])                    \
    location[1] = CODE_TAG_BLOCK16;    \
  ram_block_tag -= 2;                  \
}

#define allocate_tag_thumb(location) { \
  location[0] = ram_block_tag;         \
  ram_block_tag -= 2;                  \
}

typedef struct
{
  u32 offset_arm;     // Cache offset to the ARM-mode compiled block
  u32 offset_thumb;   // Cache offset to the Thumb-mode compiled block
} ramtag_type;

static u32 ram_block_tag = INITIAL_TOP_TAG;

#if defined(GPSP_RV32_JIT)
/* The tags have a table of their own, so cache chunks hold only code. */
#define RV32_RAM_TAGS ((INITIAL_TOP_TAG - LAST_TAG_NUM) / 2)
static ramtag_type *rv32_ram_tags;

bool rv32_ram_tags_alloc(void)
{
  if (!rv32_ram_tags)
    rv32_ram_tags = (ramtag_type *)calloc(RV32_RAM_TAGS,
        sizeof(*rv32_ram_tags));
  return rv32_ram_tags != NULL;
}

void rv32_ram_tags_free(void)
{
  free(rv32_ram_tags);
  rv32_ram_tags = NULL;
}

inline static ramtag_type* get_ram_tag(u16 tagval) {
  return &rv32_ram_tags[(INITIAL_TOP_TAG - tagval) >> 1];
}
#else
#define rv32_ram_limit() (ram_translation_cache + RAM_TRANSLATION_CACHE_SIZE \
    - TRANSLATION_CACHE_LIMIT_THRESHOLD \
    - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type))

inline static ramtag_type* get_ram_tag(u16 tagval) {
  ramtag_type *tbl = (ramtag_type*)&ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE];
  s16 tgidx = (s16)(tagval);
  return &tbl[tgidx >> 1];  /* Since LSB is always 1 and thus unused */
}
#endif

/* External branch exits are resolved iteratively through this worklist
   rather than by recursing from translate_block into block_lookup_translate
   for every branch target. Long chains of not-yet-translated blocks would
   otherwise nest one host stack frame pair per block and can overflow small
   thread stacks (Android runs cores on a ~1MiB native thread). Entries are
   queued while translating and drained by the outermost lookup, keeping the
   host stack depth constant regardless of the guest branch graph. */

typedef struct
{
  u8 *branch_source;
  u32 branch_target;
  u32 thumb;
} deferred_exit_type;

static deferred_exit_type *deferred_exits = NULL;
static u32 deferred_exit_count = 0;
#if !defined(GPSP_RV32_JIT)
static u32 deferred_exit_capacity = 0;
#endif
static u32 translate_depth = 0;

#if defined(GPSP_RV32_JIT)
typedef struct
{
  u8 *site;
  u32 key;
  u32 next;
  u32 next_new;
} rv32_pending_link_type;

typedef struct
{
  u8 *site;
  u32 key;
} rv32_pending_ram_link_type;

static rv32_pending_link_type *rv32_pending_links = NULL;
static u32 rv32_pending_link_count = 0;
static u32 rv32_pending_link_used = 0;
static u32 rv32_pending_link_capacity = 0;
static u32 rv32_pending_link_free = 0;
static u32 rv32_pending_link_new = 0;
enum { RV32_PENDING_LINK_BUCKETS = 1024 };
static u32 rv32_pending_link_buckets[RV32_PENDING_LINK_BUCKETS];
static rv32_pending_ram_link_type *rv32_pending_ram_links = NULL;
static u32 rv32_pending_ram_link_count = 0;
static u32 rv32_pending_ram_link_capacity = 0;
static bool rv32_bios_pinning;

typedef struct
{
  u32 key;
  u32 offset;
} rv32_entry_type;

static rv32_entry_type *rv32_rom_entries;
/* Native body range distinguishes repeated translations of one guest span. */
typedef struct
{
  u32 body_wait;
  u32 guest_wait;
  u32 literal_span;
} rv32_mixed_region_type;

_Static_assert(sizeof(rv32_mixed_region_type) == 12,
    "Mixed ROM region must fit in 12 bytes");

#define RV32_MIXED_BODY_MASK ((1U << 22) - 1)
#define RV32_MIXED_GUEST_MASK ((1U << 26) - 1)
#define RV32_MIXED_LITERAL_MASK ((1U << 16) - 1)
#define RV32_MIXED_SPAN_MASK ((1U << 14) - 1)
#define RV32_MIXED_BRANCH_END (1U << 30)
#define RV32_MIXED_MEMORY_HEAD (1U << 31)
#define RV32_MIXED_GUEST_BASE 0x08000000U

static u32 rv32_mixed_region_body(const rv32_mixed_region_type *region)
{
  return (region->body_wait & RV32_MIXED_BODY_MASK) << 2;
}

static u32 rv32_mixed_region_guest(const rv32_mixed_region_type *region)
{
  return RV32_MIXED_GUEST_BASE +
      ((region->guest_wait & RV32_MIXED_GUEST_MASK) << 1);
}

static rv32_mixed_region_type *rv32_mixed_regions;
static u32 rv32_mixed_region_count;
static u32 rv32_mixed_region_capacity;
static u32 rv32_rom_entry_count;
static u32 rv32_rom_entry_capacity;
static u32 rv32_rom_side_entry_count;
static rv32_entry_type *rv32_ram_entries;
static u32 rv32_ram_entry_count;
static u32 rv32_ram_entry_capacity;
enum { RV32_RAM_ENTRY_MAX_TABLE_BYTES = 32 * 1024 };

typedef struct
{
  u32 key;
  u32 pc;
  u32 offset_size;
} rv32_slow_entry_type;

enum {
  RV32_ROM_SLOW_ENTRIES = 512,
  RV32_RAM_SLOW_ENTRIES = 128
};

static rv32_slow_entry_type rv32_rom_slow_entries[RV32_ROM_SLOW_ENTRIES];
static rv32_slow_entry_type rv32_ram_slow_entries[RV32_RAM_SLOW_ENTRIES];

static u32 rv32_slow_key(u32 pc, u16 opcode)
{
  return 0x80000000U | (((pc + 2) >> 24) & 15) << 16 | opcode;
}

static u32 rv32_slow_pc(u32 pc, u16 opcode)
{
  return gpsp_rv32_thumb_batch_pc_sensitive(opcode) ? pc : 0;
}

static rv32_slow_entry_type *rv32_slow_slot(u32 pc, u16 opcode,
    bool ram_region)
{
  u32 key = rv32_slow_key(pc, opcode);
  u32 pc_key = rv32_slow_pc(pc, opcode);
  u32 hash = key * 2654435761U ^ pc_key * 2246822519U;
  hash ^= hash >> 16;
  return ram_region ?
      &rv32_ram_slow_entries[hash & (RV32_RAM_SLOW_ENTRIES - 1)] :
      &rv32_rom_slow_entries[hash & (RV32_ROM_SLOW_ENTRIES - 1)];
}

static u8 *rv32_slow_find(u32 pc, u16 opcode, bool ram_region,
    u32 *size)
{
  rv32_slow_entry_type *entry = rv32_slow_slot(pc, opcode, ram_region);
  if (entry->key != rv32_slow_key(pc, opcode) ||
      entry->pc != rv32_slow_pc(pc, opcode))
    return NULL;

  *size = gpsp_rv32_slow_size(entry->offset_size);
  u32 offset = gpsp_rv32_slow_offset(entry->offset_size);
  return ram_region ? rv32_ram_at(offset) : rv32_rom_at(offset);
}

/* A body the entry cannot hold stays in use by its block; only reuse is
   lost. */
static void rv32_slow_publish(u32 pc, u16 opcode, bool ram_region,
    u8 *target, u32 size)
{
  rv32_slow_entry_type *entry = rv32_slow_slot(pc, opcode, ram_region);
  u32 offset = ram_region ? rv32_ram_offset(target) : rv32_rom_offset(target);
  uint32_t offset_size;
  if (!gpsp_rv32_slow_pack(offset, size, &offset_size))
    return;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  if (entry->key && (entry->key != rv32_slow_key(pc, opcode) ||
      entry->pc != rv32_slow_pc(pc, opcode)))
  {
    if (ram_region)
      rv32_overlap_byte_stats.slow_ram_replacements++;
    else
      rv32_overlap_byte_stats.slow_rom_replacements++;
  }
#endif
  entry->key = rv32_slow_key(pc, opcode);
  entry->pc = rv32_slow_pc(pc, opcode);
  entry->offset_size = offset_size;
}

static void rv32_slow_commit(u32 pc, u16 opcode, bool ram_region,
    u8 *target, u32 size, bool emitted)
{
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  if (!emitted)
  {
    if (ram_region)
    {
      rv32_overlap_byte_stats.slow_ram_hits++;
      rv32_overlap_byte_stats.slow_ram_saved_bytes += size;
    }
    else
    {
      rv32_overlap_byte_stats.slow_rom_hits++;
      rv32_overlap_byte_stats.slow_rom_saved_bytes += size;
    }
  }
  else
  {
    if (ram_region)
      rv32_overlap_byte_stats.slow_ram_emitted++;
    else
      rv32_overlap_byte_stats.slow_rom_emitted++;
  }
#else
  (void)size;
#endif
  if (emitted)
    rv32_slow_publish(pc, opcode, ram_region, target, size);
}

/* The packed offset leaves room for the two five-bit batch distances. */
#if ROM_TRANSLATION_CACHE_SIZE <= 16 * 1024 * 1024
#define RV32_ROM_SIDE_ENTRIES 1
#define RV32_ROM_ENTRY_OFFSET_BITS 22
#define RV32_ROM_ENTRY_OFFSET_MASK ((1U << RV32_ROM_ENTRY_OFFSET_BITS) - 1)
#define RV32_ROM_SIDE_PREFIX(value) (((value) >> 22) & 31)
#define RV32_ROM_SIDE_REMAINING(value) ((value) >> 27)
#define RV32_ROM_ENTRY_OFFSET(value) (((value) & RV32_ROM_ENTRY_OFFSET_MASK) << 2)
/* A zero remaining count marks a lookup-only raw entry. */
#define RV32_ROM_UNGUARDED_PREFIX 1
#define RV32_ROM_ENTRY_UNGUARDED(value) \
  (RV32_ROM_SIDE_PREFIX(value) == RV32_ROM_UNGUARDED_PREFIX && \
   !RV32_ROM_SIDE_REMAINING(value))
#else
#define RV32_ROM_SIDE_ENTRIES 0
#define RV32_ROM_ENTRY_OFFSET(value) (value)
#endif
#if !RV32_ROM_SIDE_ENTRIES
#error "Flow guard elision needs packed ROM entry metadata"
#endif
enum { RV32_ROM_SIDE_ENTRY_MAX = 16384 };

#if defined(GPSP_BENCHMARK)
u64 rv32_rom_cache_discarded_bytes;
u64 rv32_ram_cache_discarded_bytes;
#endif

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
void gpsp_rv32_block_get_overlap_byte_stats(
    struct gpsp_rv32_overlap_byte_stats *stats)
{
  *stats = rv32_overlap_byte_stats;
  stats->alias_entries = rv32_rom_entry_count;
  stats->alias_table_bytes = rv32_rom_entry_capacity *
      sizeof(*rv32_rom_entries);
  stats->side_pending = rv32_rom_side_entry_count;
  stats->slow_rom_table_bytes = sizeof(rv32_rom_slow_entries);
  stats->slow_ram_table_bytes = sizeof(rv32_ram_slow_entries);
}
#endif

static u32 rv32_entry_slot(u32 key, u32 capacity)
{
  u32 hash = key * 2654435761U;
  return (hash ^ (hash >> 16)) & (capacity - 1);
}

/* Returns key's entry, or the empty slot that ends its probe. */
static rv32_entry_type *rv32_rom_entry_probe(u32 key)
{
  u32 slot = rv32_entry_slot(key, rv32_rom_entry_capacity);
  while (rv32_rom_entries[slot].key && rv32_rom_entries[slot].key != key)
    slot = (slot + 1) & (rv32_rom_entry_capacity - 1);
  return &rv32_rom_entries[slot];
}

static rv32_entry_type *rv32_rom_entry_slot(u32 key)
{
  if (!rv32_rom_entry_capacity)
    return NULL;

  rv32_entry_type *entry = rv32_rom_entry_probe(key);
  return entry->key ? entry : NULL;
}

static u32 rv32_rom_entry_value(u8 *target, unsigned prefix,
    unsigned remaining)
{
  u32 offset = rv32_rom_offset(target);
#if RV32_ROM_SIDE_ENTRIES
  return (offset >> 2) | (prefix << 22) | (remaining << 27);
#else
  (void)prefix;
  (void)remaining;
  return offset;
#endif
}

static bool rv32_rom_entry_target_valid(u8 *target)
{
  u32 offset = rv32_rom_offset(target);
#if RV32_ROM_SIDE_ENTRIES
  return gpsp_rv32_packed_offset_valid(offset, RV32_ROM_ENTRY_OFFSET_BITS);
#else
  return offset != UINT32_MAX && !(offset & 3);
#endif
}

static u8 *rv32_rom_entry_find(u32 key)
{
  rv32_entry_type *entry = rv32_rom_entry_slot(key);
#if RV32_ROM_SIDE_ENTRIES
  if (entry && RV32_ROM_SIDE_PREFIX(entry->offset))
    return NULL;
#endif
  return entry ? rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset)) : NULL;
}

static u8 *rv32_rom_entry_find_lookup(u32 key)
{
  rv32_entry_type *entry = rv32_rom_entry_slot(key);
  if (!entry)
    return NULL;
#if RV32_ROM_SIDE_ENTRIES
  if (RV32_ROM_ENTRY_UNGUARDED(entry->offset))
    return rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset));
  if (RV32_ROM_SIDE_PREFIX(entry->offset))
    return NULL;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  if (RV32_ROM_SIDE_REMAINING(entry->offset))
    rv32_overlap_byte_stats.side_entry_hits++;
#endif
#endif
  return rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset));
}

static bool rv32_rom_entry_grow(void)
{
  u32 capacity = rv32_rom_entry_capacity ? rv32_rom_entry_capacity * 2 : 1024;
  rv32_entry_type *entries = (rv32_entry_type *)calloc(capacity,
      sizeof(*entries));
  if (!entries)
  {
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    rv32_overlap_byte_stats.alias_alloc_failures++;
#endif
    return false;
  }

  for (u32 i = 0; i < rv32_rom_entry_capacity; i++)
  {
    rv32_entry_type old = rv32_rom_entries[i];
    if (!old.key)
      continue;
    u32 slot = rv32_entry_slot(old.key, capacity);
    while (entries[slot].key)
      slot = (slot + 1) & (capacity - 1);
    entries[slot] = old;
  }
  free(rv32_rom_entries);
  rv32_rom_entries = entries;
  rv32_rom_entry_capacity = capacity;
  return true;
}

/* Keeps an existing entry, except that a plain entry replaces a side
   entry when replace_side is set. */
static bool rv32_rom_entry_put(u32 key, u32 value, bool side,
    bool replace_side)
{
  rv32_entry_type *entry = rv32_rom_entry_capacity ?
      rv32_rom_entry_probe(key) : NULL;
  if (entry && entry->key)
  {
#if RV32_ROM_SIDE_ENTRIES
    if (!side && replace_side && RV32_ROM_SIDE_PREFIX(entry->offset))
    {
      if (!RV32_ROM_ENTRY_UNGUARDED(entry->offset))
        rv32_rom_side_entry_count--;
      entry->offset = value;
    }
#endif
    return true;
  }

  if (side && rv32_rom_side_entry_count >= RV32_ROM_SIDE_ENTRY_MAX)
    return false;

  if (rv32_rom_entry_count * 4 >= rv32_rom_entry_capacity * 3)
  {
    if (!rv32_rom_entry_grow())
      return false;
    entry = rv32_rom_entry_probe(key);
  }

  entry->key = key;
  entry->offset = value;
  rv32_rom_entry_count++;
  if (side)
    rv32_rom_side_entry_count++;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  if (rv32_rom_entry_count > rv32_overlap_byte_stats.alias_peak_entries)
    rv32_overlap_byte_stats.alias_peak_entries = rv32_rom_entry_count;
#endif
  return true;
}

static bool rv32_rom_entry_add(u32 key, u8 *target)
{
  if (!rv32_rom_entry_target_valid(target))
    return false;
  return rv32_rom_entry_put(key, rv32_rom_entry_value(target, 0, 0), false,
      true);
}

#if RV32_ROM_SIDE_ENTRIES
static bool rv32_rom_entry_add_unguarded(u32 key, u8 *target)
{
  if (!rv32_rom_entry_target_valid(target))
    return false;
  return rv32_rom_entry_put(key,
      rv32_rom_entry_value(target, RV32_ROM_UNGUARDED_PREFIX, 0), false,
      false);
}

static bool rv32_rom_entry_unguarded(u32 key)
{
  rv32_entry_type *entry = rv32_rom_entry_slot(key);
  return entry && RV32_ROM_ENTRY_UNGUARDED(entry->offset);
}

/* Give a linked raw entry its own budget guard. */
static u8 *rv32_rom_entry_guard_adapter(u32 key)
{
  rv32_entry_type *entry = rv32_rom_entry_slot(key);
  if (!entry || !RV32_ROM_ENTRY_UNGUARDED(entry->offset))
    return NULL;

  u8 *body = rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset));
  u8 *start = rv32_skip_to_word(rom_translation_ptr);
  u8 *limit = rv32_rom_limit();
  if (start > limit || limit - start < 12)
    return NULL;

  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, start, (size_t)(limit - start));
  if (!gpsp_rv32_flow_guard(&code))
    return NULL;
  intptr_t delta = (intptr_t)body - (intptr_t)code.ptr;
  if (delta >= -1048576 && delta <= 1048574)
  {
    if (!gpsp_rv32_jal(&code, X0, (int)delta))
      return NULL;
  }
  else
  {
    u8 *site;
    if (!gpsp_rv32_emit_link_slot(&code, &site) ||
        !gpsp_rv32_patch_link_slot(&code, site, (uintptr_t)body))
      return NULL;
  }
  if (!rv32_rom_entry_add(key, start))
    return NULL;
  rom_translation_ptr = code.ptr;
  return start;
}
#endif

#if RV32_ROM_SIDE_ENTRIES
static bool rv32_rom_entry_add_side(u32 key, u8 *body,
    unsigned prefix, unsigned remaining,
    unsigned prefix_cost, unsigned total_cost)
{
  if (!rv32_rom_entry_target_valid(body) || !prefix || !remaining ||
      prefix + remaining > 32)
    return false;
  if ((total_cost && (!prefix_cost || prefix_cost >= total_cost ||
          total_cost > 48)) || (!total_cost && prefix_cost))
    return false;
  return rv32_rom_entry_put(key,
      rv32_rom_entry_value(body, prefix, remaining), true, false);
}
#endif

static const rv32_mixed_region_type *rv32_mixed_region_find(
    u32 key, u32 body, unsigned prefix, unsigned remaining)
{
  u32 low = 0;
  u32 high = rv32_mixed_region_count;
  while (low < high)
  {
    u32 middle = low + (high - low) / 2;
    if (rv32_mixed_region_body(&rv32_mixed_regions[middle]) <= body)
      low = middle + 1;
    else
      high = middle;
  }
  if (!low)
    return NULL;
  const rv32_mixed_region_type *region = &rv32_mixed_regions[low - 1];
  u32 first = rv32_mixed_region_body(region);
  u32 span = ((region->literal_span >> 16) & RV32_MIXED_SPAN_MASK) << 2;
  unsigned length = (region->body_wait >> 22) & 31;
  return body - first <= span &&
      rv32_mixed_region_guest(region) == (key & ~1U) - prefix * 2 &&
      length == prefix + remaining ? region : NULL;
}

static bool rv32_mixed_region_prepare(u32 start, s32 count,
    const block_data_type *blocks,
    const u8 *side_prefix, const u8 *side_remaining,
    const u8 *side_prefix_cost, const u8 *side_total_cost,
    rv32_mixed_region_type *pending, unsigned *pending_count)
{
  *pending_count = 0;
  if (!side_prefix || !side_remaining || !side_prefix_cost ||
      !side_total_cost)
    return true;

  for (s32 i = 1; i < count; i++)
  {
    if (side_prefix[i] != 1 || !side_total_cost[i])
      continue;
    unsigned length = side_remaining[i] + 1;
    s32 last = i + side_remaining[i] - 1;
    bool memory_head =
        (side_total_cost[i] & GPSP_RV32_THUMB_FLOW_MEMORY_HEAD) != 0;
    if (length < 2 || last >= count ||
        *pending_count >= RV32_ROM_CACHE_THUMB_BLOCK_LIMIT / 2)
      return false;

    u32 guest_start = start + (u32)(i - 1) * 2;
    /* Match the waits captured by this generated region. */
    unsigned seq = gpsp_rv32_flow_fetch_bound(guest_start + 2, true);
    unsigned nseq = ws_cyc_nseq[guest_start >> 24][1];
    unsigned total = side_total_cost[i] & ~GPSP_RV32_THUMB_FLOW_MEMORY_HEAD;
    unsigned prior = 0;
    u32 literal_mask = 0;
    for (s32 j = i; j <= last; j++)
    {
      unsigned prefix_cost = side_prefix_cost[j];
      unsigned cost = prefix_cost - prior;
      if (side_prefix[j] != (unsigned)(j - i + 1) ||
          side_remaining[j] != (unsigned)(last - j + 1) ||
          (side_total_cost[j] & ~GPSP_RV32_THUMB_FLOW_MEMORY_HEAD) !=
              total ||
          ((side_total_cost[j] & GPSP_RV32_THUMB_FLOW_MEMORY_HEAD) != 0) !=
              memory_head ||
          prefix_cost < prior ||
          (cost != seq && cost != seq + nseq))
        return false;
      if (nseq && cost == seq + nseq)
        literal_mask |= 1U << (j - i);
      prior = prefix_cost;
    }
    unsigned final_cost = total - prior;
    /* A ROM word wait minus one equals branch nseq16 plus seq16. */
    bool branch_end =
        nseq > seq + 1 && final_cost == nseq - 1;
    if (prior >= total ||
        (!branch_end && final_cost != seq &&
         final_cost != seq + nseq))
      return false;
    if (!branch_end && nseq && final_cost == seq + nseq)
      literal_mask |= 1U << (length - 1);

    u8 *first = blocks[i].block_offset;
    u8 *last_body = blocks[last].block_offset;
    if (!rv32_rom_entry_target_valid(first) ||
        !rv32_rom_entry_target_valid(last_body) ||
        first > last_body ||
        length * seq + __builtin_popcount(literal_mask) * nseq +
            (branch_end ? nseq - 1 - seq : 0) != total)
      return false;
    u32 first_offset = rv32_rom_offset(first);
    u32 body_span = (u32)(last_body - first);
    if (length > 16 || seq > 15 || nseq > 31 ||
        literal_mask > RV32_MIXED_LITERAL_MASK ||
        first_offset >> 2 > RV32_MIXED_BODY_MASK ||
        body_span >> 2 > RV32_MIXED_SPAN_MASK ||
        guest_start < RV32_MIXED_GUEST_BASE ||
        guest_start >= 0x0e000000U || (guest_start & 1))
      return false;
    rv32_mixed_region_type *region = &pending[(*pending_count)++];
    /* Four-byte native units and two-byte guest units leave waits intact. */
    region->body_wait = (first_offset >> 2) | (length << 22) |
        (seq << 27);
    region->guest_wait = ((guest_start - RV32_MIXED_GUEST_BASE) >> 1) |
        (nseq << 26);
    region->literal_span = literal_mask | ((body_span >> 2) << 16) |
        (branch_end ? RV32_MIXED_BRANCH_END : 0) |
        (memory_head ? RV32_MIXED_MEMORY_HEAD : 0);
    if (*pending_count > 1 &&
        rv32_mixed_region_body(&pending[*pending_count - 2]) >=
        rv32_mixed_region_body(region))
      return false;
  }
  return true;
}

static bool rv32_mixed_region_reserve(unsigned additional)
{
  u32 needed = rv32_mixed_region_count + additional;
  if (needed <= rv32_mixed_region_capacity)
    return true;
  u32 capacity = rv32_mixed_region_capacity ?
      rv32_mixed_region_capacity * 2 : 16;
  while (capacity < needed)
    capacity *= 2;
  rv32_mixed_region_type *regions =
      (rv32_mixed_region_type *)realloc(rv32_mixed_regions,
          capacity * sizeof(*regions));
  if (!regions)
    return false;
  rv32_mixed_regions = regions;
  rv32_mixed_region_capacity = capacity;
  return true;
}

static void rv32_rom_entry_clear(void)
{
  if (rv32_rom_entries)
    memset(rv32_rom_entries, 0,
        rv32_rom_entry_capacity * sizeof(*rv32_rom_entries));
  rv32_mixed_region_count = 0;
  rv32_rom_entry_count = 0;
  rv32_rom_side_entry_count = 0;
}

static u8 *rv32_ram_entry_find(u32 key)
{
  if (!rv32_ram_entry_capacity)
    return NULL;

  u32 slot = rv32_entry_slot(key, rv32_ram_entry_capacity);
  while (rv32_ram_entries[slot].key)
  {
    if (rv32_ram_entries[slot].key == key)
      return rv32_ram_at(rv32_ram_entries[slot].offset);
    slot = (slot + 1) & (rv32_ram_entry_capacity - 1);
  }
  return NULL;
}

static bool rv32_ram_entry_add(u32 key, u8 *target)
{
  if (rv32_ram_entry_find(key))
    return true;

  if (rv32_ram_entry_count * 4 >= rv32_ram_entry_capacity * 3)
  {
    u32 capacity = rv32_ram_entry_capacity ? rv32_ram_entry_capacity * 2 : 256;
    if (capacity * sizeof(*rv32_ram_entries) >
        RV32_RAM_ENTRY_MAX_TABLE_BYTES)
      return false;
    rv32_entry_type *entries = (rv32_entry_type *)calloc(capacity,
        sizeof(*entries));
    if (!entries)
      return false;

    for (u32 i = 0; i < rv32_ram_entry_capacity; i++)
    {
      rv32_entry_type old = rv32_ram_entries[i];
      if (!old.key)
        continue;
      u32 slot = rv32_entry_slot(old.key, capacity);
      while (entries[slot].key)
        slot = (slot + 1) & (capacity - 1);
      entries[slot] = old;
    }
    free(rv32_ram_entries);
    rv32_ram_entries = entries;
    rv32_ram_entry_capacity = capacity;
  }

  u32 slot = rv32_entry_slot(key, rv32_ram_entry_capacity);
  while (rv32_ram_entries[slot].key)
    slot = (slot + 1) & (rv32_ram_entry_capacity - 1);
  rv32_ram_entries[slot].key = key;
  rv32_ram_entries[slot].offset = rv32_ram_offset(target);
  rv32_ram_entry_count++;
  return true;
}

static void rv32_ram_entry_clear(void)
{
  if (rv32_ram_entries)
    memset(rv32_ram_entries, 0,
        rv32_ram_entry_capacity * sizeof(*rv32_ram_entries));
  rv32_ram_entry_count = 0;
}

/* Backward-shift deletion: later entries of the probe chain move into the
   hole, so no tombstones are needed. */
static void rv32_ram_entry_remove_slot(u32 slot)
{
  u32 mask = rv32_ram_entry_capacity - 1;
  u32 hole = slot;
  u32 next = (slot + 1) & mask;
  while (rv32_ram_entries[next].key)
  {
    u32 home = rv32_entry_slot(rv32_ram_entries[next].key,
        rv32_ram_entry_capacity);
    /* Move it unless its home lies between the hole and its slot. */
    if (((next - home) & mask) >= ((next - hole) & mask))
    {
      rv32_ram_entries[hole] = rv32_ram_entries[next];
      hole = next;
    }
    next = (next + 1) & mask;
  }
  rv32_ram_entries[hole].key = 0;
  rv32_ram_entry_count--;
}

/* Removes entries into the given units, interior and mirror PCs included.
   Each unit lists the keys it entered; another unit may own a key now. */
static void rv32_ram_entry_drop(const u16 *ids, unsigned count)
{
  if (!rv32_ram_entry_capacity)
    return;
  u32 mask = rv32_ram_entry_capacity - 1;
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(ids[i]);
    const gpsp_rv32_ram_ref *refs = gpsp_rv32_ram_unit_refs(ids[i]);
    for (unsigned k = 0; k < unit->entries; k++)
    {
      u32 key = refs[k].key;
      u32 slot = rv32_entry_slot(key, rv32_ram_entry_capacity);
      while (rv32_ram_entries[slot].key &&
          rv32_ram_entries[slot].key != key)
        slot = (slot + 1) & mask;
      if (rv32_ram_entries[slot].key &&
          gpsp_rv32_ram_units_contain(rv32_ram_entries[slot].offset,
              ids, count))
        rv32_ram_entry_remove_slot(slot);
    }
  }
}

static bool rv32_ram_pc(u32 pc)
{
  u32 region = pc >> 24;
  return region == 0x02 || region == 0x03;
}

static bool rv32_canonical_ram_pc(u32 pc)
{
  /* Mirrored PCs share physical tags but have different PC-relative values. */
  u32 region = pc >> 24;
  return (region == 0x02 && !(pc & 0x00fc0000)) ||
      (region == 0x03 && !(pc & 0x00ff8000));
}

static bool rv32_rom_pc(u32 pc)
{
  u32 region = pc >> 24;
  return region >= 0x08 && region <= 0x0d;
}

/* BIOS and ROM code are immutable. RAM code inside the scanned block
   carries its SMC tags, so a write there flushes this translation too. */
static unsigned rv32_lookahead_bytes(u32 pc, u32 block_start, u32 block_end)
{
  u32 limit = 0x8000 - (pc & 0x7fff);
  if (rv32_ram_pc(pc) && pc >= block_start && pc < block_end)
    limit = MIN(limit, block_end - pc);
  else if (pc < 0x4000)
    limit = MIN(limit, 0x4000 - pc);
  else if (!rv32_rom_pc(pc))
    return 0;
  return memory_map_read[pc >> 15] ? limit : 0;
}

static u8 rv32_thumb_live_flags(u32 pc, u32 block_start, u32 block_end)
{
  u16 opcodes[8];
  unsigned count = (pc & 1) ? 0 :
      rv32_lookahead_bytes(pc, block_start, block_end) / 2;
  u8 *page = memory_map_read[(pc >> 15) & 0x1fff];
  count = MIN(count, 8);
  for (unsigned i = 0; i < count; i++)
    opcodes[i] = readaddress16(page, (pc + i * 2) & 0x7fff);
  return count ? gpsp_rv32_thumb_flow_live_flags(opcodes, count) : 15;
}

static u8 rv32_arm_live_flags(u32 pc, u32 block_start, u32 block_end)
{
  uint32_t opcodes[8];
  unsigned count = (pc & 3) ? 0 :
      rv32_lookahead_bytes(pc, block_start, block_end) / 4;
  u8 *page = memory_map_read[(pc >> 15) & 0x1fff];
  count = MIN(count, 8);
  for (unsigned i = 0; i < count; i++)
    opcodes[i] = readaddress32(page, (pc + i * 4) & 0x7fff);
  return count ? gpsp_rv32_arm_flow_live_flags(pc, opcodes, count) : 15;
}

static bool rv32_thumb_literal_value(u32 pc, u16 opcode, u8 *page,
    u32 *value)
{
  uint32_t address;
  if (!page || !value ||
      !gpsp_rv32_thumb_rom_literal_address(pc, opcode, gamepak_size,
          &address))
    return false;
  *value = readaddress32(page, address & 0x7fff);
  return true;
}

static bool rv32_jit_pc(u32 pc)
{
  return rv32_ram_pc(pc) || rv32_rom_pc(pc);
}

static u32 rv32_next_rom_thumb_entry(u32 pc)
{
  for (u32 distance = 2; distance <= 64; distance += 2)
  {
    u32 next = pc + distance;
    if (!rv32_rom_pc(next))
      break;
    if (rv32_rom_entry_slot(next | 1))
      return next;
  }
  return 0;
}

static u8 *rv32_published_rom_block(u32 key)
{
  u32 bucket = ((key * 2654435761U) >> (32 - ROM_BRANCH_HASH_BITS)) &
      (ROM_BRANCH_HASH_SIZE - 1);
  u32 offset = rom_branch_hash[bucket];
  while(offset)
  {
    hashhdr_type *header = (hashhdr_type *)rv32_rom_at(offset);
    if(header->pc_value == key)
      return (u8 *)header + sizeof(hashhdr_type) + block_prologue_size;
    offset = header->next_entry;
  }
  return rv32_rom_entry_find(key);
}

static u8 *rv32_published_ram_block(u32 key)
{
  u32 pc = key & ~1u;
  if (!rv32_ram_pc(pc))
    return NULL;
  if (rv32_canonical_ram_pc(pc))
  {
    u16 *tagp = (pc >> 24) == 0x02 ?
        (u16 *)(ewram + (pc & 0x3ffff) + 0x40000) :
        (u16 *)(iwram + (pc & 0x7fff));
    if (VALID_TAG(*tagp))
    {
      ramtag_type *entry = get_ram_tag(*tagp);
      u32 offset = (key & 1) ? entry->offset_thumb : entry->offset_arm;
      if (offset)
        return rv32_ram_at(offset);
    }
  }
  return rv32_ram_entry_find(key);
}

static bool rv32_patch_link(u8 *site, u8 *target, u8 *end, bool publish)
{
  /* The slot's chunk bounds it. Code after end exists only in the chunk
     being filled; an earlier chunk is written to its end. */
  gpsp_rv32_arena *arena = &gpsp_rv32_ram_arena;
  u32 offset = rv32_ram_offset(site);
  if (offset == UINT32_MAX)
  {
    arena = &gpsp_rv32_rom_arena;
    offset = rv32_rom_offset(site);
    if (offset == UINT32_MAX)
      return false;
  }
  u8 *start = arena->base[offset / GPSP_RV32_CHUNK_BYTES];
  u8 *chunk_end = start + GPSP_RV32_CHUNK_BYTES;
  /* A patch view of published code; link slots keep full length. */
  gpsp_rv32_code code =
  {
    .start = start,
    .ptr = end >= start && end <= chunk_end ? end : chunk_end,
    .end = chunk_end,
  };
  if (!gpsp_rv32_patch_link_slot(&code, site, (uintptr_t)target))
    return false;
  if(publish)
  {
    rv32_phase_count(phase_patch_syncs);
    platform_cache_sync(site, site + GPSP_RV32_LINK_SLOT_SIZE);
    if (!rv32_cache_sync_ok)
      return false;
  }
  return true;
}

static void rv32_clear_pending_links(void)
{
  rv32_pending_link_count = 0;
  rv32_pending_link_used = 0;
  rv32_pending_link_free = 0;
  rv32_pending_link_new = 0;
  memset(rv32_pending_link_buckets, 0, sizeof(rv32_pending_link_buckets));
}

static void rv32_clear_pending_ram_links(void)
{
  rv32_pending_ram_link_count = 0;
}

/* Tables that grow during a game; the next game starts them small again. */
void rv32_tables_free(void)
{
  free(rv32_rom_entries);
  rv32_rom_entries = NULL;
  rv32_rom_entry_capacity = 0;
  rv32_rom_entry_clear();
  free(rv32_ram_entries);
  rv32_ram_entries = NULL;
  rv32_ram_entry_capacity = 0;
  rv32_ram_entry_clear();
  free(rv32_mixed_regions);
  rv32_mixed_regions = NULL;
  rv32_mixed_region_capacity = 0;
  free(rv32_pending_links);
  rv32_pending_links = NULL;
  rv32_pending_link_capacity = 0;
  rv32_clear_pending_links();
  free(rv32_pending_ram_links);
  rv32_pending_ram_links = NULL;
  rv32_pending_ram_link_capacity = 0;
  rv32_clear_pending_ram_links();
}

static u32 rv32_pending_link_bucket(u32 key)
{
  u32 hash = key * 2654435761U;
  return (hash ^ (hash >> 16)) & (RV32_PENDING_LINK_BUCKETS - 1);
}

/* Slot indices are one-based so zero can mark an empty chain. */
static void rv32_remove_pending_link(u32 *link, u32 slot)
{
  rv32_pending_link_type *entry = &rv32_pending_links[slot - 1];
  *link = entry->next;
  entry->next = rv32_pending_link_free;
  rv32_pending_link_free = slot;
  rv32_pending_link_count--;
}

static bool rv32_record_link(u8 *site, u32 target, bool thumb,
 u8 *code_end)
{
  if (target == cheat_master_hook || (target & (thumb ? 1 : 3)))
    return true;

  if (target < 0x4000)
  {
    u32 site_offset = rv32_rom_offset(site);
    /* The pinned BIOS entry must never jump to a flushable BIOS block. */
    if (rv32_bios_pinning || site_offset < rom_cache_watermark ||
        site_offset == UINT32_MAX)
      return true;
  }
  else if (!rv32_rom_pc(target))
    return true;

  u32 key = target | (u32)thumb;
  u8 *published = rv32_published_rom_block(key);
  if (published)
    return rv32_patch_link(site, published, code_end, false);

  u32 slot = rv32_pending_link_free;
  if (slot)
    rv32_pending_link_free = rv32_pending_links[slot - 1].next;
  else
  {
    if (rv32_pending_link_used == rv32_pending_link_capacity)
    {
      u32 capacity = rv32_pending_link_capacity ?
          rv32_pending_link_capacity * 2 : 64;
      rv32_pending_link_type *links = (rv32_pending_link_type *)realloc(
          rv32_pending_links, capacity * sizeof(*links));
      if (!links)
        return true; /* The unlinked slot still returns to the dispatcher. */
      rv32_pending_links = links;
      rv32_pending_link_capacity = capacity;
    }
    slot = ++rv32_pending_link_used;
  }
  u32 bucket = rv32_pending_link_bucket(key);
  rv32_pending_link_type *entry = &rv32_pending_links[slot - 1];
  entry->site = site;
  entry->key = key;
  entry->next = rv32_pending_link_buckets[bucket];
  rv32_pending_link_buckets[bucket] = slot;
  if (rv32_rom_pc(target))
  {
    entry->next_new = rv32_pending_link_new;
    rv32_pending_link_new = slot;
  }
  rv32_pending_link_count++;
  return true;
}

static bool rv32_ram_site(const u8 *site)
{
  return gpsp_rv32_arena_contains(&gpsp_rv32_ram_arena, site);
}

/* RAM to RAM links are recorded so a dropped target can be unlinked. */
static bool rv32_patch_ram_link(u8 *site, u8 *target, u8 *end, bool publish,
    u32 key)
{
  if (!rv32_patch_link(site, target, end, publish))
    return false;
  if (rv32_ram_site(site))
    gpsp_rv32_ram_link_add(rv32_ram_offset(site), rv32_ram_offset(target),
        key);
  return true;
}

/* An unlinked slot that links once its target is translated. */
static void rv32_add_pending_ram_link(u8 *site, u32 key)
{
  if (rv32_pending_ram_link_count == rv32_pending_ram_link_capacity)
  {
    u32 capacity = rv32_pending_ram_link_capacity ?
        rv32_pending_ram_link_capacity * 2 : 64;
    rv32_pending_ram_link_type *links =
        (rv32_pending_ram_link_type *)realloc(
        rv32_pending_ram_links, capacity * sizeof(*links));
    if (!links)
      return; /* The unlinked slot still returns to the dispatcher. */
    rv32_pending_ram_links = links;
    rv32_pending_ram_link_capacity = capacity;
  }
  rv32_pending_ram_links[rv32_pending_ram_link_count].site = site;
  rv32_pending_ram_links[rv32_pending_ram_link_count].key = key;
  rv32_pending_ram_link_count++;
}

static bool rv32_record_ram_link(u8 *site, u32 target, bool thumb,
 u8 *code_end)
{
  if (!rv32_ram_pc(target) || target == cheat_master_hook ||
      (target & (thumb ? 1 : 3)))
    return true;

  u32 key = target | (u32)thumb;
  /* A revived unit relinks its exits from this list. */
  if (rv32_ram_site(site))
    gpsp_rv32_ram_stage_exit(key, rv32_ram_offset(site));
  u8 *published = rv32_published_ram_block(key);
  if (published)
    return rv32_patch_ram_link(site, published, code_end, false, key);

  rv32_add_pending_ram_link(site, key);
  return true;
}

static bool rv32_publish_links(u32 key, u8 *target)
{
  u32 *link = &rv32_pending_link_buckets[rv32_pending_link_bucket(key)];
  while (*link)
  {
    u32 slot = *link;
    rv32_pending_link_type *entry = &rv32_pending_links[slot - 1];
    if (entry->key != key)
    {
      link = &entry->next;
      continue;
    }
    if (!rv32_patch_link(entry->site, target,
            rom_translation_ptr, true))
      return false;
    rv32_remove_pending_link(link, slot);
  }
  return true;
}

static bool rv32_publish_rom_entry_links(u32 key)
{
  u32 *link = &rv32_pending_link_buckets[rv32_pending_link_bucket(key)];
  while (*link && rv32_pending_links[*link - 1].key != key)
    link = &rv32_pending_links[*link - 1].next;
  if (!*link)
    return true;

  u8 *target = rv32_rom_entry_find(key);
#if RV32_ROM_SIDE_ENTRIES
  if (!target && rv32_rom_entry_unguarded(key))
  {
    target = rv32_rom_entry_guard_adapter(key);
    if (!target)
    {
      /* Keep the slot unlinked when the cache cannot hold the adapter. */
      while (*link)
      {
        u32 slot = *link;
        if (rv32_pending_links[slot - 1].key == key)
          rv32_remove_pending_link(link, slot);
        else
          link = &rv32_pending_links[slot - 1].next;
      }
      return true;
    }
  }
#endif
  return !target || rv32_publish_links(key, target);
}

static bool rv32_publish_ram_links(u32 key, u8 *target)
{
  u32 i = 0;
  while (i < rv32_pending_ram_link_count)
  {
    if (rv32_pending_ram_links[i].key != key)
    {
      i++;
      continue;
    }
    if (!rv32_patch_ram_link(rv32_pending_ram_links[i].site, target,
            ram_translation_ptr, true, rv32_pending_ram_links[i].key))
      return false;
    rv32_pending_ram_links[i] =
        rv32_pending_ram_links[--rv32_pending_ram_link_count];
  }
  return true;
}

static bool rv32_publish_ram_entry_links(void)
{
  u32 i = 0;
  while (i < rv32_pending_ram_link_count)
  {
    u8 *target = rv32_ram_entry_find(rv32_pending_ram_links[i].key);
    if (!target)
    {
      i++;
      continue;
    }
    if (!rv32_patch_ram_link(rv32_pending_ram_links[i].site, target,
            ram_translation_ptr, true, rv32_pending_ram_links[i].key))
      return false;
    rv32_pending_ram_links[i] =
        rv32_pending_ram_links[--rv32_pending_ram_link_count];
  }
  return true;
}
#endif

u8 function_cc *block_lookup_translate_arm(u32 pc);
u8 function_cc *block_lookup_translate_thumb(u32 pc);

#if !defined(GPSP_RV32_JIT)
static bool defer_external_exit(u8 *branch_source, u32 branch_target,
 u32 thumb)
{
  if(deferred_exit_count == deferred_exit_capacity)
  {
    u32 new_capacity = deferred_exit_capacity ? deferred_exit_capacity * 2
                                              : 128;
    deferred_exit_type *new_exits = (deferred_exit_type *)realloc(
     deferred_exits, new_capacity * sizeof(deferred_exit_type));
    if(!new_exits)
      return false;
    deferred_exits = new_exits;
    deferred_exit_capacity = new_capacity;
  }

  deferred_exits[deferred_exit_count].branch_source = branch_source;
  deferred_exits[deferred_exit_count].branch_target = branch_target;
  deferred_exits[deferred_exit_count].thumb = thumb;
  deferred_exit_count++;
  return true;
}
#endif

/* Runs at translate_depth == 1 so that the nested lookups it performs do
   not drain recursively themselves; they only translate their block and
   queue its exits. Entries must be popped before looking up their target
   since the lookup can grow (and thus move) the worklist. */

static bool process_deferred_exits(void)
{
  while(deferred_exit_count)
  {
    u8 *branch_source;
    u32 branch_target;
    u32 thumb;
    u8 *translation_target;

    deferred_exit_count--;
    branch_source = deferred_exits[deferred_exit_count].branch_source;
    branch_target = deferred_exits[deferred_exit_count].branch_target;
    thumb         = deferred_exits[deferred_exit_count].thumb;

    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else if(thumb)
      translation_target = block_lookup_translate_thumb(branch_target);
    else
      translation_target = block_lookup_translate_arm(branch_target);

    if(!translation_target)
    {
      if(translation_failure == TRANSLATION_FAILURE_NONE)
        translation_failure = TRANSLATION_FAILURE_LOOKUP;
      return false;
    }

    generate_branch_patch_unconditional(branch_source, translation_target);
  }
  return true;
}

static void abandon_failed_translation(void)
{
#if defined(GPSP_RV32_JIT)
  if (translation_failure == TRANSLATION_FAILURE_NONE)
    translation_failure = TRANSLATION_FAILURE_LOOKUP;
  /* A deferred branch may already link blocks in either cache. */
  deferred_exit_count = 0;
  if (translation_failure == TRANSLATION_FAILURE_CACHE_FULL)
  {
    rv32_lookup_rom_capacity_flush();
    rv32_lookup_ram_capacity_flush();
  }
  flush_translation_cache_rom();
  flush_translation_cache_ram();
#else
  deferred_exit_count = 0;
#endif
}

#if defined(GPSP_RV32_JIT)
static u8 *rv32_rom_entry_create_side(u32 key, bool *fatal)
{
#if RV32_ROM_SIDE_ENTRIES
  rv32_entry_type *entry = rv32_rom_entry_slot(key);
  if (!entry || !(key & 1) || !RV32_ROM_SIDE_PREFIX(entry->offset))
    return NULL;
  if (RV32_ROM_ENTRY_UNGUARDED(entry->offset))
    return NULL;
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  u32 side_start = rv32_lookup_cycle_now();
  rv32_lookup_stats.side_create_calls++;
#endif

  const rv32_mixed_region_type *region = rv32_mixed_region_find(
      key, RV32_ROM_ENTRY_OFFSET(entry->offset),
      RV32_ROM_SIDE_PREFIX(entry->offset),
      RV32_ROM_SIDE_REMAINING(entry->offset));
  if (region)
  {
    unsigned prefix = RV32_ROM_SIDE_PREFIX(entry->offset);
    unsigned prefix_mask = (1U << prefix) - 1;
    unsigned seq = (region->body_wait >> 27) & 15;
    unsigned nseq = (region->guest_wait >> 26) & 31;
    unsigned length = (region->body_wait >> 22) & 31;
    u32 literal_mask = region->literal_span & RV32_MIXED_LITERAL_MASK;
    unsigned prefix_cost = prefix * seq +
        __builtin_popcount(literal_mask & prefix_mask) * nseq;
    unsigned total_cost = length * seq +
        __builtin_popcount(literal_mask) * nseq +
        ((region->literal_span & RV32_MIXED_BRANCH_END) ?
         nseq - 1 - seq : 0);
    u8 *body = rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset));
    u8 *start = rv32_skip_to_word(rom_translation_ptr);
    u8 *limit = rv32_rom_limit();
    /* The entry value must hold start before links to it are published. */
    if (!prefix_cost || prefix_cost >= total_cost || total_cost > 48 ||
        start > limit || !rv32_rom_entry_target_valid(start))
      goto failed;

    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, start, (size_t)(limit - start));
    /* Rewind PC to the value the body holds at this op: the region start,
       or just past the last RAM stub site before it. That site charged the
       ops before it, so refund only the ops from it up to this one. */
    u32 guest = rv32_mixed_region_guest(region);
    u32 synced = guest;
    unsigned charged = 0;
    if (region->literal_span & RV32_MIXED_MEMORY_HEAD)
    {
      u8 *page = memory_map_read[guest >> 15];
      if (!page)
        page = load_gamepak_page((guest >> 15) & 0x3FF);
      if (!page)
        goto failed;
      for (unsigned op = 0; op < prefix; op++)
      {
        u32 op_pc = guest + op * 2;
        u16 opcode = readaddress16(page, op_pc & 0x7FFF);
        bool memory = (opcode & 0xf800) == 0x4800 ||
            (opcode >= 0x5000 && opcode < 0xa000);
        if (memory && !(literal_mask & (1U << op)))
        {
          synced = op_pc + 2;
          charged = op * seq +
              __builtin_popcount(literal_mask & ((1U << op) - 1)) * nseq;
        }
      }
    }
    unsigned rewind = guest + prefix * 2 - synced;
    if (!gpsp_rv32_flow_guard(&code) ||
        !gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, prefix_cost - charged) ||
        (rewind && !gpsp_rv32_addi(&code, RV_PC, RV_PC, -(int)rewind)))
      goto failed;
    intptr_t delta = (intptr_t)body - (intptr_t)code.ptr;
    if (delta >= -1048576 && delta <= 1048574)
    {
      if (!gpsp_rv32_jal(&code, X0, (int)delta))
        goto failed;
    }
    else
    {
      u8 *site;
      if (!gpsp_rv32_emit_link_slot(&code, &site) ||
          !gpsp_rv32_patch_link_slot(&code, site, (uintptr_t)body))
        goto failed;
    }
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
    rv32_lookup_stats.side_emit_cycles +=
        rv32_lookup_cycle_now() - side_start;
    u32 side_publish_start = rv32_lookup_cycle_now();
    rv32_lookup_stats.side_publish_pending += rv32_pending_link_count;
    if (rv32_pending_link_count >
        rv32_lookup_stats.side_publish_max_pending)
      rv32_lookup_stats.side_publish_max_pending = rv32_pending_link_count;
#endif
    rom_translation_ptr = code.ptr;
    bool published = rv32_publish_links(key, start);
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
    rv32_lookup_stats.side_publish_cycles +=
        rv32_lookup_cycle_now() - side_publish_start;
#endif
    if (!published)
    {
      translation_failure = TRANSLATION_FAILURE_LOOKUP;
      abandon_failed_translation();
      *fatal = true;
      goto failed;
    }
    entry->offset = rv32_rom_entry_value(start, 0, 1);
    rv32_rom_side_entry_count--;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    rv32_overlap_byte_stats.side_entry_created++;
    rv32_overlap_byte_stats.side_entry_bytes +=
        (size_t)(code.ptr - start);
    if (region->literal_span & RV32_MIXED_BRANCH_END)
    {
      rv32_overlap_byte_stats.branch_side_created++;
      rv32_overlap_byte_stats.branch_side_bytes +=
          (size_t)(code.ptr - start);
    }
#endif
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
    rv32_lookup_stats.side_create_cycles +=
        rv32_lookup_cycle_now() - side_start;
#endif
    return start;
  }

  unsigned prefix = RV32_ROM_SIDE_PREFIX(entry->offset);
  unsigned remaining = RV32_ROM_SIDE_REMAINING(entry->offset);
  u32 pc = key & ~1U;
  u32 page_index = pc >> 15;
  u8 *page = memory_map_read[page_index];
  if (!page)
    page = load_gamepak_page(page_index & 0x3FF);
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  rv32_lookup_stats.side_page_cycles +=
      rv32_lookup_cycle_now() - side_start;
  u32 side_emit_start = rv32_lookup_cycle_now();
#endif
  if (!page)
    goto failed;

  u16 opcode = readaddress16(page, pc & 0x7FFF);
  u8 *body = rv32_rom_at(RV32_ROM_ENTRY_OFFSET(entry->offset));
  u8 *start = rv32_skip_to_word(rom_translation_ptr);
  u8 *cursor = start;
  u8 *slow_literal;
  u8 *limit = rv32_rom_limit();
  if (!rv32_rom_entry_target_valid(start) ||
      !gpsp_rv32_emit_thumb_side_entry(&cursor, limit, pc, opcode,
          (uintptr_t)&ws_cyc_seq[((pc + 2) >> 24) & 15][0],
          prefix, remaining, body,
          (uintptr_t)gpsp_rv32_thumb_side_preflight_stub, &slow_literal))
    goto failed;
  u32 slow_size = 0;
  u8 *slow = rv32_slow_find(pc, opcode, false, &slow_size);
  bool slow_emitted = !slow;
  if (slow_emitted)
  {
    slow = cursor;
    if (!gpsp_rv32_emit_thumb_batch_slow(&cursor, limit, pc, opcode))
      goto failed;
    /* Include an unused tail gap in the packed four-byte size. */
    cursor = rv32_skip_to_word(cursor);
    slow_size = (u32)(cursor - slow);
  }
  if (!slow || ((uintptr_t)slow & 3) ||
      (uintptr_t)slow > UINT32_MAX || slow_literal + sizeof(u32) > cursor)
    goto failed;
  u32 slow_target = (u32)(uintptr_t)slow;
  memcpy(slow_literal, &slow_target, sizeof(slow_target));
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  rv32_lookup_stats.side_emit_cycles +=
      rv32_lookup_cycle_now() - side_emit_start;
  u32 side_publish_start = rv32_lookup_cycle_now();
  rv32_lookup_stats.side_publish_pending += rv32_pending_link_count;
  if (rv32_pending_link_count >
      rv32_lookup_stats.side_publish_max_pending)
    rv32_lookup_stats.side_publish_max_pending = rv32_pending_link_count;
#endif

  rom_translation_ptr = cursor;
  bool published = rv32_publish_links(key, start);
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  rv32_lookup_stats.side_publish_cycles +=
      rv32_lookup_cycle_now() - side_publish_start;
#endif
  if (!published)
  {
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
    rv32_lookup_stats.side_create_failures++;
    rv32_lookup_stats.side_create_cycles +=
        rv32_lookup_cycle_now() - side_start;
#endif
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    rv32_overlap_byte_stats.side_entry_failures++;
#endif
    translation_failure = TRANSLATION_FAILURE_LOOKUP;
    abandon_failed_translation();
    *fatal = true;
    return NULL;
  }
  entry->offset = rv32_rom_entry_value(start, 0, 1);
  rv32_rom_side_entry_count--;
  rv32_slow_commit(pc, opcode, false, slow, slow_size, slow_emitted);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  rv32_overlap_byte_stats.side_entry_created++;
  rv32_overlap_byte_stats.side_entry_bytes += (size_t)(cursor - start);
#endif
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  rv32_lookup_stats.side_create_cycles +=
      rv32_lookup_cycle_now() - side_start;
#endif
  return start;

failed:
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  rv32_lookup_stats.side_create_failures++;
  rv32_lookup_stats.side_create_cycles +=
      rv32_lookup_cycle_now() - side_start;
#endif
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  rv32_overlap_byte_stats.side_entry_failures++;
#endif
  return NULL;
#else
  (void)key;
  (void)fatal;
  return NULL;
#endif
}
#endif

// This function will return a pointer to a translated block of code. If it
// doesn't exist it will translate it, if it does it will pass it back.

// type should be "arm", "thumb", or "dual." For arm or thumb the PC should
// be a real PC, for dual the least significant bit will determine if it's
// ARM or Thumb mode.

#define block_lookup_address_pc_arm()                                         \
  u32 thumb = 0;                                                              \
  pc &= ~0x03

#define block_lookup_address_pc_thumb()                                       \
  u32 thumb = 1;                                                              \
  pc &= ~0x01                                                                 \

/* Existing backends link in-progress blocks; RV32 publishes complete ones. */
#if defined(GPSP_RV32_JIT)
#define TRANSLATION_LINK_EARLY 0
#define rv32_lookup_rom_entry(key) rv32_rom_entry_find_lookup(key)
#define rv32_lookup_rom_side_entry(key, fatal) \
    rv32_rom_entry_create_side(key, fatal)
#define rv32_lookup_ram_entry(key) rv32_ram_entry_find(key)
#define rv32_ram_head_pc(pc) rv32_canonical_ram_pc(pc)
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
#define rv32_note_rom_alias_hit() (rv32_overlap_byte_stats.alias_hits++)
#else
#define rv32_note_rom_alias_hit() ((void)0)
#endif
#define rv32_publish_rom_links(key, target, result)                            \
  do                                                                          \
  {                                                                           \
    if ((result) && !rv32_publish_links(key, target))                          \
    {                                                                         \
      translation_failure = TRANSLATION_FAILURE_LOOKUP;                       \
      result = false;                                                         \
    }                                                                         \
  } while(0)
#define rv32_publish_ram_block_links(key, target, result)                      \
  do                                                                          \
  {                                                                           \
    if ((result) && !rv32_publish_ram_links(key, target))                      \
    {                                                                         \
      translation_failure = TRANSLATION_FAILURE_LOOKUP;                       \
      result = false;                                                         \
    }                                                                         \
  } while(0)
#else
#define TRANSLATION_LINK_EARLY 1
#define rv32_lookup_rom_entry(key) ((u8 *)NULL)
#define rv32_lookup_rom_side_entry(key, fatal) ((u8 *)NULL)
#define rv32_lookup_ram_entry(key) ((u8 *)NULL)
#define rv32_ram_head_pc(pc) true
#define rv32_note_rom_alias_hit() ((void)0)
#define rv32_publish_rom_links(key, target, result)                            \
  ((void)(key), (void)(target), (void)(result))
#define rv32_publish_ram_block_links(key, target, result)                      \
  ((void)(key), (void)(target), (void)(result))
#endif

#ifdef HAVE_VRAM_INTERPRETER
#define vram_lookup_region case 0x06:
#else
#define vram_lookup_region
#endif

#if defined(GPSP_RV32_JIT)
static u8 *rv32_revive_ram_unit(u32 pc, bool thumb, bool *failed);
#define rv32_lookup_ram_revive(pc, thumb, failed) \
  rv32_revive_ram_unit(pc, thumb, failed)
#define rv32_lookup_ram_stage() gpsp_rv32_ram_stage_begin()
#else
#define rv32_lookup_ram_revive(pc, thumb, failed) ((u8 *)NULL)
#define rv32_lookup_ram_stage() ((void)0)
#endif

#define block_lookup_translate_builder(type)                                  \
u8 function_cc *block_lookup_translate_##type(u32 pc)                         \
{                                                                             \
  u8 pcregion = (pc >> 24);                                                   \
  rv32_lookup_region_var(pcregion);                                          \
                                                                              \
  block_lookup_address_pc_##type();                                           \
                                                                              \
  switch(pcregion)                                                            \
  {                                                                           \
    case 0x2:                                                                 \
    case 0x3:                                                                 \
    {                                                                         \
      u16* tagp = (pcregion == 2) ? (u16 *)(ewram + (pc & 0x3FFFF) + 0x40000) \
                                  : (u16 *)(iwram + (pc & 0x7FFF));           \
      ramtag_type* trentry = VALID_TAG(*tagp) ? get_ram_tag(*tagp) : NULL;     \
      bool canonical = rv32_ram_head_pc(pc);                                  \
      if (!canonical || !trentry || !trentry->offset_##type)                  \
      {                                                                       \
        u8 *alias = rv32_lookup_ram_entry(pc | thumb);                        \
        if (alias)                                                            \
        {                                                                     \
          rv32_lookup_hit();                                                  \
          return alias;                                                       \
        }                                                                     \
      }                                                                       \
      /* Allocate a tag if not a valid one, and initialize header */          \
      if (!trentry) {                                                         \
        /* Out of tags: flush as a full cache does. */                        \
        if (ram_block_tag <= LAST_TAG_NUM)                                    \
        {                                                                     \
          translation_failure = TRANSLATION_FAILURE_CACHE_FULL;               \
          if (!translate_depth)                                               \
            abandon_failed_translation();                                     \
          return NULL;                                                        \
        }                                                                     \
        allocate_tag_##type(tagp);                                            \
        trentry = get_ram_tag(*tagp);                                         \
        trentry->offset_arm = 0;                                              \
        trentry->offset_thumb = 0;                                            \
      }                                                                       \
                                                                              \
      if (!canonical || !trentry->offset_##type) {                            \
        bool revive_failed = false;                                           \
        u8 *revived = rv32_lookup_ram_revive(pc, thumb, &revive_failed);      \
        if (revived)                                                          \
        {                                                                     \
          rv32_lookup_hit();                                                  \
          return revived;                                                     \
        }                                                                     \
        if (revive_failed)                                                    \
        {                                                                     \
          /* The lookup starts over after the flush. */                       \
          translation_failure = TRANSLATION_FAILURE_RETRY;                    \
          if (!translate_depth)                                               \
            abandon_failed_translation();                                     \
          return NULL;                                                        \
        }                                                                     \
        rv32_lookup_miss();                                                   \
        bool result;                                                          \
        u8 *blkptr;                                                           \
        u32 pending_offset;                                                   \
        rv32_reserve_ram();                                                   \
        translate_depth++;                                                    \
        rv32_lookup_translate_begin();                                        \
        do                                                                    \
        {                                                                     \
          blkptr = ram_translation_ptr + block_prologue_size;                 \
          pending_offset = rv32_ram_offset(blkptr);                           \
          if (TRANSLATION_LINK_EARLY && canonical)                            \
            trentry->offset_##type = pending_offset;                          \
          rv32_lookup_ram_stage();                                            \
          result = translate_block_##type(pc, true);                          \
        } while (rv32_retry_ram(result, blkptr - block_prologue_size));       \
        rv32_lookup_translate_end(result);                                   \
        if (!TRANSLATION_LINK_EARLY && result && canonical)                    \
          trentry->offset_##type = pending_offset;                            \
        rv32_publish_ram_block_links(pc | thumb, blkptr, result);              \
        if (result && translate_depth == 1)                                   \
          result = process_deferred_exits();                                  \
        translate_depth--;                                                    \
        if (!result && !translate_depth)                                      \
          abandon_failed_translation();                                       \
                                                                              \
        if (result)                                                           \
          return blkptr;                                                      \
      } else {                                                                \
        rv32_lookup_hit();                                                    \
        return rv32_ram_at(trentry->offset_##type);                           \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
                                                                              \
    vram_lookup_region                                                        \
    case 0x0:                                                                 \
    case 0x8 ... 0xD:                                                         \
    {                                                                         \
      u32 key = pc | thumb;                                                   \
      u32 hash_target = ((key * 2654435761U) >> (32 - ROM_BRANCH_HASH_BITS))  \
                                              & (ROM_BRANCH_HASH_SIZE - 1);   \
                                                                              \
      hashhdr_type *bhdr;                                                     \
      u32 blk_offset = rom_branch_hash[hash_target];                          \
      u32 *blk_offset_addr = &rom_branch_hash[hash_target];                   \
      while(blk_offset)                                                       \
      {                                                                       \
        bhdr = (hashhdr_type*)rv32_rom_at(blk_offset);                        \
        rv32_lookup_hash_probe();                                            \
        if(bhdr->pc_value == key)                                             \
        {                                                                     \
          rv32_lookup_hit();                                                  \
          return (u8 *)bhdr + sizeof(hashhdr_type) + block_prologue_size;     \
        }                                                                     \
                                                                              \
        blk_offset = bhdr->next_entry;                                        \
        blk_offset_addr = &bhdr->next_entry;                                  \
      }                                                                       \
      {                                                                       \
        u8 *entry = rv32_lookup_rom_entry(key);                               \
        if (entry)                                                            \
        {                                                                     \
          rv32_lookup_hit();                                                  \
          rv32_note_rom_alias_hit();                                          \
          return entry;                                                       \
        }                                                                     \
      }                                                                       \
      {                                                                       \
        bool fatal = false;                                                   \
        u8 *entry = rv32_lookup_rom_side_entry(key, &fatal);                  \
        if (entry)                                                            \
        {                                                                     \
          rv32_lookup_hit();                                                  \
          rv32_note_rom_alias_hit();                                          \
          return entry;                                                       \
        }                                                                     \
        if (fatal)                                                            \
          return NULL;                                                        \
      }                                                                       \
                                                                              \
      { /* Not found, go ahead and translate, and backfill the hash table */  \
        rv32_lookup_miss();                                                   \
        u8 *blkptr;                                                           \
        bool result;                                                          \
        u32 pending_offset;                                                   \
        rv32_reserve_rom();                                                   \
        translate_depth++;                                                    \
        rv32_lookup_translate_begin();                                        \
        do                                                                    \
        {                                                                     \
          bhdr = (hashhdr_type*)rom_translation_ptr;                          \
          bhdr->pc_value = key;                                               \
          bhdr->next_entry = 0;                                               \
          pending_offset = rv32_rom_offset((u8*)bhdr);                        \
          if (TRANSLATION_LINK_EARLY)                                         \
            *blk_offset_addr = pending_offset;                                \
          rom_translation_ptr += sizeof(hashhdr_type);                        \
          blkptr = rom_translation_ptr + block_prologue_size;                 \
          result = translate_block_##type(pc, false);                         \
        } while (rv32_retry_rom(result, bhdr));                               \
        rv32_lookup_translate_end(result);                                   \
        if (!TRANSLATION_LINK_EARLY && result)                                 \
          *blk_offset_addr = pending_offset;                                  \
        u32 publish_start = rv32_phase_start();                               \
        rv32_publish_rom_links(key, blkptr, result);                           \
        if (result && translate_depth == 1)                                   \
          result = process_deferred_exits();                                  \
        rv32_phase_add(phase_publish_cycles, publish_start);                  \
        rv32_benchmark_publish_sites(result);                                 \
        translate_depth--;                                                    \
        if (!result && !translate_depth)                                      \
          abandon_failed_translation();                                       \
                                                                              \
        if (result)                                                           \
          return blkptr;                                                      \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
  }                                                                           \
                                                                              \
  /* Do not return NULL since it could indeed happen that some branch         \
     points to some random place (perhaps due to being garbage). This can     \
     happen when especulatively compiling code in RAM. Perhaps the game       \
     patches these instructions later, which would trigger a flush */         \
  return (u8*)(~0);                                                           \
}                                                                             \

block_lookup_translate_builder(arm);
block_lookup_translate_builder(thumb);

u8 function_cc *block_lookup_address_dual(u32 pc)
{
  u32 thumb = pc & 0x01;
  if(thumb) {
    pc &= ~1;
    reg[REG_CPSR] |= 0x20;
    return block_lookup_address_thumb(pc);
  } else {
    pc = (pc + 2) & ~0x03;
    reg[REG_CPSR] &= ~0x20;
    return block_lookup_address_arm(pc);
  }
}

u8 function_cc *block_lookup_address_arm(u32 pc)
{
  /* Sole gateway for all writes to the translation caches: block
   * headers, RAM tag entries, emitted code and branch patches all
   * happen under block_lookup_translate below. Open the per-thread
   * write window here and return to the executable state before any
   * translated code can run again (a no-op outside Apple silicon). */
  unsigned i;
  rv32_lookup_top_begin();
  jit_write_enable();
  for (i = 0; i < 4; i++) {
#if defined(GPSP_RV32_JIT)
    /* Each attempt reports its own sync result. */
    rv32_cache_sync_ok = true;
#endif
    u8 *ret = block_lookup_translate_arm(pc);
    if (ret) {
      translate_icache_sync();
      jit_exec_enable();
#if defined(GPSP_RV32_JIT)
      if (!rv32_cache_sync_ok)
      {
        rv32_lookup_top_end();
        return NULL;
      }
#endif
      rv32_lookup_top_end();
      return ret;
    }
#if defined(GPSP_RV32_JIT)
    if (translation_failure != TRANSLATION_FAILURE_CACHE_FULL &&
        translation_failure != TRANSLATION_FAILURE_RETRY)
      break;
#endif
  }
  jit_exec_enable();

#if defined(GPSP_RV32_JIT)
  rv32_lookup_top_end();
  return NULL;
#else
  printf("bad jump %x (%x)\n", pc, reg[REG_PC]);
  fflush(stdout);
  return NULL;
#endif
}

u8 function_cc *block_lookup_address_thumb(u32 pc)
{
  /* See block_lookup_address_arm for the W^X window rationale. */
  unsigned i;
  rv32_lookup_top_begin();
  jit_write_enable();
  for (i = 0; i < 4; i++) {
#if defined(GPSP_RV32_JIT)
    /* Each attempt reports its own sync result. */
    rv32_cache_sync_ok = true;
#endif
    u8 *ret = block_lookup_translate_thumb(pc);
    if (ret) {
#if defined(GPSP_RV32_JIT)
      if (last_rom_translation_ptr != rom_translation_ptr ||
          last_ram_translation_ptr != ram_translation_ptr)
      {
        u32 sync_start = rv32_phase_start();
        translate_icache_sync();
        rv32_phase_add(phase_sync_cycles, sync_start);
      }
#else
      translate_icache_sync();
#endif
      jit_exec_enable();
#if defined(GPSP_RV32_JIT)
      if (!rv32_cache_sync_ok)
      {
        rv32_lookup_top_end();
        return NULL;
      }
#endif
      rv32_lookup_top_end();
      return ret;
    }
#if defined(GPSP_RV32_JIT)
    if (translation_failure != TRANSLATION_FAILURE_CACHE_FULL &&
        translation_failure != TRANSLATION_FAILURE_RETRY)
      break;
#endif
  }
  jit_exec_enable();
#if defined(GPSP_RV32_JIT)
  rv32_lookup_top_end();
  return NULL;
#else
  printf("bad jump %x (%x)\n", pc, reg[REG_PC]);
  fflush(stdout);
  return NULL;
#endif
}


// Potential exit point: If the rd field is pc for instructions is 0x0F,
// the instruction is b/bl/bx, or the instruction is ldm with PC in the
// register list.
// All instructions with upper 3 bits less than 100b have an rd field
// except bx, where the bits must be 0xF there anyway, multiplies,
// which cannot have 0xF in the corresponding fields, and msr, which
// has 0x0F there but doesn't end things (therefore must be special
// checked against). Because MSR and BX overlap both are checked for.

#define arm_exit_point                                                        \
 (((opcode < 0x8000000) && ((opcode & 0x000F000) == 0x000F000) &&             \
  ((opcode & 0xDB0F000) != 0x120F000)) ||                                     \
  ((opcode & 0x12FFF10) == 0x12FFF10) ||                                      \
  ((opcode & 0x8108000) == 0x8108000) ||                                      \
  ((opcode >= 0xA000000) && (opcode < 0xF000000)) ||                          \
  ((opcode >= 0xF000000) && (!is_div_swi((opcode >> 16) & 0xFF))))            \

#define arm_opcode_branch                                                     \
  ((opcode & 0xE000000) == 0xA000000)                                         \

#define arm_opcode_swi                                                        \
  ((opcode & 0xF000000) == 0xF000000)                                         \

#define arm_opcode_unconditional_branch                                       \
  (condition == 0x0E)                                                         \

#define arm_load_opcode()                                                     \
  opcode = readaddress32(pc_address_block, (block_end_pc & 0x7FFF));          \
  condition = opcode >> 28;                                                   \
                                                                              \
  opcode &= 0xFFFFFFF;                                                        \
                                                                              \
  block_end_pc += 4                                                           \

#define arm_branch_target()                                                   \
  branch_target = (block_end_pc + 4 + (((s32)((u32)(opcode << 8))) >> 6))     \

// Contiguous conditional block flags modification - it will set 0x20 in the
// condition's bits if this instruction modifies flags. Taken from the CPU
// switch so it'd better be right this time.

#define arm_set_condition(_condition)                                         \
  block_data[block_data_position].condition = _condition;                     \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x01:                                                                \
    case 0x03:                                                                \
    case 0x09:                                                                \
    case 0x0B:                                                                \
    case 0x0D:                                                                \
    case 0x0F:                                                                \
      if((((opcode >> 5) & 0x03) == 0) || ((opcode & 0x90) != 0x90))          \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
    case 0x07:                                                                \
    case 0x11:                                                                \
    case 0x13:                                                                \
    case 0x15 ... 0x17:                                                       \
    case 0x19:                                                                \
    case 0x1B:                                                                \
    case 0x1D:                                                                \
    case 0x1F:                                                                \
      if((opcode & 0x90) != 0x90)                                             \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if(((opcode & 0x90) != 0x90) && !(opcode & 0x10))                       \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
    case 0x23:                                                                \
    case 0x25:                                                                \
    case 0x27:                                                                \
    case 0x29:                                                                \
    case 0x2B:                                                                \
    case 0x2D:                                                                \
    case 0x2F ... 0x37:                                                       \
    case 0x39:                                                                \
    case 0x3B:                                                                \
    case 0x3D:                                                                \
    case 0x3F:                                                                \
      block_data[block_data_position].condition |= 0x20;                      \
    break;                                                                    \
  }                                                                           \

#define arm_instruction_width 4

#define arm_base_cycles()                                                     \
  cycle_count += def_seq_cycles[pc >> 24][1]                                  \

// For now this just sets a variable that says flags should always be
// computed.

#define arm_dead_flag_eliminate()                                             \
  flag_status = 0xF                                                           \

// The following Thumb instructions can exit:
// b, bl, bx, swi, pop {... pc}, and mov pc, ..., the latter being a hireg
// op only. Rather simpler to identify than the ARM set.

#define thumb_exit_point                                                      \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   (((opcode & 0xFF00) == 0xDF00) &&                                          \
    (!is_div_swi(opcode & 0xFF))) ||                                          \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   ((opcode & 0xFF00) == 0x4700) ||                                           \
   ((opcode & 0xFF00) == 0xBD00) ||                                           \
   ((opcode & 0xFF87) == 0x4687) ||                                           \
   ((opcode >= 0xF800)))                                                      \

#define thumb_opcode_branch                                                   \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   (opcode >= 0xF800))                                                        \

#define thumb_opcode_swi                                                      \
  ((opcode & 0xFF00) == 0xDF00)                                               \

#define thumb_opcode_unconditional_branch                                     \
  ((opcode < 0xD000) || (opcode >= 0xDF00))                                   \

#define thumb_load_opcode()                                                   \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (block_end_pc & 0x7FFF));          \
                                                                              \
  block_end_pc += 2                                                           \

#define thumb_branch_target()                                                 \
  if(opcode < 0xE000)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s8)(opcode & 0xFF) * 2);             \
  }                                                                           \
  else                                                                        \
                                                                              \
  if(opcode < 0xF800)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s32)((opcode & 0x7FF) << 21) >> 20); \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                     \
    {                                                                         \
      branch_target =                                                         \
       (block_end_pc + ((s32)((last_opcode & 0x07FF) << 21) >> 9) +           \
       ((opcode & 0x07FF) * 2));                                              \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      goto no_direct_branch;                                                  \
    }                                                                         \
  }                                                                           \

#define thumb_set_condition(_condition)                                       \

#define thumb_instruction_width 2

#define thumb_base_cycles()                                                   \
  cycle_count += def_seq_cycles[pc >> 24][0]                                  \

// Here's how this works: each instruction has three different sets of flag
// attributes, each consisiting of a 4bit mask describing how that instruction
// interacts with the 4 main flags (N/Z/C/V).
// The first set, in bits 0:3, is the set of flags the instruction may
// modify. After this pass this is changed to the set of flags the instruction
// should modify - if the bit for the corresponding flag is not set then code
// does not have to be generated to calculate the flag for that instruction.

// The second set, in bits 7:4, is the set of flags that the instruction must
// modify (ie, for shifts by the register values the instruction may not
// always modify the C flag, and thus the C bit won't be set here).

// The third set, in bits 11:8, is the set of flags that the instruction uses
// in its computation, or the set of flags that will be needed after the
// instruction is done. For any instructions that change the PC all of the
// bits should be set because it is (for now) unknown what flags will be
// needed after it arrives at its destination. Instructions that use the
// carry flag as input will have it set as well.

// The algorithm is a simple liveness analysis procedure: It starts at the
// bottom of the instruction stream and sets a "currently needed" mask to
// the flags needed mask of the current instruction. Then it moves down
// an instruction, ANDs that instructions "should generate" mask by the
// "currently needed" mask, then ANDs the "currently needed" mask by
// the 1's complement of the instruction's "must generate" mask, and ORs
// the "currently needed" mask by the instruction's "flags needed" mask.

#define thumb_dead_flag_eliminate()                                           \
{                                                                             \
  u32 needed_mask = 0xff;                                                     \
                                                                              \
  while(--block_data_position >= 0)                                           \
  {                                                                           \
    flag_status = block_data[block_data_position].flag_data;                  \
    block_data[block_data_position].flag_data =                               \
     (flag_status & needed_mask);                                             \
    needed_mask &= ~((flag_status >> 4) & 0x0F);                              \
    needed_mask |= flag_status >> 8;                                          \
  }                                                                           \
}                                                                             \

#define MAX_BLOCK_SIZE   1024   // 2/4KiB blocks max
#define MAX_EXITS          32   // This covers 99% blocks

block_data_type block_data[MAX_BLOCK_SIZE];
block_exit_type block_exits[MAX_EXITS];

#if defined(GPSP_RV32_JIT)
static u32 rv32_thumb_entry_mask[
    (RV32_ROM_CACHE_THUMB_BLOCK_LIMIT + 31) / 32];
static u32 rv32_thumb_unguarded_mask[
    (RV32_ROM_CACHE_THUMB_BLOCK_LIMIT + 31) / 32];
static u8 rv32_thumb_side_prefix[RV32_ROM_CACHE_THUMB_BLOCK_LIMIT];
static u8 rv32_thumb_side_remaining[RV32_ROM_CACHE_THUMB_BLOCK_LIMIT];
static u8 rv32_thumb_side_prefix_cost[RV32_ROM_CACHE_THUMB_BLOCK_LIMIT];
static u8 rv32_thumb_side_total_cost[RV32_ROM_CACHE_THUMB_BLOCK_LIMIT];
static rv32_mixed_region_type rv32_thumb_mixed_pending[
    RV32_ROM_CACHE_THUMB_BLOCK_LIMIT / 2];

static bool rv32_record_ram_entries(u32 start, s32 count, bool thumb,
    const u32 *safe_entries)
{
  /* A physical RAM tag cannot identify which virtual mirror owns a head. */
  s32 first = rv32_canonical_ram_pc(start) ? 1 : 0;
  for (s32 i = first; i < count; i++)
  {
    u32 pc = start + (u32)i * (thumb ? 2 : 4);
    if ((pc >> 24) != (start >> 24))
      break;
    if (safe_entries &&
        !(safe_entries[i >> 5] & (1U << (i & 31))))
      continue;
    if (pc == cheat_master_hook)
      continue;
    /* Unindexed entries retain the normal block lookup path. */
    if (!rv32_ram_entry_add(pc | (u32)thumb,
            block_data[i].block_offset))
      break;
    gpsp_rv32_ram_stage_entry(pc | (u32)thumb,
        rv32_ram_offset(block_data[i].block_offset));
  }
  return rv32_publish_ram_entry_links();
}

static bool rv32_record_rom_entries(u32 start, s32 count, bool thumb,
    const u32 *thumb_safe_mask, const u32 *thumb_unguarded_mask,
    const u8 *side_prefix,
    const u8 *side_remaining, const u8 *side_prefix_cost,
    const u8 *side_total_cost, const u32 *arm_safe_entries)
{
  if (thumb && (!thumb_safe_mask ||
      count > RV32_ROM_CACHE_THUMB_BLOCK_LIMIT))
    return false;
  unsigned mixed_count = 0;
  if (thumb)
  {
    if (!rv32_mixed_region_prepare(start, count, block_data,
            side_prefix, side_remaining, side_prefix_cost,
            side_total_cost, rv32_thumb_mixed_pending, &mixed_count) ||
        !rv32_mixed_region_reserve(mixed_count) ||
        (mixed_count && rv32_mixed_region_count &&
         rv32_mixed_region_body(
             &rv32_mixed_regions[rv32_mixed_region_count - 1]) >=
         rv32_mixed_region_body(&rv32_thumb_mixed_pending[0])))
      return false;
    if (mixed_count)
    {
      memcpy(rv32_mixed_regions + rv32_mixed_region_count,
          rv32_thumb_mixed_pending,
          mixed_count * sizeof(*rv32_thumb_mixed_pending));
      rv32_mixed_region_count += mixed_count;
    }
  }
  for (s32 i = 0; i < count; i++)
  {
    if (thumb && !(thumb_safe_mask[i >> 5] & (1U << (i & 31))))
      continue;
    if (!thumb && arm_safe_entries &&
        !(arm_safe_entries[i >> 5] & (1U << (i & 31))))
      continue;
    u32 key = start + (u32)i * (thumb ? 2 : 4) + (u32)thumb;
    if (thumb && thumb_unguarded_mask &&
        (thumb_unguarded_mask[i >> 5] & (1U << (i & 31))))
    {
      if (!rv32_rom_entry_add_unguarded(key,
              block_data[i].block_offset))
        break;
      continue;
    }
    if (!rv32_rom_entry_add(key, block_data[i].block_offset))
      break;
  }
#if RV32_ROM_SIDE_ENTRIES
  if (thumb && side_prefix && side_remaining)
  {
    for (s32 i = 1; i < count; i++)
    {
      if (!side_prefix[i])
        continue;
      u32 key = start + (u32)i * 2 + 1;
      if (!rv32_rom_entry_add_side(key, block_data[i].block_offset,
              side_prefix[i], side_remaining[i],
              side_prefix_cost ? side_prefix_cost[i] : 0,
              side_total_cost ? side_total_cost[i] &
                  ~GPSP_RV32_THUMB_FLOW_MEMORY_HEAD : 0))
      {
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
        rv32_overlap_byte_stats.side_entry_failures++;
#endif
        break;
      }
    }
  }
#else
  (void)side_prefix;
  (void)side_remaining;
#endif
  if (!rv32_pending_link_count)
  {
    rv32_pending_link_new = 0;
    return true;
  }
  for (s32 i = 0; i < count; i++)
  {
    if (thumb && !(thumb_safe_mask[i >> 5] & (1U << (i & 31))))
      continue;
    if (!thumb && arm_safe_entries &&
        !(arm_safe_entries[i >> 5] & (1U << (i & 31))))
      continue;
    u32 key = start + (u32)i * (thumb ? 2 : 4) + (u32)thumb;
    if (!rv32_publish_rom_entry_links(key))
      return false;
  }
  /* Also resolve links to raw entries published by earlier blocks. */
  u32 new_slot = rv32_pending_link_new;
  rv32_pending_link_new = 0;
  while (new_slot)
  {
    rv32_pending_link_type *entry = &rv32_pending_links[new_slot - 1];
    u32 next_new = entry->next_new;
    if (!rv32_publish_rom_entry_links(entry->key))
      return false;
    new_slot = next_new;
  }
  return true;
}

typedef struct
{
  u8 *site;
  u32 pc;
  u16 opcode;
  u8 *target;
  u16 size;
  bool emitted;
} rv32_thumb_slow_site_type;

static rv32_thumb_slow_site_type rv32_thumb_slow_sites[
    (MAX_BLOCK_SIZE + 2) / 3];

static unsigned rv32_thumb_span_length(u32 pc, u32 end_pc,
    s32 position, unsigned scanned_count, const u8 *pc_address_block)
{
  if (!rv32_jit_pc(pc) || !pc_address_block)
    return 0;

  u32 fetch_region = (pc + 2) >> 24;
  u32 page = pc >> 15;
  unsigned length = 0;
  while (length < 32 && pc < end_pc &&
      (unsigned)position + length < scanned_count)
  {
    if ((length && block_data[position + length].update_cycles) ||
        (pc >> 15) != page || ((pc + 2) >> 24) != fetch_region ||
        pc == cheat_master_hook || pc + 2 == idle_loop_target_pc ||
        !gpsp_rv32_thumb_batch_alu(readaddress16(pc_address_block,
            pc & 0x7FFF)))
      break;
    length++;
    pc += 2;
  }
  return length >= 3 ? length : 0;
}

static void rv32_thumb_span_flags(s32 position, unsigned length)
{
  u32 needed = 0xF;
  for (s32 i = position + length - 1; i >= position; i--)
  {
    u16 status = block_data[i].flag_data;
    block_data[i].flag_data = status & needed;
    needed &= ~((status >> 4) & 0xF);
    needed |= status >> 8;
  }
}
#endif

#define smc_write_arm_yes() {                                                 \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  u32 tags = address32(pc_address_block,                                    \
      (block_end_pc & 0x7FFF) + offset);                                      \
  /* ARM code may overlap an existing Thumb tag. */                          \
  if (!(tags & 0xFFFF))                                                       \
    tags |= CODE_TAG_BLOCK16;                                                 \
  if (!(tags & 0xFFFF0000))                                                   \
    tags |= CODE_TAG_BLOCK16 << 16;                                           \
  address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) = tags;       \
}

#define smc_write_thumb_yes() {                                               \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
  }                                                                           \
}

#define smc_write_arm_no()                                                    \

#define smc_write_thumb_no()                                                  \

#if defined(GPSP_RV32_JIT)
/* End RAM blocks before learned self-writes so the writer survives
   invalidation of the rewritten code. */
#define RV32_RAM_CUTS 8
static struct
{
  u32 low, at;
  bool thumb;
} rv32_ram_cuts[RV32_RAM_CUTS];
static unsigned rv32_ram_cut_count, rv32_ram_cut_next;

static u32 rv32_ram_physical(u32 pc)
{
  return (pc >> 24) == 0x02 ? 0x02000000 | (pc & 0x3ffff) :
      0x03000000 | (pc & 0x7fff);
}

static bool rv32_ram_cut(u32 start, u32 end, bool thumb)
{
  start = rv32_ram_physical(start);
  end = rv32_ram_physical(end);
  for (unsigned i = 0; i < rv32_ram_cut_count; i++)
  {
    if (rv32_ram_cuts[i].at == end && rv32_ram_cuts[i].thumb == thumb &&
        start >= rv32_ram_cuts[i].low && start < end)
      return true;
  }
  return false;
}

static void rv32_ram_cut_note(u32 low, u32 at, bool thumb)
{
  for (unsigned i = 0; i < rv32_ram_cut_count; i++)
  {
    if (rv32_ram_cuts[i].at == at && rv32_ram_cuts[i].thumb == thumb)
    {
      rv32_ram_cuts[i].low = MIN(rv32_ram_cuts[i].low, low);
      return;
    }
  }
  unsigned i = rv32_ram_cut_count < RV32_RAM_CUTS ? rv32_ram_cut_count++ :
      rv32_ram_cut_next++ % RV32_RAM_CUTS;
  rv32_ram_cuts[i].low = low;
  rv32_ram_cuts[i].at = at;
  rv32_ram_cuts[i].thumb = thumb;
}

/* A switch slot links like a branch, so it counts as an exit. */
#define rv32_scan_end_arm()                                                   \
  if (block_end_pc >= 0x0E000000 ||                                         \
      (block_start_pc < 0x4000 && block_end_pc >= 0x4000) ||                 \
      block_exit_position + rv32_scan_switches >= MAX_EXITS ||              \
      (ram_region && rv32_ram_cut(block_start_pc, block_end_pc, false))) break
#define rv32_scan_track_arm()                                                 \
  do                                                                         \
  {                                                                          \
    if (ram_region && condition == 14 && opcode == 0x1A00000 &&              \
        gpsp_rv32_arm_flow_switch_known(block_end_pc - 4))                    \
      rv32_scan_switches++;                                                  \
  } while(0)
/* Helpers can inspect every flag after a non-batched instruction. */
#define rv32_scan_track_thumb()                                              \
  do                                                                         \
  {                                                                          \
    if (!gpsp_rv32_thumb_batch_alu((u16)opcode))                              \
      block_data[block_data_position].flag_data |= 0xF00;                    \
  } while(0)
#define rv32_scan_end_thumb()                                                \
  if ((!ram_region &&                                                        \
       block_data_position >= RV32_ROM_CACHE_THUMB_BLOCK_LIMIT) ||           \
      (rv32_thumb_join_pc && block_end_pc == rv32_thumb_join_pc) ||          \
      block_end_pc >= 0x0E000000 ||                                          \
      (block_start_pc < 0x4000 && block_end_pc >= 0x4000) ||                 \
      block_end_pc - 2 == cheat_master_hook ||                               \
      (gpsp_rv32_thumb_needs_helper((u16)opcode) &&                          \
       !(opcode >= 0x4800 && opcode < 0xA000) &&                             \
       !gpsp_rv32_thumb_multi_native((u16)opcode)) ||                        \
      (opcode & 0xFF87) == 0x4487 ||                                         \
      (ram_region && rv32_ram_cut(block_start_pc, block_end_pc, true))) break
#else
#define rv32_scan_end_arm()
#define rv32_scan_end_thumb()
#define rv32_scan_track_arm() ((void)0)
#define rv32_scan_track_thumb() ((void)0)
#endif

/* Scanned tags must be cleared even when code emission fails. */
#define update_ram_tag_limits()                                               \
if (ram_region) {                                                             \
  if (block_end_pc >= 0x3000000) {                                            \
    iwram_code_min = MIN(block_end_pc & 0x7FFF, iwram_code_min);              \
    iwram_code_max = MAX(block_end_pc & 0x7FFF, iwram_code_max);              \
  } else {                                                                    \
    ewram_code_min = MIN(block_end_pc & 0x3FFFF, ewram_code_min);             \
    ewram_code_max = MAX(block_end_pc & 0x3FFFF, ewram_code_max);             \
  }                                                                           \
}                                                                             \

#define scan_block(type, smc_write_op)                                        \
{                                                                             \
  __label__ block_end;                                                        \
  /* Find the end of the block */                                             \
  do                                                                          \
  {                                                                           \
    check_pc_region(block_end_pc);                                            \
    update_ram_tag_limits();                                                  \
    smc_write_##type##_##smc_write_op();                                      \
    type##_load_opcode();                                                     \
    type##_flag_status();                                                     \
    rv32_scan_track_##type();                                                 \
                                                                              \
    if(type##_exit_point)                                                     \
    {                                                                         \
      /* Branch/branch with link */                                           \
      if(type##_opcode_branch)                                                \
      {                                                                       \
        __label__ no_direct_branch;                                           \
        type##_branch_target();                                               \
        block_exits[block_exit_position].branch_target = branch_target;       \
        block_exit_position++;                                                \
                                                                              \
        /* Give the branch target macro somewhere to bail if it turns out to  \
           be an indirect branch (ala malformed Thumb bl) */                  \
        if (0) goto no_direct_branch;                                         \
        no_direct_branch:;                                                    \
      }                                                                       \
                                                                              \
      /* SWI branches to the BIOS, unless it's an HLE call, then it is        \
         not parsed as an exit_point but rather an "instruction" of sorts. */ \
      if(type##_opcode_swi)                                                   \
      {                                                                       \
        block_exits[block_exit_position].branch_target = 0x00000008;          \
        block_exit_position++;                                                \
      }                                                                       \
                                                                              \
      type##_set_condition(condition | 0x10);                                 \
                                                                              \
      /* Only unconditional branches can end the block. */                    \
      if(type##_opcode_unconditional_branch)                                  \
      {                                                                       \
        /* Check to see if any prior block exits branch after here,           \
           if so don't end the block. Starts from the top and works           \
           down because the most recent branch is most likely to              \
           join after the end (if/then form) */                               \
        for(i = block_exit_position - 2; i >= 0; i--)                         \
        {                                                                     \
          if(block_exits[i].branch_target == block_end_pc)                    \
            break;                                                            \
        }                                                                     \
                                                                              \
        if(i < 0)                                                             \
          break;                                                              \
      }                                                                       \
      if(block_exit_position == MAX_EXITS)                                    \
        break;                                                                \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      type##_set_condition(condition);                                        \
    }                                                                         \
                                                                              \
    for(i = 0; i < translation_gate_targets; i++)                             \
    {                                                                         \
      if(block_end_pc == translation_gate_target_pc[i])                       \
        goto block_end;                                                       \
    }                                                                         \
                                                                              \
    block_data[block_data_position].update_cycles = 0;                        \
    block_data_position++;                                                    \
    rv32_scan_end_##type();                                                    \
    if((block_data_position == MAX_BLOCK_SIZE) ||                             \
     (block_end_pc == 0x3007FF0) || (block_end_pc == 0x203FFFF0))             \
    {                                                                         \
      break;                                                                  \
    }                                                                         \
  } while(1);                                                                 \
                                                                              \
  block_end:;                                                                 \
}                                                                             \

#define arm_fix_pc()                                                          \
  pc &= ~0x03                                                                 \

#define thumb_fix_pc()                                                        \
  pc &= ~0x01                                                                 \

/* ROM-cache entries for VRAM contain only an immutable PC and a branch to
 * the interpreter bridge. In particular, speculative/direct branch linking
 * must not change reg[PC] while compiling some other block. */
#ifdef HAVE_VRAM_INTERPRETER
#define translate_vram_entry(type)                                           \
  if ((pc >> 24) == 0x06) {                                                  \
    translation_ptr = rom_translation_ptr;                                   \
    if (translation_ptr > rv32_rom_limit()) {                                \
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;                  \
      rv32_lookup_rom_capacity_flush();                                      \
      flush_translation_cache_rom();                                         \
      return false;                                                          \
    }                                                                        \
    generate_vram_interpreter(type);                                         \
    rom_translation_ptr = translation_ptr;                                   \
    return true;                                                             \
  }
#else
#define translate_vram_entry(type)
#endif

#if defined(GPSP_RV32_JIT)
/* End of the scanned, code-tagged range of the RAM block being
   translated. */
static u32 rv32_ram_scan_end;

static u8 *rv32_ram_guest(u32 pc)
{
  return (pc >> 24) == 0x02 ? ewram + (pc & 0x3ffff) :
      iwram + 0x8000 + (pc & 0x7fff);
}

/* Records the scanned guest range of a RAM block for code write lookups
   and reuse. A full table is noted there and the next code write flushes
   the RAM cache. */
static void rv32_add_ram_unit(u32 start_pc, u8 *native_start, bool thumb)
{
  u32 start = rv32_ram_offset(native_start);
  u32 head = rv32_canonical_ram_pc(start_pc) ? start + block_prologue_size : 0;
  const uint32_t *slots;
  gpsp_rv32_ram_unit_add(start_pc, rv32_ram_scan_end, start,
      start + (u32)(ram_translation_ptr - native_start), head, thumb,
      gpsp_rv32_arm_flow_switch_list(&slots), rv32_ram_guest(start_pc));
}
#endif

#if defined(GPSP_RV32_JIT)
static void rv32_stage_arm_patches(const gpsp_rv32_arm_flow_ir *ir,
    const gpsp_rv32_arm_flow_layout *layout, u8 *chunk_start)
{
  for (unsigned i = 0; i < layout->guest_ops; i++)
  {
    unsigned mode = layout->op_mode[i];
    if (gpsp_rv32_arm_flow_patch_site(ir->pc + i * 4, ir->nodes[i].opcode,
            mode))
      gpsp_rv32_ram_stage_patch(ir->pc + i * 4,
          rv32_ram_offset(chunk_start + layout->op_offset[i]),
          layout->op_bytes[i], mode, ir->nodes[i].target_live);
  }
}
#endif

bool translate_block_arm(u32 pc, bool ram_region)
{
  u32 opcode = 0;
#if defined(GPSP_RV32_JIT)
  u32 rv32_scan_switches = 0;
#endif
  u32 condition;
  u32 last_condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
#if defined(GPSP_RV32_JIT)
  u32 rv32_arm_required_entries[MAX_BLOCK_SIZE / 32] = {1};
  u32 rv32_arm_published_entries[MAX_BLOCK_SIZE / 32] = {0};
#endif
#if defined(GPSP_RV32_JIT)
  const u32 *rv32_arm_entry_filter = NULL;
#endif
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  struct gpsp_rv32_site_stats rv32_block_sites = {0};
#endif
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  struct gpsp_rv32_emit_byte_stats rv32_emit_before;
  if (!ram_region && rv32_rom_pc(block_start_pc))
    gpsp_rv32_emit_get_byte_stats(&rv32_emit_before);
#endif
  generate_block_extra_vars_arm();
  arm_fix_pc();
  translation_failure = TRANSLATION_FAILURE_NONE;
  translate_vram_entry(arm);

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = rv32_ram_limit();
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit = rv32_rom_limit();
  }

  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(arm, yes);
#if defined(GPSP_RV32_JIT)
    rv32_ram_scan_end = block_end_pc;
#if defined(GPSP_RV32_TEST_RAM_SMC)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram scan: pc=%08x end=%08x switches=%u exits=%u limit=%u",
        (unsigned)block_start_pc, (unsigned)block_end_pc,
        (unsigned)rv32_scan_switches, (unsigned)block_exit_position,
        (unsigned)MAX_EXITS);
#endif
#endif
  }
  else
  {
    scan_block(arm, no);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       arm_instruction_width].update_cycles = 1;
#if defined(GPSP_RV32_JIT)
      unsigned entry = (branch_target - block_start_pc) /
          arm_instruction_width;
      rv32_arm_required_entries[entry >> 5] |= 1U << (entry & 31);
#endif
    }
  }

  arm_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  last_condition = 0x0E;

#if defined(GPSP_RV32_JIT)
  bool rv32_arm_flow_used = false;
#if defined(GPSP_BENCHMARK)
  u8 *rv32_arm_flow_start = ram_region ? ram_translation_ptr :
      rom_translation_ptr;
  u32 rv32_arm_flow_helper_ops = 0;
  u32 rv32_arm_flow_regions = 0;
  u32 rv32_arm_flow_entries = 0;
#endif
  if (block_end_pc > block_start_pc &&
      !((block_end_pc - block_start_pc) & 3) &&
      (block_end_pc - block_start_pc) / 4 <= MAX_BLOCK_SIZE &&
      !(cheat_master_hook >= block_start_pc &&
        cheat_master_hook < block_end_pc &&
        !((cheat_master_hook - block_start_pc) & 3)))
  {
    rv32_arm_flow_used = true;
    rv32_arm_entry_filter = rv32_arm_published_entries;
    while (pc < block_end_pc)
    {
      u32 chunk_pc = pc;
      unsigned count = (block_end_pc - pc) / 4;
      unsigned page_count = (0x8000 - (pc & 0x7fff)) / 4;
      if (count > GPSP_RV32_ARM_FLOW_MAX_OPS)
        count = GPSP_RV32_ARM_FLOW_MAX_OPS;
      if (count > page_count)
        count = page_count;
      uint32_t opcodes[GPSP_RV32_ARM_FLOW_MAX_OPS];
      for (unsigned index = 0; index < count; index++)
      {
        u32 instruction_pc = chunk_pc + index * 4;
        check_pc_region(instruction_pc);
        opcodes[index] = readaddress32(pc_address_block,
            instruction_pc & 0x7fff);
      }
      gpsp_rv32_arm_flow_ir ir;
      gpsp_rv32_arm_flow_layout layout;
      if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
      u8 *chunk_start = translation_ptr;
      u32 required_entries = 0;
      for (unsigned index = 0; index < count; index++)
      {
        unsigned entry = (chunk_pc - block_start_pc) / 4 + index;
        if (rv32_arm_required_entries[entry >> 5] &
            (1U << (entry & 31)))
          required_entries |= 1U << index;
      }
      if (!gpsp_rv32_arm_flow_full_decode(chunk_pc, opcodes,
              count, required_entries, &ir))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      for (unsigned index = 0; index < count; index++)
      {
        u32 target = gpsp_rv32_arm_flow_branch_target(&ir, index);
        if (target != UINT32_MAX &&
            (target < chunk_pc || target >= chunk_pc + count * 4))
          ir.nodes[index].target_live = rv32_arm_live_flags(target,
              block_start_pc, block_end_pc);
      }
      ir.fallthrough_live = rv32_arm_live_flags(chunk_pc + count * 4,
          block_start_pc, block_end_pc);
      if (!gpsp_rv32_arm_flow_full_emit(&ir, &translation_ptr,
              translation_cache_limit, &layout))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
      if (ram_region)
        rv32_stage_arm_patches(&ir, &layout, chunk_start);
      if (!layout.guest_ops ||
          (layout.guest_ops != count && !layout.ends_block) ||
          block_exit_position + layout.link_count > MAX_EXITS)
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      for (unsigned index = 0; index < layout.guest_ops; index++)
      {
        unsigned entry = block_data_position + index;
        if ((required_entries & (1U << index)) &&
            layout.entry_offset[index] == UINT16_MAX)
        {
          translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
          return false;
        }
        if (layout.entry_offset[index] != UINT16_MAX)
        {
          block_data[entry].block_offset =
              chunk_start + layout.entry_offset[index];
          rv32_arm_published_entries[entry >> 5] |=
              1U << (entry & 31);
#if defined(GPSP_BENCHMARK)
          rv32_arm_flow_entries++;
#endif
        }
        rv32_benchmark_arm_site();
        pc += 4;
      }
      block_data_position += layout.guest_ops;
#if defined(GPSP_BENCHMARK)
      rv32_arm_flow_helper_ops += layout.helper_ops;
      rv32_arm_flow_regions += layout.region_count;
#endif
      for (unsigned index = 0; index < layout.link_count; index++)
      {
        block_exits[block_exit_position].branch_source =
            chunk_start + layout.link_offset[index];
        block_exits[block_exit_position].branch_target =
            layout.link_target[index];
        block_exit_position++;
      }
      if (layout.ends_block)
      {
        block_end_pc = pc;
        break;
      }
    }
  }
  if (!rv32_arm_flow_used)
#endif
  while(pc != block_end_pc)
  {
#if defined(GPSP_RV32_JIT)
    if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit))
    {
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
      return false;
    }
#endif
    block_data[block_data_position].block_offset = translation_ptr;
    arm_base_cycles();

    if (pc == cheat_master_hook)
    {
      arm_process_cheats();
    }

    translate_arm_instruction();
    if(translation_failure != TRANSLATION_FAILURE_NONE)
      return false;
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit) {
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
      /* RV32 tries the next chunk before abandon_failed_translation(). */
#if !defined(GPSP_RV32_JIT)
      if (ram_region)
      {
        rv32_lookup_ram_capacity_flush();
        flush_translation_cache_ram();
      }
      else
      {
        rv32_lookup_rom_capacity_flush();
        flush_translation_cache_rom();
      }
#endif
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* This can happen if the last instruction is *not* inconditional */
  if ((last_condition & 0x0F) != 0x0E) {
    generate_branch_patch_conditional(backpatch_address, translation_ptr);
  }

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
  generate_translation_gate(arm);

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

#if defined(GPSP_RV32_JIT)
    if (ram_region)
    {
      if ((branch_target >= block_start_pc) &&
          (branch_target < block_end_pc))
      {
        translation_target = block_data[(branch_target - block_start_pc) /
            arm_instruction_width].block_offset;
        if (!rv32_patch_link(block_exits[i].branch_source,
                translation_target, translation_ptr, false))
        {
          translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
          return false;
        }
        /* Switch slot trick: a slot's link is an exit too, so a later
           write can move it (rv32_ram_edit_branch). */
        u32 site = rv32_ram_offset(block_exits[i].branch_source);
        if (gpsp_rv32_ram_stage_mode_before(site) ==
            GPSP_RV32_ARM_FLOW_OP_SWITCH)
          gpsp_rv32_ram_stage_exit(branch_target, site);
      }
      else if (!rv32_record_ram_link(block_exits[i].branch_source,
              branch_target, false, translation_ptr))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      continue;
    }
    if (branch_target == cheat_master_hook)
      continue;
    /* External BIOS links are guarded by the source's cache lifetime. */
    if (block_start_pc < 0x4000)
    {
      if (branch_target >= 0x4000)
        continue;
    }
    else if (!rv32_rom_pc(block_start_pc) || !rv32_rom_pc(branch_target))
      continue;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      translation_target = block_data[(branch_target - block_start_pc) /
       arm_instruction_width].block_offset;
      if (!rv32_patch_link(block_exits[i].branch_source,
              translation_target, translation_ptr, false))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
    }
    else if (!rv32_record_link(block_exits[i].branch_source,
            branch_target, false, translation_ptr))
    {
      translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
      return false;
    }
#else
    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        arm_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, queue for the outermost lookup to resolve */
      if(!defer_external_exit(block_exits[i].branch_source,
       branch_target, 0))
      {
        translation_failure = TRANSLATION_FAILURE_OUT_OF_MEMORY;
        return false;
      }
    }
#endif
  }

  if (ram_region)
  {
#if defined(GPSP_RV32_JIT)
    u8 *unit_start = ram_translation_ptr;
#endif
    ram_translation_ptr = translation_ptr;
#if defined(GPSP_RV32_JIT)
    if (!rv32_record_ram_entries(block_start_pc, block_data_position,
            false, rv32_arm_entry_filter))
    {
      translation_failure = TRANSLATION_FAILURE_LOOKUP;
      return false;
    }
    rv32_add_ram_unit(block_start_pc, unit_start, false);
#if defined(GPSP_RV32_TEST_RAM_SMC)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram translate: pc=%08x native=%u used=%u emitted_end=%08x scan_end=%08x flushes=%u",
        (unsigned)block_start_pc, (unsigned)rv32_ram_offset(unit_start),
        (unsigned)rv32_ram_offset(ram_translation_ptr),
        (unsigned)block_end_pc, (unsigned)rv32_ram_scan_end,
        (unsigned)rv32_ram_cache_flush_count);
#endif
#endif
  }
  else
  {
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
    size_t block_bytes = (size_t)(translation_ptr - rom_translation_ptr);
#endif
    rom_translation_ptr = translation_ptr;
#if defined(GPSP_RV32_JIT)
    if (rv32_rom_pc(block_start_pc) &&
        !rv32_record_rom_entries(block_start_pc, block_data_position,
            false, NULL, NULL, NULL, NULL, NULL, NULL,
            rv32_arm_entry_filter))
    {
      translation_failure = TRANSLATION_FAILURE_LOOKUP;
      return false;
    }
#endif
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
    if (rv32_rom_pc(block_start_pc))
    {
      rv32_record_rom_block((unsigned)block_data_position,
          block_bytes, false, NULL, 0);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
      rv32_record_rom_emitted_bytes(&rv32_emit_before, block_bytes);
      rv32_record_rom_overlap(block_start_pc, 0, block_bytes);
#endif
      rv32_pending_site_stats = rv32_block_sites;
    }
#endif
  }

#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  if (rv32_arm_flow_used)
  {
    rv32_compile_stats.flow_blocks++;
    rv32_compile_stats.flow_arm_blocks++;
    rv32_compile_stats.flow_ram_blocks += ram_region;
    rv32_compile_stats.flow_guest_ops += block_data_position;
    rv32_compile_stats.flow_helper_ops += rv32_arm_flow_helper_ops;
    rv32_compile_stats.flow_regions += rv32_arm_flow_regions;
    rv32_compile_stats.flow_entries += rv32_arm_flow_entries;
    rv32_compile_stats.flow_code_bytes +=
        translation_ptr - rv32_arm_flow_start;
  }
  else
    rv32_compile_stats.flow_legacy_blocks++;
#endif
  rv32_lookup_record_range(block_start_pc, block_end_pc, 0, 0);
  return true;
}

bool translate_block_thumb(u32 pc, bool ram_region)
{
  u32 opcode = 0;
  u32 last_opcode;
#if !defined(GPSP_RV32_JIT)
  u32 condition;
#endif
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
#if !defined(GPSP_RV32_JIT)
  u8 *backpatch_address = NULL;
#endif
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  struct gpsp_rv32_site_stats rv32_block_sites = {0};
#endif
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  /* Counted only for a published block; a retry emits it again. */
  unsigned rv32_thumb_fused_branches = 0;
  struct gpsp_rv32_emit_byte_stats rv32_emit_before;
  if (!ram_region && rv32_rom_pc(block_start_pc))
    gpsp_rv32_emit_get_byte_stats(&rv32_emit_before);
#endif
#if defined(GPSP_RV32_JIT)
  unsigned rv32_thumb_batch_remaining = 0;
  unsigned rv32_thumb_batch_length = 0;
  unsigned rv32_thumb_slow_count = 0;
  memset(rv32_thumb_entry_mask, 0, sizeof(rv32_thumb_entry_mask));
  memset(rv32_thumb_unguarded_mask, 0,
      sizeof(rv32_thumb_unguarded_mask));
  memset(rv32_thumb_side_prefix, 0, sizeof(rv32_thumb_side_prefix));
  memset(rv32_thumb_side_remaining, 0,
      sizeof(rv32_thumb_side_remaining));
  memset(rv32_thumb_side_prefix_cost, 0,
      sizeof(rv32_thumb_side_prefix_cost));
  memset(rv32_thumb_side_total_cost, 0,
      sizeof(rv32_thumb_side_total_cost));
  u32 rv32_thumb_ram_entry_mask[MAX_BLOCK_SIZE / 32] = {0};
  u32 rv32_phase = rv32_phase_start();
  u32 rv32_thumb_join_pc = !ram_region && rv32_rom_pc(pc) ?
      rv32_next_rom_thumb_entry(pc) : 0;
  rv32_phase_add(phase_join_cycles, rv32_phase);
  bool rv32_thumb_fallthrough = false;
  u8 *rv32_thumb_gate_site = NULL;
  bool rv32_thumb_flow_used = false;
#if defined(GPSP_BENCHMARK)
  u8 *rv32_thumb_flow_start = NULL;
  u32 rv32_thumb_flow_helper_ops = 0;
  u32 rv32_thumb_flow_regions = 0;
  u32 rv32_thumb_flow_entries = 0;
#endif
  u32 rv32_thumb_required_entries[MAX_BLOCK_SIZE / 32] = {1};
#if defined(GPSP_BENCHMARK)
  u32 rv32_thumb_span_count[4] = {0};
  unsigned rv32_thumb_bundled_guest_ops = 0;
#endif
#endif
  generate_block_extra_vars_thumb();
  thumb_fix_pc();
  translation_failure = TRANSLATION_FAILURE_NONE;
  translate_vram_entry(thumb);

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = rv32_ram_limit();
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit = rv32_rom_limit();
  }

  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

#if defined(GPSP_RV32_JIT)
  rv32_phase = rv32_phase_start();
#endif
  if(ram_region)
  {
    scan_block(thumb, yes);
#if defined(GPSP_RV32_JIT)
    rv32_ram_scan_end = block_end_pc;
#endif
  }
  else
  {
    scan_block(thumb, no);
  }

#if defined(GPSP_RV32_JIT)
  rv32_phase_add(phase_scan_cycles, rv32_phase);
  rv32_phase = rv32_phase_start();
  unsigned rv32_thumb_scanned_count = (unsigned)block_data_position;
#endif

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       thumb_instruction_width].update_cycles = 1;
#if defined(GPSP_RV32_JIT)
      unsigned entry = (branch_target - block_start_pc) /
          thumb_instruction_width;
      rv32_thumb_required_entries[entry >> 5] |= 1U << (entry & 31);
#endif
    }
  }

#if !defined(GPSP_RV32_JIT)
  thumb_dead_flag_eliminate();
#endif

  block_exit_position = 0;
  block_data_position = 0;

#if defined(GPSP_RV32_JIT)
  if (block_end_pc > block_start_pc &&
      !((block_end_pc - block_start_pc) & 1) &&
      (block_end_pc - block_start_pc) / 2 <= MAX_BLOCK_SIZE &&
      !(cheat_master_hook >= block_start_pc &&
        cheat_master_hook < block_end_pc &&
        !((cheat_master_hook - block_start_pc) & 1)))
  {
    rv32_thumb_flow_used = true;
#if defined(GPSP_BENCHMARK)
    rv32_thumb_flow_start = ram_region ? ram_translation_ptr :
        rom_translation_ptr;
#endif
    while (pc < block_end_pc)
    {
      u32 chunk_pc = pc;
      unsigned count = (block_end_pc - pc) / 2;
      unsigned page_count = (0x8000 - (pc & 0x7fff)) / 2;
      if (count > GPSP_RV32_THUMB_FLOW_MAX_OPS)
        count = GPSP_RV32_THUMB_FLOW_MAX_OPS;
      if (count > page_count)
        count = page_count;
      u16 opcodes[GPSP_RV32_THUMB_FLOW_MAX_OPS];
      u8 *opcode_pages[GPSP_RV32_THUMB_FLOW_MAX_OPS];
      for (unsigned index = 0; index < count; index++)
      {
        u32 instruction_pc = chunk_pc + index * 2;
        check_pc_region(instruction_pc);
        opcode_pages[index] = pc_address_block;
        opcodes[index] = readaddress16(pc_address_block,
            instruction_pc & 0x7fff);
      }
      gpsp_rv32_thumb_flow_full_ir ir;
      gpsp_rv32_thumb_flow_full_layout layout;
      if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
      u8 *chunk_start = translation_ptr;
      if (!gpsp_rv32_thumb_flow_full_decode(chunk_pc, opcodes,
              count, &ir))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      ir.rom_region = !ram_region && rv32_rom_pc(block_start_pc);
      for (unsigned index = 0; index < count; index++)
      {
        unsigned entry = (chunk_pc - block_start_pc) / 2 + index;
        ir.ops[index].entry_required |=
            (rv32_thumb_required_entries[entry >> 5] >>
                (entry & 31)) & 1;
        if (entry < rv32_thumb_scanned_count)
          ir.ops[index].flag_data = block_data[entry].flag_data;
        u32 literal_value;
        if (rv32_thumb_literal_value(chunk_pc + index * 2,
                opcodes[index], opcode_pages[index], &literal_value))
        {
          ir.ops[index].literal_constant = 1;
          ir.ops[index].literal_value = literal_value;
        }
      }
      for (unsigned index = 0; index < count; index++)
      {
        u32 target = gpsp_rv32_thumb_flow_branch_target(&ir, index);
        if (target != UINT32_MAX &&
            (target < chunk_pc || target >= chunk_pc + count * 2))
          ir.ops[index].target_live = rv32_thumb_live_flags(target,
              block_start_pc, block_end_pc);
      }
      ir.fallthrough_live = rv32_thumb_live_flags(chunk_pc + count * 2,
          block_start_pc, block_end_pc);
      if (!gpsp_rv32_thumb_flow_full_emit(&ir, &translation_ptr,
              translation_cache_limit, &layout))
      {
        /* Layout offsets are 16-bit, so more room cannot help. */
        translation_failure =
            translation_cache_limit - chunk_start <= UINT16_MAX ?
            TRANSLATION_FAILURE_CACHE_FULL :
            TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      if (!layout.guest_ops ||
          (layout.guest_ops != count && !layout.ends_block) ||
          block_exit_position + layout.link_count > MAX_EXITS)
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      for (unsigned index = 0; index < layout.guest_ops; index++)
      {
        unsigned entry = block_data_position + index;
        if (ir.ops[index].entry_required &&
            layout.entry_offset[index] == UINT16_MAX)
        {
          translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
          return false;
        }
        if (layout.entry_offset[index] != UINT16_MAX)
        {
          block_data[entry].block_offset =
              chunk_start + layout.entry_offset[index];
          if (ram_region)
            rv32_thumb_ram_entry_mask[entry >> 5] |=
                1U << (entry & 31);
          else if (entry < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT)
            rv32_thumb_entry_mask[entry >> 5] |=
                1U << (entry & 31);
          if (!ram_region &&
              entry < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT &&
              (layout.unguarded_mask & (1U << index)))
            rv32_thumb_unguarded_mask[entry >> 5] |=
                1U << (entry & 31);
#if defined(GPSP_BENCHMARK)
          rv32_thumb_flow_entries++;
#endif
        }
        else if (ir.rom_region && layout.side_prefix[index])
        {
          if (layout.body_offset[index] == UINT16_MAX)
          {
            translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
            return false;
          }
          block_data[entry].block_offset =
              chunk_start + layout.body_offset[index];
          if (entry < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT)
          {
            rv32_thumb_side_prefix[entry] =
                layout.side_prefix[index];
            rv32_thumb_side_remaining[entry] =
                layout.side_remaining[index];
            rv32_thumb_side_prefix_cost[entry] =
                layout.side_prefix_cost[index];
            rv32_thumb_side_total_cost[entry] =
                layout.side_total_cost[index];
          }
        }
        rv32_benchmark_thumb_site(opcodes[index]);
        pc += 2;
      }
      block_data_position += layout.guest_ops;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
      if (ir.rom_region)
        rv32_thumb_fused_branches += layout.fused_branches;
#endif
#if defined(GPSP_BENCHMARK)
      rv32_thumb_flow_helper_ops += layout.helper_ops;
      rv32_thumb_flow_regions += layout.region_count;
#endif
      for (unsigned index = 0; index < layout.link_count; index++)
      {
        block_exits[block_exit_position].branch_source =
            chunk_start + layout.link_offset[index];
        block_exits[block_exit_position].branch_target =
            layout.link_target[index];
        block_exit_position++;
      }
      rv32_thumb_fallthrough = !layout.ends_block;
      if (layout.ends_block)
      {
        block_end_pc = pc;
        break;
      }
    }
  }
  if (!rv32_thumb_flow_used)
#endif
  while(pc != block_end_pc)
  {
#if defined(GPSP_RV32_JIT)
    check_pc_region(pc);
    if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit))
    {
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
      return false;
    }
    u8 *rv32_thumb_entry = translation_ptr;
    /* A batch interior depends on its head's preflight. */
    if (!rv32_thumb_batch_remaining)
    {
      if (ram_region)
        rv32_thumb_ram_entry_mask[block_data_position >> 5] |=
            1U << (block_data_position & 31);
      else if (block_data_position < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT)
        rv32_thumb_entry_mask[block_data_position >> 5] |=
            1U << (block_data_position & 31);
    }
    if (!rv32_thumb_batch_remaining)
    {
      unsigned count = rv32_thumb_span_length(pc, block_end_pc,
          block_data_position, rv32_thumb_scanned_count, pc_address_block);
      if (count)
      {
        u8 *slow_site;
        rv32_thumb_span_flags(block_data_position, count);
        if (!gpsp_rv32_emit_thumb_preflight(&translation_ptr,
                translation_cache_limit,
                (uintptr_t)&ws_cyc_seq[((pc + 2) >> 24) & 15][0], count,
                &slow_site))
        {
          translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
          return false;
        }
        rv32_thumb_slow_site_type *slow =
            &rv32_thumb_slow_sites[rv32_thumb_slow_count++];
        slow->site = slow_site;
        slow->pc = pc;
        slow->opcode = readaddress16(pc_address_block, pc & 0x7FFF);
        rv32_thumb_batch_remaining = count;
        rv32_thumb_batch_length = count;
      }
    }
    if (!ram_region && rv32_thumb_batch_remaining &&
        block_data_position < RV32_ROM_CACHE_THUMB_BLOCK_LIMIT)
    {
      rv32_thumb_side_prefix[block_data_position] =
          rv32_thumb_batch_length - rv32_thumb_batch_remaining;
      rv32_thumb_side_remaining[block_data_position] =
          rv32_thumb_batch_remaining;
    }
    block_data[block_data_position].block_offset = rv32_thumb_entry;
#else
    block_data[block_data_position].block_offset = translation_ptr;
#endif
    thumb_base_cycles();

    if (pc == cheat_master_hook)
    {
      thumb_process_cheats();
    }

    translate_thumb_instruction();
    if(translation_failure != TRANSLATION_FAILURE_NONE)
      return false;
    block_data_position++;

#if defined(GPSP_RV32_JIT)
    if (rv32_thumb_batch_remaining && !--rv32_thumb_batch_remaining)
    {
      if (!gpsp_rv32_emit_thumb_batch_finish(&translation_ptr,
              translation_cache_limit, rv32_thumb_batch_length))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
#if defined(GPSP_BENCHMARK)
      unsigned bin = rv32_thumb_batch_length <= 4 ? 0 :
          rv32_thumb_batch_length <= 8 ? 1 :
          rv32_thumb_batch_length <= 16 ? 2 : 3;
      rv32_thumb_span_count[bin]++;
      rv32_thumb_bundled_guest_ops += rv32_thumb_batch_length;
#endif
    }
#endif

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit)
    {
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
      /* RV32 tries the next chunk before abandon_failed_translation(). */
#if !defined(GPSP_RV32_JIT)
      if (ram_region)
      {
        rv32_lookup_ram_capacity_flush();
        flush_translation_cache_ram();
      }
      else
      {
        rv32_lookup_rom_capacity_flush();
        flush_translation_cache_rom();
      }
#endif
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
#if defined(GPSP_RV32_JIT)
  /* A normal fallthrough can enter the next Thumb block with registers live. */
  if (rv32_thumb_fallthrough && block_end_pc != cheat_master_hook &&
      (ram_region ? rv32_ram_pc(block_end_pc) :
          rv32_rom_pc(block_end_pc)))
  {
    gpsp_rv32_code gate;
    gpsp_rv32_code_init(&gate, translation_ptr,
        translation_cache_limit - translation_ptr);
    if (!gpsp_rv32_emit_link_slot(&gate, &rv32_thumb_gate_site))
    {
      translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
      return false;
    }
    translation_ptr = gate.ptr;
  }
  else
#endif
    generate_translation_gate(thumb);

#if defined(GPSP_RV32_JIT)
  /* Keep cold single-step bodies after the normal block exit. */
  for (unsigned slow_index = 0; slow_index < rv32_thumb_slow_count;
       slow_index++)
  {
    rv32_thumb_slow_site_type *slow =
        &rv32_thumb_slow_sites[slow_index];
    u32 size = 0;
    u8 *target = rv32_slow_find(slow->pc, slow->opcode,
        ram_region, &size);
    if (!target)
    {
      u32 key = rv32_slow_key(slow->pc, slow->opcode);
      u32 pc_key = rv32_slow_pc(slow->pc, slow->opcode);
      for (unsigned previous = 0; previous < slow_index; previous++)
      {
        rv32_thumb_slow_site_type *prior =
            &rv32_thumb_slow_sites[previous];
        if (rv32_slow_key(prior->pc, prior->opcode) == key &&
            rv32_slow_pc(prior->pc, prior->opcode) == pc_key)
        {
          target = prior->target;
          size = prior->size;
          break;
        }
      }
    }
    bool emitted = !target;
    if (emitted)
    {
      if (!rv32_pad_to_word(&translation_ptr, translation_cache_limit))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
      target = translation_ptr;
      if (!gpsp_rv32_emit_thumb_batch_slow(&translation_ptr,
              translation_cache_limit, slow->pc, slow->opcode))
      {
        translation_failure = TRANSLATION_FAILURE_CACHE_FULL;
        return false;
      }
      /* Include an unused tail gap in the packed four-byte size. */
      translation_ptr = rv32_skip_to_word(translation_ptr);
      size = (u32)(translation_ptr - target);
    }
    if (!rv32_patch_link(slow->site, target, translation_ptr, false))
    {
      translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
      return false;
    }
    slow->target = target;
    slow->size = size;
    slow->emitted = emitted;
  }
  /* Link after the last capacity check: a link to a published RAM block
     is recorded for good. */
  if (rv32_thumb_gate_site &&
      !(ram_region ?
          rv32_record_ram_link(rv32_thumb_gate_site, block_end_pc, true,
              translation_ptr) :
          rv32_record_link(rv32_thumb_gate_site, block_end_pc, true,
              translation_ptr)))
  {
    translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
    return false;
  }
#endif
#if defined(GPSP_RV32_JIT)
  rv32_phase_add(phase_emit_cycles, rv32_phase);
  rv32_phase = rv32_phase_start();
#endif

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

#if defined(GPSP_RV32_JIT)
    if (ram_region)
    {
      if ((branch_target >= block_start_pc) &&
          (branch_target < block_end_pc))
      {
        translation_target = block_data[(branch_target - block_start_pc) /
            thumb_instruction_width].block_offset;
        if (!rv32_patch_link(block_exits[i].branch_source,
                translation_target, translation_ptr, false))
        {
          translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
          return false;
        }
      }
      else if (!rv32_record_ram_link(block_exits[i].branch_source,
              branch_target, true, translation_ptr))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
      continue;
    }
    if (!rv32_rom_pc(block_start_pc) ||
        branch_target == cheat_master_hook || !rv32_rom_pc(branch_target))
      continue;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      translation_target = block_data[(branch_target - block_start_pc) /
       thumb_instruction_width].block_offset;
      if (!rv32_patch_link(block_exits[i].branch_source,
              translation_target, translation_ptr, false))
      {
        translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
        return false;
      }
    }
    else if (!rv32_record_link(block_exits[i].branch_source,
            branch_target, true, translation_ptr))
    {
      translation_failure = TRANSLATION_FAILURE_UNSUPPORTED;
      return false;
    }
#else
    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        thumb_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, queue for the outermost lookup to resolve */
      if(!defer_external_exit(block_exits[i].branch_source,
       branch_target, 1))
      {
        translation_failure = TRANSLATION_FAILURE_OUT_OF_MEMORY;
        return false;
      }
    }
#endif
  }

  if (ram_region)
  {
#if defined(GPSP_RV32_JIT)
    u8 *unit_start = ram_translation_ptr;
#endif
    ram_translation_ptr = translation_ptr;
#if defined(GPSP_RV32_JIT)
    if (!rv32_record_ram_entries(block_start_pc, block_data_position,
            true, rv32_thumb_ram_entry_mask))
    {
      translation_failure = TRANSLATION_FAILURE_LOOKUP;
      return false;
    }
    rv32_add_ram_unit(block_start_pc, unit_start, true);
#if defined(GPSP_RV32_TEST_RAM_SMC)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram translate: pc=%08x native=%u used=%u emitted_end=%08x scan_end=%08x flushes=%u",
        (unsigned)block_start_pc, (unsigned)rv32_ram_offset(unit_start),
        (unsigned)rv32_ram_offset(ram_translation_ptr),
        (unsigned)block_end_pc, (unsigned)rv32_ram_scan_end,
        (unsigned)rv32_ram_cache_flush_count);
#endif
#endif
  }
  else
  {
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
    size_t block_bytes = (size_t)(translation_ptr - rom_translation_ptr);
#endif
    rom_translation_ptr = translation_ptr;
#if defined(GPSP_RV32_JIT)
    if (rv32_rom_pc(block_start_pc) &&
        !rv32_record_rom_entries(block_start_pc, block_data_position,
            true, rv32_thumb_entry_mask, rv32_thumb_unguarded_mask,
            rv32_thumb_side_prefix,
            rv32_thumb_side_remaining,
            rv32_thumb_side_prefix_cost,
            rv32_thumb_side_total_cost, NULL))
    {
      translation_failure = TRANSLATION_FAILURE_LOOKUP;
      return false;
    }
#endif
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
    if (rv32_rom_pc(block_start_pc))
    {
      rv32_record_rom_block((unsigned)block_data_position,
          block_bytes, true,
          rv32_thumb_span_count, rv32_thumb_bundled_guest_ops);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
      rv32_record_rom_emitted_bytes(&rv32_emit_before, block_bytes);
      rv32_record_rom_overlap(block_start_pc, 1, block_bytes);
      rv32_overlap_byte_stats.fused_branch_regions +=
          rv32_thumb_fused_branches;
      if (rv32_thumb_join_pc && block_end_pc == rv32_thumb_join_pc)
        rv32_overlap_byte_stats.joins++;
#endif
      rv32_pending_site_stats = rv32_block_sites;
    }
#endif
  }

#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  if (rv32_thumb_flow_used)
  {
    rv32_compile_stats.flow_blocks++;
    rv32_compile_stats.flow_thumb_blocks++;
    rv32_compile_stats.flow_ram_blocks += ram_region;
    rv32_compile_stats.flow_guest_ops += block_data_position;
    rv32_compile_stats.flow_helper_ops += rv32_thumb_flow_helper_ops;
    rv32_compile_stats.flow_regions += rv32_thumb_flow_regions;
    rv32_compile_stats.flow_entries += rv32_thumb_flow_entries;
    rv32_compile_stats.flow_code_bytes +=
        translation_ptr - rv32_thumb_flow_start;
  }
  else
    rv32_compile_stats.flow_legacy_blocks++;
#endif
#if defined(GPSP_RV32_JIT)
  rv32_lookup_record_range(block_start_pc, block_end_pc, 1,
      rv32_rom_pc(block_start_pc) ? rv32_thumb_entry_mask : NULL);
#else
  rv32_lookup_record_range(block_start_pc, block_end_pc, 1, 0);
#endif
#if defined(GPSP_RV32_JIT)
  for (unsigned slow_index = 0; slow_index < rv32_thumb_slow_count;
       slow_index++)
  {
    rv32_thumb_slow_site_type *slow =
        &rv32_thumb_slow_sites[slow_index];
    rv32_slow_commit(slow->pc, slow->opcode, ram_region,
        slow->target, slow->size, slow->emitted);
  }
  rv32_phase_add(phase_record_cycles, rv32_phase);
#endif
  return true;
}

void init_bios_hooks(void)
{
  // Pre-generate this entry point so that we can safely invoke fast
  // SWI calls from ROM and RAM regardless of cache flushes.
  rom_translation_ptr = rv32_rom_at(rom_cache_watermark);
  last_rom_translation_ptr = rom_translation_ptr;
#if defined(GPSP_RV32_JIT)
  rv32_bios_pinning = true;
#endif
  bios_swi_entrypoint = block_lookup_address_arm(0x8);
#if defined(GPSP_RV32_JIT)
  rv32_bios_pinning = false;
#endif
  rom_cache_watermark = rv32_rom_offset(rom_translation_ptr);
}

#if defined(GPSP_RV32_JIT)
static bool rv32_wait_flush_pending;

/* Flow code holds the ROM waits it was translated with. The next RAM flush
   also drops ROM code, so RAM blocks never link into flushed ROM blocks. */
void gpsp_rv32_request_wait_flush(void)
{
  rv32_wait_flush_pending = true;
}
#endif

#if defined(GPSP_RV32_JIT)
/* Partial RAM invalidation. Code writes queue their changed range; the
   units translated from it are dropped and retranslated on their next
   lookup. Their native code stays in the cache until the next full flush. */

#define RV32_DROP_MAX 64

static bool rv32_unlink_failed;

static u16 *rv32_ram_code_tag(u32 address)
{
  return (address >> 24) == 0x02 ?
      (u16 *)(ewram + 0x40000 + (address & 0x3fffe)) :
      (u16 *)(iwram + (address & 0x7ffe));
}

/* The slot links again once its target is translated or revived. */
static void rv32_unlink_ram_site(uint32_t site, uint32_t key)
{
  u8 *at = rv32_ram_at(site);
  /* A slot that is neither linked nor unlinked needs a full flush. */
  if (!gpsp_rv32_unlink_link_slot(at))
  {
    rv32_unlink_failed = true;
    return;
  }
  platform_cache_sync(at, at + GPSP_RV32_LINK_SLOT_SIZE);
  if (!rv32_cache_sync_ok)
    rv32_unlink_failed = true;
  rv32_add_pending_ram_link(at, key);
}

/* A head tag survives; only its offset into the dropped unit is cleared, so
   the retranslation reuses the tag. */
static void rv32_drop_ram_head(const gpsp_rv32_ram_unit *unit, u16 id)
{
  u16 tag = *rv32_ram_code_tag(unit->guest_start);
  if (!VALID_TAG(tag))
    return;
  ramtag_type *entry = get_ram_tag(tag);
  if (entry->offset_arm && gpsp_rv32_ram_units_contain(entry->offset_arm,
          &id, 1))
    entry->offset_arm = 0;
  if (entry->offset_thumb && gpsp_rv32_ram_units_contain(entry->offset_thumb,
          &id, 1))
    entry->offset_thumb = 0;
}

/* Clears the code marks of dropped ranges, then marks again the parts that
   live units still cover, so their stores keep raising SMC. */
static bool rv32_retag_ram(const u16 *ids, unsigned count)
{
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(ids[i]);
    for (u32 address = unit->guest_start; address < unit->guest_end;
        address += 2)
    {
      u16 *tag = rv32_ram_code_tag(address);
      if (*tag == CODE_TAG_BLOCK16)
        *tag = 0;
    }
  }
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_unit *dead = gpsp_rv32_ram_unit_get(ids[i]);
    u16 live[RV32_DROP_MAX];
    unsigned found = gpsp_rv32_ram_units_find(dead->guest_start,
        dead->guest_end - dead->guest_start, live, RV32_DROP_MAX);
    if (found == RV32_DROP_MAX)
      return false;
    for (unsigned k = 0; k < found; k++)
    {
      const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(live[k]);
      u32 start = MAX(unit->guest_start, dead->guest_start);
      u32 end = MIN(unit->guest_end, dead->guest_end);
      for (u32 address = start & ~1u; address < end; address += 2)
      {
        u16 *tag = rv32_ram_code_tag(address);
        if (!*tag)
          *tag = CODE_TAG_BLOCK16;
      }
    }
  }
  return true;
}

/* Makes a dormant unit live again: its head, entries and code marks come
   back and its exits link to the current targets. */
typedef enum
{
  RV32_RESTORE_DONE,
  RV32_RESTORE_SKIPPED,   /* nothing was published */
  RV32_RESTORE_FAILED     /* partly published; needs a flush */
} rv32_restore_result;

static rv32_restore_result rv32_restore_ram_unit(unsigned id)
{
  const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(id);
  const gpsp_rv32_ram_ref *refs = gpsp_rv32_ram_unit_refs(id);
  if (unit->head)
  {
    u16 tag = *rv32_ram_code_tag(unit->guest_start);
    if (!VALID_TAG(tag))
      return RV32_RESTORE_SKIPPED;
    ramtag_type *entry = get_ram_tag(tag);
    if (unit->thumb)
      entry->offset_thumb = unit->head;
    else
      entry->offset_arm = unit->head;
  }
  for (unsigned i = 0; i < unit->entries; i++)
  {
    if (!rv32_ram_entry_find(refs[i].key))
      rv32_ram_entry_add(refs[i].key, rv32_ram_at(refs[i].offset));
  }
  for (u32 address = unit->guest_start; address < unit->guest_end;
      address += 2)
  {
    u16 *tag = rv32_ram_code_tag(address);
    if (!*tag)
      *tag = CODE_TAG_BLOCK16;
  }
  gpsp_rv32_ram_unit_revive(id);

  /* Exit targets may have been dropped while the unit was dormant. */
  for (unsigned i = unit->entries; i < unit->entries + unit->exits; i++)
  {
    u8 *site = rv32_ram_at(refs[i].offset);
    if (!gpsp_rv32_unlink_link_slot(site))
      return RV32_RESTORE_FAILED;
    u8 *target = rv32_published_ram_block(refs[i].key);
    if (!target)
      rv32_add_pending_ram_link(site, refs[i].key);
    else if (!rv32_patch_ram_link(site, target, ram_translation_ptr, false,
            refs[i].key))
      return RV32_RESTORE_FAILED;
  }
#if defined(GPSP_RV32_TEST_SYNC_FAULT)
  /* Test builds fail the sync of the first restore: once with 1, and
     every later sync with 2. */
  {
    extern u32 gpsp_rv32_test_sync_faults;
    static bool armed;
    if (!armed)
    {
      armed = true;
      gpsp_rv32_test_sync_faults = GPSP_RV32_TEST_SYNC_FAULT == 1 ? 1 : ~0u;
      gpsp_rv32_log(GPSP_RV32_LOG_WARN, "rv32_ram",
          "test sync fault armed in a restore");
    }
  }
#endif
  u8 *native_start = rv32_ram_at(unit->native_start);
  platform_cache_sync(native_start,
      native_start + (unit->native_end - unit->native_start));
  if (!rv32_cache_sync_ok)
    return RV32_RESTORE_FAILED;

  /* Slots that wait for this unit's entries link now. */
  if (unit->head && !rv32_publish_ram_links(
          (unit->guest_start & ~1u) | (u32)unit->thumb,
          rv32_ram_at(unit->head)))
    return RV32_RESTORE_FAILED;
  return rv32_publish_ram_entry_links() ? RV32_RESTORE_DONE :
      RV32_RESTORE_FAILED;
}

#if defined(GPSP_BENCHMARK_RAM_CODE)
/* RAM code writes: changed words, unit results, revivals and flushes.
   Word counts include the edit pass a revival runs. */
static struct
{
  unsigned write_events, wait_flushes;
  unsigned flush_dirty_overflow, flush_units_full, flush_unit_limit;
  unsigned flush_patch_sync, flush_unlink;
  unsigned units_seen, units_patched, units_dropped;
  unsigned unit_not_patchable;
  unsigned word_dynamic_ok, word_dynamic_reject, word_no_patch, word_edits_full;
  unsigned word_branch_ok, word_branch_check, word_imm_unsafe, word_learned;
  unsigned word_dropped_ok, word_repatch_ok;
  unsigned word_fail_mode, word_fail_old, word_fail_size, word_fail_address;
  unsigned revive_calls, revive_candidates, revive_exact, revive_patched;
  unsigned revive_skipped, revive_failed;
} rv32_smc;
#define rv32_smc_count(field) (rv32_smc.field++)

/* Counts why a re-emission failed; 0 means it never ran. */
static void rv32_smc_repatch_failed(void)
{
  switch (gpsp_rv32_arm_flow_repatch_failure)
  {
    case 0: rv32_smc.word_branch_check++; break;
    case GPSP_RV32_REPATCH_MODE: rv32_smc.word_fail_mode++; break;
    case GPSP_RV32_REPATCH_OLD: rv32_smc.word_fail_old++; break;
    case GPSP_RV32_REPATCH_SIZE: rv32_smc.word_fail_size++; break;
    default: rv32_smc.word_fail_address++; break;
  }
}

/* Totals since boot, printed once when a benchmark ends. */
void gpsp_rv32_smc_bench_report(void)
{
  extern void gpsp_rv32_arm_short_branch_report(void);
  gpsp_rv32_arm_short_branch_report();
  gpsp_rv32_ram_units_bench units;
  gpsp_rv32_ram_units_bench_get(&units);
  printf("BENCH_JIT_SMC version=1 write_events=%u wait_flushes=%u "
      "flush_dirty_overflow=%u flush_units_full=%u flush_unit_limit=%u "
      "flush_patch_sync=%u flush_unlink=%u units_seen=%u "
      "units_patched=%u units_dropped=%u unit_not_patchable=%u "
      "word_dynamic_ok=%u word_dynamic_reject=%u word_no_patch=%u "
      "word_edits_full=%u word_branch_ok=%u word_branch_check=%u "
      "word_imm_unsafe=%u word_learned=%u word_dropped_ok=%u "
      "word_repatch_ok=%u word_fail_mode=%u word_fail_old=%u "
      "word_fail_size=%u word_fail_address=%u revive_calls=%u "
      "revive_candidates=%u revive_exact=%u revive_patched=%u "
      "revive_skipped=%u revive_failed=%u ram_flushes_total=%u\n",
      rv32_smc.write_events, rv32_smc.wait_flushes,
      rv32_smc.flush_dirty_overflow, rv32_smc.flush_units_full,
      rv32_smc.flush_unit_limit, rv32_smc.flush_patch_sync,
      rv32_smc.flush_unlink, rv32_smc.units_seen, rv32_smc.units_patched,
      rv32_smc.units_dropped, rv32_smc.unit_not_patchable,
      rv32_smc.word_dynamic_ok, rv32_smc.word_dynamic_reject,
      rv32_smc.word_no_patch, rv32_smc.word_edits_full,
      rv32_smc.word_branch_ok, rv32_smc.word_branch_check,
      rv32_smc.word_imm_unsafe, rv32_smc.word_learned,
      rv32_smc.word_dropped_ok, rv32_smc.word_repatch_ok,
      rv32_smc.word_fail_mode, rv32_smc.word_fail_old,
      rv32_smc.word_fail_size, rv32_smc.word_fail_address,
      rv32_smc.revive_calls, rv32_smc.revive_candidates,
      rv32_smc.revive_exact, rv32_smc.revive_patched,
      rv32_smc.revive_skipped, rv32_smc.revive_failed,
      (unsigned)rv32_ram_cache_flush_count);
  printf("BENCH_JIT_RAM_UNITS version=1 peak_units=%u peak_nodes=%u "
      "peak_refs=%u peak_patches=%u peak_copy_bytes=%u peak_links=%u "
      "add_while_full=%u add_stage_full=%u add_bad_range=%u "
      "add_units_full=%u add_nodes_full=%u add_refs_full=%u "
      "add_copy_full=%u patch_table_full=%u links_full=%u full=%u\n",
      units.units, units.nodes, units.refs, units.patches,
      units.copy_bytes, units.links, units.add_while_full,
      units.add_stage_full, units.add_bad_range, units.add_units_full,
      units.add_nodes_full, units.add_refs_full, units.add_copy_full,
      units.patch_table_full, units.links_full, (unsigned)units.full);
}
#else
#define rv32_smc_count(field) ((void)0)
#endif

/* The entry for pc of a dormant unit translated from the current bytes,
   revived; NULL when there is none. *failed reports a revival that left
   state half published. */
static bool rv32_ram_unit_edits(unsigned id);
static void rv32_ram_unit_apply(unsigned id, bool live);

/* Reject units lacking a switch patch at a newly learned slot, so
   retranslation can emit it. Known slots may lie in an untranslated tail
   and need no patch there. The list is append-only until a full dynarec
   flush discards all units. */
static bool rv32_ram_unit_switches_ok(unsigned id)
{
  const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(id);
  const uint32_t *slots;
  unsigned count = gpsp_rv32_arm_flow_switch_list(&slots);
  for (unsigned i = unit->switches; !unit->thumb && i < count; i++)
  {
    u32 word;
    if (slots[i] < unit->guest_start || slots[i] >= unit->guest_end)
      continue;
    memcpy(&word, rv32_ram_guest(slots[i]), 4);
    const gpsp_rv32_ram_patch *patch = gpsp_rv32_ram_unit_patch(id,
        slots[i]);
    if ((word == 0xe1a00000 || (word & 0xff000000) == 0xea000000) &&
        (!patch || patch->mode != GPSP_RV32_ARM_FLOW_OP_SWITCH))
      return false;
  }
  return true;
}

/* Reject static ops at learned sites so retranslation emits dynamic loads. */
static bool rv32_ram_unit_immediates_ok(unsigned id)
{
  const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(id);
  const gpsp_rv32_ram_patch *patches = gpsp_rv32_ram_unit_patches(id);
  for (unsigned i = 0; i < unit->patches; i++)
  {
    u32 word;
    if (patches[i].mode != GPSP_RV32_ARM_FLOW_OP_PLAIN &&
        patches[i].mode != GPSP_RV32_ARM_FLOW_OP_NO_FLAGS)
      continue;
    memcpy(&word, rv32_ram_guest(patches[i].guest), 4);
    if (gpsp_rv32_arm_flow_dynamic_supported(word) &&
        gpsp_rv32_arm_flow_immediate_known(patches[i].guest))
      return false;
  }
  return true;
}

/* A unit made before a cut was learned would drop itself again. */
static bool rv32_ram_unit_spans_cut(const gpsp_rv32_ram_unit *unit)
{
  for (unsigned i = 0; i < rv32_ram_cut_count; i++)
  {
    if (rv32_ram_cuts[i].thumb == unit->thumb &&
        unit->guest_start >= rv32_ram_cuts[i].low &&
        unit->guest_start < rv32_ram_cuts[i].at &&
        unit->guest_end > rv32_ram_cuts[i].at)
      return true;
  }
  return false;
}

/* Exact copies first; then a unit that differs only in changes it can
   patch, which saves a translation. */
static u8 *rv32_revive_ram_unit(u32 pc, bool thumb, bool *failed)
{
  u16 ids[16];
  unsigned count = gpsp_rv32_ram_units_dormant(pc, thumb, ids, 16);
#if defined(GPSP_BENCHMARK_RAM_CODE)
  if (count)
  {
    rv32_smc.revive_calls++;
    rv32_smc.revive_candidates += count;
  }
#endif
  u32 key = pc | (u32)thumb;
  u32 head_pc = (pc >> 24) == 0x02 ? 0x02000000 | (pc & 0x3ffff) :
      0x03000000 | (pc & 0x7fff);
  for (unsigned pass = 0; pass < 2 * count; pass++)
  {
    unsigned i = pass % count;
    bool patch = pass >= count;
    const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(ids[i]);
    const gpsp_rv32_ram_ref *refs = gpsp_rv32_ram_unit_refs(ids[i]);
    u32 entry = 0;
    if (unit->head && rv32_canonical_ram_pc(pc) && head_pc == unit->guest_start)
      entry = unit->head;
    for (unsigned k = 0; !entry && k < unit->entries; k++)
    {
      if (refs[k].key == key)
        entry = refs[k].offset;
    }
    if (!entry || !rv32_ram_unit_switches_ok(ids[i]) ||
        !rv32_ram_unit_immediates_ok(ids[i]) ||
        rv32_ram_unit_spans_cut(unit) ||
        (patch ? !rv32_ram_unit_edits(ids[i]) :
         !gpsp_rv32_ram_unit_matches(ids[i],
             rv32_ram_guest(unit->guest_start))))
      continue;
    if (patch)
      rv32_ram_unit_apply(ids[i], false);
    rv32_restore_result restored = rv32_restore_ram_unit(ids[i]);
    /* A skipped restore leaves the unit to a normal translation. */
    if (restored != RV32_RESTORE_DONE)
    {
      *failed = restored == RV32_RESTORE_FAILED;
#if defined(GPSP_BENCHMARK_RAM_CODE)
      if (*failed)
        rv32_smc.revive_failed++;
      else
        rv32_smc.revive_skipped++;
#endif
      return NULL;
    }
#if defined(GPSP_BENCHMARK_RAM_CODE)
    if (patch)
      rv32_smc.revive_patched++;
    else
      rv32_smc.revive_exact++;
#endif
#if defined(GPSP_RV32_TEST_RAM_SMC)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram revive: pc=%08x native=%u used=%u patched=%u",
        (unsigned)pc, (unsigned)unit->native_start,
        (unsigned)rv32_ram_offset(ram_translation_ptr), (unsigned)patch);
#endif
    return rv32_ram_at(entry);
  }
  return NULL;
}

/* In-place patches. An ARM unit whose bytes changed only in
   data-processing imm8 fields or B targets keeps its native code: each
   changed op is emitted again over its old code, which must keep its size.
   ARM units share no code with other units. */
static bool rv32_safe_imm_patch(u32 old_word, u32 new_word)
{
  return !((old_word ^ new_word) & ~0xffu) &&
      gpsp_rv32_arm_flow_imm_op(old_word);
}

enum { RV32_PATCH_EDITS = 8 };

typedef struct
{
  u8 *native;
  unsigned bytes;
  u32 key;              /* branch: new target key, else 0 */
  uint32_t code[64];
} rv32_ram_edit;

static u32 rv32_arm_b_target(u32 pc, u32 opcode)
{
  return pc + 8 + (u32)(((s32)(opcode << 8) >> 8) * 4);
}

/* Switch slot trick: a slot takes NOP or an always-taken B. Its
   flags were all kept, so its target may lie inside the unit. */
static bool rv32_switch_word(u32 word)
{
  return word == 0xe1a00000 || (word & 0xff000000) == 0xea000000;
}

static u32 rv32_switch_target(u32 pc, u32 word)
{
  return word == 0xe1a00000 ? pc + 4 : rv32_arm_b_target(pc, word);
}

/* A B or BL to a new target outside the unit. Its exit link moves to the
   new target; flags live there must have been kept for the old one. */
static bool rv32_ram_edit_branch(const gpsp_rv32_ram_unit *unit,
    unsigned id, const gpsp_rv32_ram_patch *patch, u32 old_word,
    u32 new_word, rv32_ram_edit *edit)
{
  u32 old_target, new_target;
  if (patch->mode == GPSP_RV32_ARM_FLOW_OP_SWITCH)
  {
    if (!rv32_switch_word(old_word) || !rv32_switch_word(new_word))
      return false;
    old_target = rv32_switch_target(patch->guest, old_word);
    new_target = rv32_switch_target(patch->guest, new_word);
  }
  else
  {
    if ((old_word ^ new_word) & 0xff000000)
      return false;
    old_target = rv32_arm_b_target(patch->guest, old_word);
    new_target = rv32_arm_b_target(patch->guest, new_word);
    if ((old_target >= unit->guest_start &&
         old_target <= unit->guest_end) ||
        (new_target >= unit->guest_start &&
         new_target <= unit->guest_end))
      return false;
  }

  /* The exit key holds the PC the unit ran at, maybe a mirror. */
  u32 site = patch->native + patch->bytes;
  const gpsp_rv32_ram_ref *refs = gpsp_rv32_ram_unit_refs(id);
  unsigned i = unit->entries;
  while (i < unit->entries + unit->exits && refs[i].offset != site)
    i++;
  if (i == unit->entries + unit->exits)
    return false;
  u32 base = refs[i].key - old_target;
  u32 key = new_target + base;
  if (!rv32_ram_pc(key) || (key & 3) || key == cheat_master_hook ||
      (rv32_arm_live_flags(key, unit->guest_start, unit->guest_end) &
       ~patch->live))
    return false;

  edit->native = rv32_ram_at(patch->native);
  edit->bytes = patch->bytes;
  edit->key = key;
  return gpsp_rv32_arm_flow_repatch(edit->native, patch->bytes, patch->mode,
      patch->guest + base, old_word, new_word, (uint8_t *)edit->code);
}

static void rv32_drop_pending_ram_link(u8 *site)
{
  for (u32 i = 0; i < rv32_pending_ram_link_count;)
  {
    if (rv32_pending_ram_links[i].site == site)
      rv32_pending_ram_links[i] =
          rv32_pending_ram_links[--rv32_pending_ram_link_count];
    else
      i++;
  }
}

/* Links a retargeted exit slot as a new translation would. */
static void rv32_relink_ram_site(unsigned id, u8 *site, u32 key)
{
  u32 offset = rv32_ram_offset(site);
  if (!gpsp_rv32_unlink_link_slot(site))
  {
    rv32_unlink_failed = true;
    return;
  }
  gpsp_rv32_ram_link_drop(offset);
  rv32_drop_pending_ram_link(site);
  gpsp_rv32_ram_unit_retarget(id, offset, key);
  u8 *published = rv32_published_ram_block(key);
  if (!published)
    rv32_add_pending_ram_link(site, key);
  else if (!rv32_patch_ram_link(site, published, ram_translation_ptr, false,
          key))
    rv32_unlink_failed = true;
}

static rv32_ram_edit rv32_ram_edits[RV32_PATCH_EDITS];
static unsigned rv32_ram_edit_count;
#if defined(GPSP_RV32_TEST_RAM_SMC)
/* Patch metadata stays valid until the prepared edits are applied. */
static const gpsp_rv32_ram_patch *rv32_ram_test_patches[RV32_PATCH_EDITS];
#endif

/* Prepares the edits that turn an ARM unit's code into code for its current
   guest bytes: changes in immediates and B targets with patch points.
   False when some change has none. */
static bool rv32_ram_unit_edits(unsigned id)
{
  const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(id);
  if (unit->thumb || !unit->patches)
  {
    rv32_smc_count(unit_not_patchable);
    return false;
  }

  rv32_ram_edit *edits = rv32_ram_edits;
  unsigned edit_count = 0;
  const u8 *old = gpsp_rv32_ram_unit_copy(id);
  const u8 *now = rv32_ram_guest(unit->guest_start);
  u32 bytes = unit->guest_end - unit->guest_start;
  if (bytes & 3)
  {
    rv32_smc_count(unit_not_patchable);
    return false;
  }
  for (u32 offset = 0; offset < bytes; offset += 4)
  {
    u32 old_word, new_word;
    memcpy(&old_word, old + offset, 4);
    memcpy(&new_word, now + offset, 4);
    if (old_word == new_word)
      continue;
    /* A NOP swapped with B becomes a switch slot when next translated. */
    if ((old_word == 0xe1a00000 && (new_word >> 24) == 0xea) ||
        (new_word == 0xe1a00000 && (old_word >> 24) == 0xea))
    {
#if defined(GPSP_RV32_TEST_RAM_SMC)
      bool known = gpsp_rv32_arm_flow_switch_known(unit->guest_start + offset);
#endif
      gpsp_rv32_arm_flow_switch_note(unit->guest_start + offset);
#if defined(GPSP_RV32_TEST_RAM_SMC)
      if (!known && gpsp_rv32_arm_flow_switch_known(unit->guest_start + offset))
      {
        const uint32_t *slots;
        gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
            "test ram switch: pc=%08x count=%u",
            (unsigned)(unit->guest_start + offset),
            gpsp_rv32_arm_flow_switch_list(&slots));
      }
#endif
    }
    const gpsp_rv32_ram_patch *patch = gpsp_rv32_ram_unit_patch(id,
        unit->guest_start + offset);
    /* A dynamic immediate reads the new byte itself. */
    if (patch && patch->mode == GPSP_RV32_ARM_FLOW_OP_DYNAMIC)
    {
      if ((old_word ^ new_word) & ~0xffu)
      {
        rv32_smc_count(word_dynamic_reject);
        return false;
      }
      rv32_smc_count(word_dynamic_ok);
      continue;
    }
#if defined(GPSP_BENCHMARK_RAM_CODE)
    if (!patch)
    {
      rv32_smc_count(word_no_patch);
      return false;
    }
    if (edit_count == RV32_PATCH_EDITS)
    {
      rv32_smc_count(word_edits_full);
      return false;
    }
#else
    if (!patch || edit_count == RV32_PATCH_EDITS)
      return false;
#endif
#if defined(GPSP_RV32_TEST_RAM_SMC)
    rv32_ram_test_patches[edit_count] = patch;
#endif
    if (patch->mode == GPSP_RV32_ARM_FLOW_OP_BRANCH ||
        patch->mode == GPSP_RV32_ARM_FLOW_OP_SWITCH)
    {
#if defined(GPSP_BENCHMARK_RAM_CODE)
      gpsp_rv32_arm_flow_repatch_failure = 0;
#endif
      if (!rv32_ram_edit_branch(unit, id, patch, old_word, new_word,
              &edits[edit_count]))
      {
#if defined(GPSP_BENCHMARK_RAM_CODE)
        rv32_smc_repatch_failed();
#endif
        return false;
      }
      rv32_smc_count(word_branch_ok);
      edit_count++;
      continue;
    }
    if (!rv32_safe_imm_patch(old_word, new_word))
    {
      rv32_smc_count(word_imm_unsafe);
      return false;
    }
    /* Retranslate learned sites with dynamic immediates. */
    if (patch->mode != GPSP_RV32_ARM_FLOW_OP_DROPPED &&
        gpsp_rv32_arm_flow_dynamic_supported(new_word) &&
        gpsp_rv32_arm_flow_immediate_note(unit->guest_start + offset,
            rv32_ram_guest(unit->guest_start + offset)))
    {
      rv32_smc_count(word_learned);
      return false;
    }
    u8 *native = rv32_ram_at(patch->native);
#if defined(GPSP_BENCHMARK_RAM_CODE)
    gpsp_rv32_arm_flow_repatch_failure = 0;
#endif
    if (!gpsp_rv32_arm_flow_repatch(native, patch->bytes, patch->mode,
            unit->guest_start + offset, old_word, new_word,
            (uint8_t *)edits[edit_count].code))
    {
#if defined(GPSP_BENCHMARK_RAM_CODE)
      rv32_smc_repatch_failed();
#endif
      return false;
    }
#if defined(GPSP_BENCHMARK_RAM_CODE)
    if (patch->mode == GPSP_RV32_ARM_FLOW_OP_DROPPED)
      rv32_smc.word_dropped_ok++;
    else
      rv32_smc.word_repatch_ok++;
#endif
    if (patch->mode != GPSP_RV32_ARM_FLOW_OP_DROPPED)
    {
      edits[edit_count].native = native;
      edits[edit_count].bytes = patch->bytes;
      edits[edit_count].key = 0;
      edit_count++;
    }
  }
  rv32_ram_edit_count = edit_count;
  return true;
}

/* Writes the prepared edits. A live unit's moved exits link again now and
   its code is synced; a dormant unit does both when it is revived. */
static void rv32_ram_unit_apply(unsigned id, bool live)
{
  for (unsigned i = 0; i < rv32_ram_edit_count; i++)
  {
    rv32_ram_edit *edit = &rv32_ram_edits[i];
    u8 *end = edit->native + edit->bytes;
    memcpy(edit->native, edit->code, edit->bytes);
    if (edit->key)
    {
      if (live)
        rv32_relink_ram_site(id, end, edit->key);
      else
        gpsp_rv32_ram_unit_retarget(id, rv32_ram_offset(end),
            edit->key);
      end += GPSP_RV32_LINK_SLOT_SIZE;
    }
    if (live)
    {
      platform_cache_sync(edit->native, end);
      if (!rv32_cache_sync_ok)
        rv32_unlink_failed = true;
    }
#if defined(GPSP_RV32_TEST_RAM_SMC)
    if (live && !rv32_unlink_failed)
    {
      const gpsp_rv32_ram_patch *patch = rv32_ram_test_patches[i];
      gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
          "test ram patch: pc=%08x native=%u used=%u switch=%u",
          (unsigned)patch->guest, (unsigned)rv32_ram_offset(edit->native),
          (unsigned)rv32_ram_offset(ram_translation_ptr),
          (unsigned)(patch->mode == GPSP_RV32_ARM_FLOW_OP_SWITCH));
    }
#endif
  }
  gpsp_rv32_ram_unit_update(id,
      rv32_ram_guest(gpsp_rv32_ram_unit_get(id)->guest_start));
}

/* Changed imm8 bytes need no invalidation if all live units accept them. */
bool gpsp_rv32_code_write_absorbed(u32 address, unsigned bytes, u32 before,
    u32 after)
{
  unsigned shift = (address & 3) * 8;
  u32 mask = bytes >= 4 ? ~0u : (1u << (bytes * 8)) - 1;
  u32 word = rv32_ram_physical(address & ~3u);
  u16 ids[16];
  if ((((before ^ after) & mask) << shift) & ~0xffu ||
      !gpsp_rv32_arm_flow_immediate_known(word))
    return false;
  unsigned count = gpsp_rv32_ram_units_find(word, 4, ids, 16);
  if (!count || count == 16)
    return false;
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_patch *patch = gpsp_rv32_ram_unit_patch(ids[i], word);
    if (gpsp_rv32_ram_unit_get(ids[i])->thumb || !patch ||
        (patch->mode != GPSP_RV32_ARM_FLOW_OP_DYNAMIC &&
         patch->mode != GPSP_RV32_ARM_FLOW_OP_DROPPED))
      return false;
  }
  return true;
}

/* Patches a live unit; false leaves its code unchanged. */
static bool rv32_patch_ram_unit(unsigned id)
{
  if (!rv32_ram_unit_edits(id))
    return false;
  rv32_ram_unit_apply(id, true);
  return true;
}

/* Empties the entries that enter the given dropped RAM units; others
   still point at live code. */
static void rv32_branch_cache_drop(const u16 *ids, unsigned count)
{
  /* Native address bounds skip most entries without a chunk search. */
  u32 low = UINT32_MAX, high = 0;
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(ids[i]);
    u32 start = (u32)(uintptr_t)rv32_ram_at(unit->native_start);
    low = MIN(low, start);
    high = MAX(high, start + (unit->native_end - unit->native_start));
  }
  u32 miss = (u32)(uintptr_t)gpsp_rv32_branch_cache_miss;
  for (u32 i = 0; i < GPSP_RV32_BRANCH_CACHE_SIZE; i++)
  {
    u32 target = gpsp_rv32_branch_cache[i * 2 + 1];
    if (gpsp_rv32_branch_cache[i * 2] == ~0u || target < low ||
        target >= high || !gpsp_rv32_ram_units_contain(
            rv32_ram_offset((const u8 *)(uintptr_t)target), ids, count))
      continue;
    gpsp_rv32_branch_cache[i * 2] = ~0u;
    gpsp_rv32_branch_cache[i * 2 + 1] = miss;
  }
}

/* Drops the units that cover the queued code writes. False when the writes
   or units were not all tracked; the caller then flushes the RAM cache. */
static bool rv32_invalidate_ram_writes(void)
{
  rv32_smc_count(write_events);
  if (gpsp_rv32_ram_dirty_overflow())
  {
    rv32_smc_count(flush_dirty_overflow);
    return false;
  }
  if (gpsp_rv32_ram_units_full())
  {
    rv32_smc_count(flush_units_full);
    return false;
  }

  u16 ids[RV32_DROP_MAX];
  unsigned count = 0;
  /* Learn a cut only when a forward write drops the resume PC's unit. */
  bool resume_thumb = (reg[REG_CPSR] >> 5) & 1;
  u32 resume = rv32_ram_physical(reg[REG_PC]);
  u32 cut = 0;
  int cut_unit = -1;
  for (unsigned i = 0; i < gpsp_rv32_ram_dirty_count(); i++)
  {
    unsigned bytes;
    u32 address = gpsp_rv32_ram_dirty_get(i, &bytes);
#if defined(GPSP_RV32_TEST_RAM_SMC)
    gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
        "test ram write: pc=%08x bytes=%u", (unsigned)address, bytes);
#endif
    u16 found[RV32_DROP_MAX];
    unsigned found_count = gpsp_rv32_ram_units_find(address, bytes, found,
        RV32_DROP_MAX);
    for (unsigned k = 0; k < found_count; k++)
    {
      const gpsp_rv32_ram_unit *unit = gpsp_rv32_ram_unit_get(found[k]);
      if (((reg[REG_PC] >> 24) == 0x02 || (reg[REG_PC] >> 24) == 0x03) &&
          unit->thumb == resume_thumb &&
          resume >= unit->guest_start && resume < unit->guest_end &&
          address > resume && (!cut || address < cut))
      {
        cut = address;
        cut_unit = found[k];
      }
    }
    /* A full result may have dropped units. */
    if (found_count == RV32_DROP_MAX)
    {
#if defined(GPSP_RV32_TEST_RAM_SMC)
      u16 covered[RV32_DROP_MAX + 1];
      unsigned live = gpsp_rv32_ram_units_find(address, bytes, covered,
          RV32_DROP_MAX + 1);
      gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram",
          "test unit limit: live=%u", live);
#endif
      gpsp_rv32_log(GPSP_RV32_LOG_WARN, "rv32_ram",
          "code write at %08x covers %u or more units; flushing",
          (unsigned)address, (unsigned)RV32_DROP_MAX);
      rv32_smc_count(flush_unit_limit);
      return false;
    }
    for (unsigned k = 0; k < found_count; k++)
    {
      unsigned j = 0;
      while (j < count && ids[j] != found[k])
        j++;
      if (j < count)
        continue;
      if (count == RV32_DROP_MAX)
      {
        rv32_smc_count(flush_unit_limit);
        return false;
      }
      ids[count++] = found[k];
    }
  }
  gpsp_rv32_ram_dirty_clear();

  /* Units changed only in immediates stay live with patched code. */
  rv32_unlink_failed = false;
  unsigned kept = 0;
  for (unsigned i = 0; i < count; i++)
  {
    if (!rv32_patch_ram_unit(ids[i]))
      ids[kept++] = ids[i];
  }
#if defined(GPSP_BENCHMARK_RAM_CODE)
  /* Patched units count once the whole update succeeds. */
  unsigned patched = count - kept;
  rv32_smc.units_seen += count;
#endif
  count = kept;
  if (rv32_unlink_failed)
  {
    rv32_smc_count(flush_patch_sync);
    return false;
  }
  for (unsigned i = 0; cut && i < count; i++)
  {
    if (ids[i] == cut_unit)
      rv32_ram_cut_note(gpsp_rv32_ram_unit_get(ids[i])->guest_start,
          cut & ~(resume_thumb ? 1u : 3u), resume_thumb);
  }

  /* Tags past a shortened flow block can match no unit. */
  if (!count)
  {
#if defined(GPSP_BENCHMARK_RAM_CODE)
    rv32_smc.units_patched += patched;
#endif
    return true;
  }

  for (unsigned i = 0; i < count; i++)
  {
    rv32_drop_ram_head(gpsp_rv32_ram_unit_get(ids[i]), ids[i]);
    gpsp_rv32_ram_unit_kill(ids[i]);
  }
  rv32_ram_entry_drop(ids, count);

  u32 i = 0;
  while (i < rv32_pending_ram_link_count)
  {
    u32 site = rv32_ram_offset(rv32_pending_ram_links[i].site);
    if (gpsp_rv32_ram_units_contain(site, ids, count))
      rv32_pending_ram_links[i] =
          rv32_pending_ram_links[--rv32_pending_ram_link_count];
    else
      i++;
  }

  rv32_unlink_failed = false;
  gpsp_rv32_ram_links_kill(ids, count, rv32_unlink_ram_site);
  if (rv32_unlink_failed || !rv32_retag_ram(ids, count))
  {
    rv32_smc_count(flush_unlink);
    return false;
  }
#if defined(GPSP_BENCHMARK_RAM_CODE)
  rv32_smc.units_patched += patched;
  rv32_smc.units_dropped += count;
#endif

  /* Shared exits may hold dropped code; resume points are checked by the
     generation. */
  rv32_branch_cache_drop(ids, count);
  rv32_ram_cache_generation++;
  return true;
}
#endif

void invalidate_translation_cache_ram(void)
{
#if defined(GPSP_RV32_TEST_RAM_SMC)
  /* Count handled SMC alerts; pending bits may combine several raises. */
  gpsp_rv32_log(GPSP_RV32_LOG_INFO, "rv32_ram", "test ram alert: pc=%08x",
      (unsigned)reg[REG_PC]);
#endif
#if defined(GPSP_RV32_JIT)
  bool wait_flush = false;
  wait_flush = rv32_wait_flush_pending;
  /* An earlier exit may already have processed the pending writes. */
  if (!wait_flush && !gpsp_rv32_ram_dirty_count() &&
      !gpsp_rv32_ram_dirty_overflow())
    return;
#if defined(GPSP_BENCHMARK_RAM_CODE)
  if (wait_flush)
    rv32_smc_count(wait_flushes);
#endif
  if (!wait_flush && rv32_invalidate_ram_writes())
    return;
#endif
  flush_translation_cache_ram();
}

void flush_translation_cache_ram(void)
{
  rv32_lookup_clear_ram_ranges();
#if defined(GPSP_RV32_JIT)
  rv32_ram_cache_generation++;
  rv32_branch_cache_clear();
  rv32_ram_entry_clear();
  memset(rv32_ram_slow_entries, 0, sizeof(rv32_ram_slow_entries));
#endif
  /* Flushes RAM caches avoiding doing too much work (ie. wiping unused memory) */
  flush_ram_count++;
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  rv32_ram_cache_flush_count++;
  if (ram_translation_ptr)
  {
    u32 occupied = rv32_ram_offset(ram_translation_ptr);
    if (occupied > INITIAL_RAM_WATERMARK)
      rv32_ram_cache_discarded_bytes +=
          occupied - INITIAL_RAM_WATERMARK;
  }
#endif
  /*printf("ram flush %d (pc %x), %x to %x, %x to %x\n",
   flush_ram_count, reg[REG_PC], iwram_code_min, iwram_code_max,
   ewram_code_min, ewram_code_max);*/

#if defined(GPSP_RV32_JIT)
  gpsp_rv32_ram_arena.current = 0;
  last_ram_translation_ptr = rv32_ram_at(INITIAL_RAM_WATERMARK);
#else
  last_ram_translation_ptr = ram_translation_cache + INITIAL_RAM_WATERMARK;
#endif
  ram_translation_ptr = last_ram_translation_ptr;

  // Proceed to clean the SMC area if needed
  // (also try to memset as little as possible for performance)
  if (iwram_code_min != ~0U) {
    if(iwram_code_max >= iwram_code_min) {
      iwram_code_min &= ~15U;
      iwram_code_max = MIN(iwram_code_max + 8, 0x8000);
      memset(&iwram[iwram_code_min], 0, iwram_code_max - iwram_code_min);
    } else
      memset(iwram, 0, 0x8000);
  }

  if (ewram_code_min != ~0U) {
    if(ewram_code_max >= ewram_code_min) {
      ewram_code_min &= ~15U;
      ewram_code_max = MIN(ewram_code_max + 8, 0x40000);
      memset(&ewram[0x40000 + ewram_code_min], 0, ewram_code_max - ewram_code_min);
    } else
      memset(&ewram[0x40000], 0, 0x40000);
  }

  iwram_code_min = ~0U;
  iwram_code_max =  0U;
  ewram_code_min = ~0U;
  ewram_code_max =  0U;
  ram_block_tag = INITIAL_TOP_TAG;
#if defined(GPSP_RV32_JIT)
  rv32_clear_pending_ram_links();
  gpsp_rv32_ram_units_reset();
  gpsp_rv32_ram_dirty_clear();
#endif
#if defined(GPSP_RV32_JIT)
  if (rv32_wait_flush_pending)
  {
    rv32_wait_flush_pending = false;
    flush_translation_cache_rom();
  }
#endif
}

void flush_translation_cache_rom(void)
{
  rv32_lookup_clear_rom_ranges();
#if defined(GPSP_RV32_JIT)
  rv32_rom_cache_generation++;
  rv32_branch_cache_clear();
  rv32_rom_entry_clear();
  memset(rv32_rom_slow_entries, 0, sizeof(rv32_rom_slow_entries));
#endif
#if defined(GPSP_RV32_JIT) && defined(GPSP_BENCHMARK)
  rv32_rom_cache_flush_count++;
  if (rom_translation_ptr)
  {
    u32 occupied = rv32_rom_offset(rom_translation_ptr);
    rv32_compile_stats.flushed_bytes += occupied;
    if (occupied > rv32_compile_stats.max_flush_bytes)
      rv32_compile_stats.max_flush_bytes = occupied;
    if (occupied > rom_cache_watermark)
      rv32_rom_cache_discarded_bytes += occupied - rom_cache_watermark;
  }
#endif
  /* We flush the generated code except for everything below the watermark. */
#if defined(GPSP_RV32_JIT)
  gpsp_rv32_rom_arena.current = 0;
  last_rom_translation_ptr = rv32_rom_at(rom_cache_watermark);
  rom_translation_ptr      = last_rom_translation_ptr;
#else
  last_rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  rom_translation_ptr      = &rom_translation_cache[rom_cache_watermark];
#endif

  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
#if defined(GPSP_RV32_JIT)
  rv32_clear_pending_links();
#endif
}

void init_dynarec_caches(void)
{
  rv32_lookup_init_ranges();
#if defined(GPSP_RV32_JIT)
  rv32_rom_cache_generation++;
  rv32_ram_cache_generation++;
  rv32_branch_cache_clear();
  rv32_rom_entry_clear();
  rv32_ram_entry_clear();
  memset(rv32_rom_slow_entries, 0, sizeof(rv32_rom_slow_entries));
  memset(rv32_ram_slow_entries, 0, sizeof(rv32_ram_slow_entries));
#endif
  /* Initialize caches so that we can start initalizing the emitter. */
#if defined(GPSP_RV32_JIT)
  gpsp_rv32_rom_arena.current = 0;
  gpsp_rv32_ram_arena.current = 0;
  rom_translation_ptr = last_rom_translation_ptr = rv32_rom_at(0);
#else
  rom_translation_ptr = last_rom_translation_ptr = &rom_translation_cache[0];
#endif
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
#if defined(GPSP_RV32_JIT)
  rv32_clear_pending_links();
  rv32_clear_pending_ram_links();
  gpsp_rv32_ram_units_reset();
  gpsp_rv32_ram_dirty_clear();
#if defined(GPSP_BENCHMARK)
  rv32_rom_cache_discarded_bytes = 0;
  rv32_ram_cache_discarded_bytes = 0;
  rv32_chunk_retries = 0;
  rv32_chunk_tail_bytes = 0;
  memset(&rv32_compile_stats, 0, sizeof(rv32_compile_stats));
  memset(&rv32_site_stats, 0, sizeof(rv32_site_stats));
  memset(&rv32_pending_site_stats, 0, sizeof(rv32_pending_site_stats));
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
  memset(&rv32_lookup_stats, 0, sizeof(rv32_lookup_stats));
#endif
#endif
#endif

#if defined(GPSP_RV32_JIT)
  ram_translation_ptr = last_ram_translation_ptr =
      rv32_ram_at(INITIAL_RAM_WATERMARK);
#else
  ram_translation_ptr = last_ram_translation_ptr =
      &ram_translation_cache[INITIAL_RAM_WATERMARK];
#endif
  memset(iwram, 0, 0x8000);
  memset(&ewram[0x40000], 0, 0x40000);

  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
}

void flush_dynarec_caches(void)
{
  /* Flush ROM and RAM caches. */
#if defined(GPSP_RV32_JIT)
  gpsp_rv32_arm_flow_switch_clear();
  gpsp_rv32_arm_flow_immediate_clear();
#endif
#if defined(GPSP_RV32_JIT)
  rv32_ram_cut_count = 0;
#endif
  flush_translation_cache_rom();
  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
  flush_translation_cache_ram();
}

void gpsp_jit_translation_stats(uint32_t *blocks, uint32_t *cycles)
{
#if defined(GPSP_RV32_JIT) && defined(__riscv) && \
    !defined(GPSP_BENCHMARK_JIT_LOOKUP)
  *blocks = rv32_translations;
  *cycles = rv32_translate_cycles;
#else
  *blocks = 0;
  *cycles = 0;
#endif
}


void gpsp_jit_upkeep(void)
{
#if defined(GPSP_RV32_JIT)
  /* Calls to skip after a failed allocation, about ten seconds. */
  enum { RV32_UPKEEP_RETRY_CALLS = 600 };
  /* Room kept below the 3/4 load limit, about 1,200 Thumb blocks; one
     translation burst seldom fills it, so it rarely has to rehash. */
  enum { RV32_ROM_ENTRY_HEADROOM = 16384 };
  static u32 retry_wait;

  if (retry_wait)
  {
    retry_wait--;
    return;
  }
  if (rv32_rom_entry_capacity &&
      rv32_rom_entry_count + RV32_ROM_ENTRY_HEADROOM >=
          rv32_rom_entry_capacity / 4 * 3 &&
      !rv32_rom_entry_grow())
    retry_wait = RV32_UPKEEP_RETRY_CALLS;
#endif
}

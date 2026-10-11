/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../common.h"
#include "rv32_arm_memory.h"

#include "rv32_arm_branch.h"
#include "rv32_register_map.h"
#include "rv32_rom_load.h"
#include "rv32_stub.h"
#include "rv32_thumb_memory.h"

#include <string.h>

/* Private JIT ABI: a1=address, a2=value, a0=return address. */
extern void gpsp_rv32_arm_palette_store32(void);
extern void gpsp_rv32_arm_palette_store32_compact(void);
/* Same ABI; they return to a0 for the C path and to a0 + 4 when done.
   Loads leave the value in a0. */
extern void gpsp_rv32_arm_io_load16(void);
extern void gpsp_rv32_arm_io_load32(void);
extern void gpsp_rv32_arm_io_store16(void);

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
extern uint64_t gpsp_rv32_jit_arm_memory_calls;
extern uint64_t gpsp_rv32_jit_arm_block_calls;
extern uint64_t gpsp_rv32_jit_arm_memory_cycles;
extern uint64_t gpsp_rv32_jit_arm_block_cycles;
extern uint64_t gpsp_rv32_jit_arm_memory_ram;
extern uint64_t gpsp_rv32_jit_arm_memory_io;
extern uint64_t gpsp_rv32_jit_arm_memory_video;
extern uint64_t gpsp_rv32_jit_arm_memory_rom;
extern uint64_t gpsp_rv32_jit_arm_memory_save;
extern uint64_t gpsp_rv32_jit_arm_memory_other;
extern uint64_t gpsp_rv32_jit_arm_memory_stores;
extern uint64_t gpsp_rv32_jit_rom_read_body_cycles;
extern uint64_t gpsp_rv32_jit_rom_read_body_calls;
extern uint64_t gpsp_rv32_jit_rom_read_page_misses;

static struct gpsp_rv32_arm_video_memory_stats arm_video_memory_stats;
static struct gpsp_rv32_arm_video_source_stats arm_video_source_stats;

void gpsp_rv32_arm_video_memory_get_stats(
    struct gpsp_rv32_arm_video_memory_stats *stats)
{
  *stats = arm_video_memory_stats;
}

void gpsp_rv32_arm_video_source_get_stats(
    struct gpsp_rv32_arm_video_source_stats *stats)
{
  *stats = arm_video_source_stats;
}

static inline uint32_t arm_memory_cycles_now(void)
{
  uint32_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles) :: "memory");
  return cycles;
}

static inline void arm_memory_finish_timing(uint32_t start, bool rom_read,
    unsigned region, gpsp_rv32_memory_kind kind, uint32_t pc)
{
  uint32_t elapsed = arm_memory_cycles_now() - start;
  gpsp_rv32_jit_arm_memory_cycles += elapsed;
  if (rom_read)
    gpsp_rv32_jit_rom_read_body_cycles += elapsed;
  if (region >= 0x05 && region <= 0x07)
  {
    unsigned width = kind == GPSP_RV32_MEM_LOAD_U32 ||
        kind == GPSP_RV32_MEM_STORE_U32 ? 2 :
        kind == GPSP_RV32_MEM_LOAD_U16 ||
        kind == GPSP_RV32_MEM_LOAD_S16 ||
        kind == GPSP_RV32_MEM_STORE_U16 ? 1 : 0;
    unsigned store = kind >= GPSP_RV32_MEM_STORE_U8;
    arm_video_memory_stats.calls[region - 0x05][store][width]++;
    arm_video_memory_stats.cycles[region - 0x05][store][width] += elapsed;
    if (store && pc < 0x4000)
    {
      arm_video_source_stats.bios_store_calls[region - 0x05]++;
      arm_video_source_stats.bios_store_cycles[region - 0x05] += elapsed;
      if (pc == 0x7b4)
      {
        arm_video_source_stats.pc_7b4_store_calls[region - 0x05]++;
        arm_video_source_stats.pc_7b4_store_cycles[region - 0x05] += elapsed;
      }
    }
  }
}
#endif

typedef struct
{
  unsigned rn;
  unsigned rd;
  unsigned rm;
  unsigned kind;
  unsigned shift;
  unsigned shift_type;
  uint32_t offset;
  bool register_offset;
  bool up;
  bool pre;
  bool writeback;
} arm_single_transfer;

static bool decode_single(uint32_t opcode, arm_single_transfer *decoded)
{
  unsigned operation;
  decoded->rn = (opcode >> 16) & 15;
  decoded->rd = (opcode >> 12) & 15;
  decoded->up = (opcode & 0x00800000) != 0;
  decoded->pre = (opcode & 0x01000000) != 0;
  decoded->writeback = !decoded->pre || (opcode & 0x00200000) != 0;
  decoded->register_offset = false;
  decoded->shift = 0;
  decoded->shift_type = 0;
  decoded->offset = 0;

  if ((opcode & 0x0c000000) == 0x04000000)
  {
    bool load = (opcode & 0x00100000) != 0;
    bool byte = (opcode & 0x00400000) != 0;
    decoded->kind = load ? (byte ? GPSP_RV32_MEM_LOAD_U8 :
        GPSP_RV32_MEM_LOAD_U32) : (byte ? GPSP_RV32_MEM_STORE_U8 :
        GPSP_RV32_MEM_STORE_U32);
    decoded->register_offset = (opcode & 0x02000000) != 0;
    if (decoded->register_offset)
    {
      /* Register-specified shifts do not encode a single transfer. */
      if (opcode & 0x10)
        return false;
      decoded->rm = opcode & 15;
      if (decoded->rm == 15)
        return false;
      decoded->shift = (opcode >> 7) & 31;
      decoded->shift_type = (opcode >> 5) & 3;
    }
    else
      decoded->offset = opcode & 0xfff;
  }
  else if ((opcode & 0x0e000090) == 0x00000090)
  {
    bool load = (opcode & 0x00100000) != 0;
    operation = (opcode >> 5) & 3;
    if (!operation || (!load && operation != 1))
      return false;
    decoded->kind = load ? (operation == 1 ? GPSP_RV32_MEM_LOAD_U16 :
        operation == 2 ? GPSP_RV32_MEM_LOAD_S8 :
                         GPSP_RV32_MEM_LOAD_S16) :
        GPSP_RV32_MEM_STORE_U16;
    decoded->register_offset = (opcode & 0x00400000) == 0;
    if (decoded->register_offset)
    {
      if (opcode & 0x00000f00)
        return false;
      decoded->rm = opcode & 15;
      if (decoded->rm == 15)
        return false;
    }
    else
      decoded->offset = ((opcode >> 4) & 0xf0) | (opcode & 15);
  }
  else
    return false;

  /* The ARM7TDMI cannot use PC as an updated base register. */
  return decoded->rn != 15 || !decoded->writeback;
}

static bool emit_constant(gpsp_rv32_code *code, unsigned rd, uint32_t value)
{
  int low = (int)(value & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((value + 0x800u) >> 12) & 0xfffff;
  if (!upper)
    return gpsp_rv32_addi(code, rd, X0, low);
  return gpsp_rv32_lui(code, rd, upper) &&
      (!low || gpsp_rv32_addi(code, rd, rd, low));
}

static bool emit_call(gpsp_rv32_code *code, uintptr_t target)
{
  if (target > UINT32_MAX)
    return false;
  int low = (int)(target & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = (((uint32_t)target + 0x800u) >> 12) & 0xfffff;
  return gpsp_rv32_lui(code, RV_TMP1, upper) &&
      gpsp_rv32_jalr(code, RA, RV_TMP1, low);
}

static bool emit_guest_read(gpsp_rv32_code *code, unsigned rd,
    unsigned guest, uint32_t pc, unsigned pc_offset)
{
  return guest == 15 ? emit_constant(code, rd, pc + pc_offset) :
      gpsp_rv32_addi(code, rd, guest_reg_map[guest], 0);
}

static bool emit_offset(gpsp_rv32_code *code,
    const arm_single_transfer *decoded, uint32_t pc)
{
  if (!decoded->register_offset)
    return emit_constant(code, RV_TMP0, decoded->offset);
  if (!emit_guest_read(code, RV_TMP0, decoded->rm, pc, 8))
    return false;

  unsigned shift = decoded->shift;
  switch (decoded->shift_type)
  {
    case 0:
      return !shift || gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, shift);
    case 1:
      return shift ? gpsp_rv32_srli(code, RV_TMP0, RV_TMP0, shift) :
          gpsp_rv32_addi(code, RV_TMP0, X0, 0);
    case 2:
      return gpsp_rv32_srai(code, RV_TMP0, RV_TMP0, shift ? shift : 31);
    case 3:
      if (!shift)
        return gpsp_rv32_srli(code, RV_TMP0, RV_TMP0, 1) &&
            gpsp_rv32_slli(code, RV_TMP1, RV_FLAG_C, 31) &&
            gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_TMP1);
#if GPSP_RV32_USE_ZBB
      return gpsp_rv32_rori(code, RV_TMP0, RV_TMP0, shift);
#else
      return gpsp_rv32_srli(code, RV_TMP1, RV_TMP0, shift) &&
          gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, 32 - shift) &&
          gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_TMP1);
#endif
  }
  return false;
}

/* reg is the load target or the stored value; RV_TMP0 + offset the address. */
static bool emit_native_access(gpsp_rv32_code *code, unsigned kind,
    unsigned reg, int offset)
{
  switch (kind)
  {
    case GPSP_RV32_MEM_LOAD_U8:
      return gpsp_rv32_lbu(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_LOAD_U16:
      return gpsp_rv32_lhu(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_LOAD_U32:
      return gpsp_rv32_lw(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_LOAD_S8:
      return gpsp_rv32_lb(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_LOAD_S16:
      return gpsp_rv32_lh(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_STORE_U8:
      return gpsp_rv32_sb(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_STORE_U16:
      return gpsp_rv32_sh(code, reg, RV_TMP0, offset);
    case GPSP_RV32_MEM_STORE_U32:
      return gpsp_rv32_sw(code, reg, RV_TMP0, offset);
  }
  return false;
}

static bool emit_code_tag_check(gpsp_rv32_code *code, unsigned kind,
    bool ewram_region, uint8_t **slow_site)
{
  if (kind < GPSP_RV32_MEM_STORE_U8)
    return true;
  uint32_t displacement = ewram_region ? 0x40000u : (uint32_t)-0x8000;
  if (!emit_constant(code, A0, displacement) ||
      !gpsp_rv32_add(code, A0, A0, RV_TMP0) ||
      (kind == GPSP_RV32_MEM_STORE_U8 &&
       !gpsp_rv32_andi(code, A0, A0, -2)))
    return false;
  bool ok = kind == GPSP_RV32_MEM_STORE_U8 ?
      gpsp_rv32_lhu(code, A0, A0, 0) :
      kind == GPSP_RV32_MEM_STORE_U16 ?
      gpsp_rv32_lhu(code, A0, A0, 0) :
      gpsp_rv32_lw(code, A0, A0, 0);
  if (!ok)
    return false;
  *slow_site = code->ptr;
  return gpsp_rv32_bne(code, A0, X0, 0);
}

/* A load folds the low base bits into its offset; a store keeps the
   full address in RV_TMP0 for its code tag check. */
static bool emit_ram_path(gpsp_rv32_code *code, unsigned kind,
    bool ewram_region, unsigned reg, uint8_t **slow_site)
{
  uintptr_t base = ewram_region ? (uintptr_t)ewram :
      (uintptr_t)(iwram + 0x8000);
  bool store = kind >= GPSP_RV32_MEM_STORE_U8;
  int low = (int)(base & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = (((uint32_t)base + 0x800u) >> 12) & 0xfffff;
  if (base > UINT32_MAX ||
      !(ewram_region ? gpsp_rv32_slli(code, RV_TMP1, A1, 14) &&
                        gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, 14) :
                        gpsp_rv32_slli(code, RV_TMP1, A1, 17) &&
                        gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, 17)) ||
      !(store ? emit_constant(code, RV_TMP0, (uint32_t)base) :
                gpsp_rv32_lui(code, RV_TMP0, upper)) ||
      !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, RV_TMP1) ||
      !emit_code_tag_check(code, kind, ewram_region, slow_site) ||
      !emit_native_access(code, kind, reg, store ? 0 : low))
    return false;
  int cycles = ewram_region ?
      (kind == GPSP_RV32_MEM_LOAD_U32 ||
       kind == GPSP_RV32_MEM_STORE_U32 ? 6 : 3) : 1;
  return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -cycles);
}

/* Probes ROM through gpsp_rv32_rom_probe_*, linked in RV_TMP0. A hit returns
   to the jump that follows; a miss returns past it to the slow path. */
static bool emit_rom_probe(gpsp_rv32_code *code, unsigned kind)
{
  static void (*const rom_probes[5])(void) =
  {
    gpsp_rv32_rom_probe_u8, gpsp_rv32_rom_probe_u16,
    gpsp_rv32_rom_probe_u32, gpsp_rv32_rom_probe_s8,
    gpsp_rv32_rom_probe_s16,
  };
  uintptr_t target = (uintptr_t)rom_probes[kind];
  if (target > UINT32_MAX)
    return false;
  int low = (int)(target & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = (((uint32_t)target + 0x800u) >> 12) & 0xfffff;
  return gpsp_rv32_lui(code, RV_TMP1, upper) &&
      gpsp_rv32_jalr(code, RV_TMP0, RV_TMP1, low);
}

static uint64_t arm_memory_access(uint32_t *regs, uint32_t *cycles,
    uint32_t address, uint32_t value, gpsp_rv32_memory_kind kind)
{
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  uint32_t memory_start = arm_memory_cycles_now();
  gpsp_rv32_jit_arm_memory_calls++;
  unsigned region = address >> 24;
  if (region == 0x02 || region == 0x03)
    gpsp_rv32_jit_arm_memory_ram++;
  else if (region == 0x04)
    gpsp_rv32_jit_arm_memory_io++;
  else if (region >= 0x05 && region <= 0x07)
    gpsp_rv32_jit_arm_memory_video++;
  else if (region >= 0x08 && region <= 0x0d)
    gpsp_rv32_jit_arm_memory_rom++;
  else if (region == 0x0e)
    gpsp_rv32_jit_arm_memory_save++;
  else
    gpsp_rv32_jit_arm_memory_other++;
  if (kind >= GPSP_RV32_MEM_STORE_U8)
    gpsp_rv32_jit_arm_memory_stores++;
#endif
  bool store = kind >= GPSP_RV32_MEM_STORE_U8;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  bool rom_read = !store && region >= 8 && region <= 0x0d &&
      (address & 0x1ffffffu) < gamepak_size &&
      (region != 0x0d || backup_type != BACKUP_EEPROM);
  if (rom_read)
  {
    gpsp_rv32_jit_rom_read_body_calls++;
    if (!memory_map_read[address >> 15])
      gpsp_rv32_jit_rom_read_page_misses++;
  }
#endif
  bool word = kind == GPSP_RV32_MEM_LOAD_U32 ||
      kind == GPSP_RV32_MEM_STORE_U32;
  uint32_t bytes = word ? 4 :
      kind == GPSP_RV32_MEM_LOAD_U16 ||
      kind == GPSP_RV32_MEM_LOAD_S16 ||
      kind == GPSP_RV32_MEM_STORE_U16 ? 2 : 1;
  uint32_t pc = regs[REG_PC];
  uint32_t next_pc = pc + 4;
  uint32_t old_cpsr = regs[REG_CPSR];
  uint32_t old_mode = regs[CPU_MODE];
  uint32_t old_halt = regs[CPU_HALT_STATE];
  cpu_alert_type alert = CPU_ALERT_NONE;
  uint32_t result = 0;

  if (store)
    address &= ~(bytes - 1);
  if (address < 0x10000000u)
    *cycles -= ws_cyc_nseq[address >> 24][word ? 1 : 0];
  if (store && gpsp_rv32_store_changes_code(address, bytes, value))
    alert |= CPU_ALERT_SMC;

  uint32_t access_pc = next_pc;
  if (!store && (address >> 24) == 4 &&
      io_read_requires_handler(address))
    access_pc = pc;
  regs[REG_PC] = access_pc;

  switch (kind)
  {
    case GPSP_RV32_MEM_LOAD_U8:  result = read_memory8(address); break;
    case GPSP_RV32_MEM_LOAD_U16: result = (uint16_t)read_memory16(address); break;
    case GPSP_RV32_MEM_LOAD_U32: result = read_memory32(address); break;
    case GPSP_RV32_MEM_LOAD_S8:  result = read_memory8s(address); break;
    case GPSP_RV32_MEM_LOAD_S16: result = read_memory16s(address); break;
    case GPSP_RV32_MEM_STORE_U8: alert |= write_memory8(address, value); break;
    case GPSP_RV32_MEM_STORE_U16: alert |= write_memory16(address, value); break;
    case GPSP_RV32_MEM_STORE_U32: alert |= write_memory32(address, value); break;
    default: break;
  }

  if (alert || regs[REG_PC] != access_pc ||
      regs[REG_CPSR] != old_cpsr || regs[CPU_MODE] != old_mode ||
      regs[CPU_HALT_STATE] != old_halt)
  {
    if (regs[REG_PC] == access_pc)
      regs[REG_PC] = next_pc;
    *cycles -= ws_cyc_seq[(regs[REG_PC] >> 24) & 15][1];
    if (regs[REG_PC] == idle_loop_target_pc && (int32_t)*cycles > 0)
      *cycles = 0;
    gpsp_rv32_add_memory_alert(alert);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    arm_memory_finish_timing(memory_start, rom_read, region, kind, pc);
#endif
    return (uint64_t)result | GPSP_RV32_HELPER_EXIT;
  }

  regs[REG_PC] = pc;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  arm_memory_finish_timing(memory_start, rom_read, region, kind, pc);
#endif
  return result;
}

#define arm_memory_helper(name, kind)                                        \
static uint64_t name(uint32_t *regs, uint32_t *cycles,                       \
    uint32_t address, uint32_t value)                                         \
{                                                                            \
  return arm_memory_access(regs, cycles, address, value, kind);               \
}

arm_memory_helper(arm_load_u8, GPSP_RV32_MEM_LOAD_U8)
arm_memory_helper(arm_load_u16, GPSP_RV32_MEM_LOAD_U16)
arm_memory_helper(arm_load_u32, GPSP_RV32_MEM_LOAD_U32)
arm_memory_helper(arm_load_s8, GPSP_RV32_MEM_LOAD_S8)
arm_memory_helper(arm_load_s16, GPSP_RV32_MEM_LOAD_S16)
arm_memory_helper(arm_store_u8, GPSP_RV32_MEM_STORE_U8)
arm_memory_helper(arm_store_u16, GPSP_RV32_MEM_STORE_U16)
arm_memory_helper(arm_store_u32, GPSP_RV32_MEM_STORE_U32)

static const gpsp_rv32_helper_fn arm_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] =
{
  arm_load_u8, arm_load_u16, arm_load_u32, arm_load_s8, arm_load_s16,
  arm_store_u8, arm_store_u16, arm_store_u32,
};

static bool emit_fast_call(gpsp_rv32_code *code, uintptr_t target_address)
{
  uint32_t target = (uint32_t)target_address;
  int low = (int)(target & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((target + 0x800u) >> 12) & 0xfffff;
  return gpsp_rv32_lui(code, RV_TMP0, upper) &&
      gpsp_rv32_jalr(code, A0, RV_TMP0, low);
}

/* Lazy PC: region code keeps RV_PC at its head. A helper call needs the
   instruction's PC, so only slow paths add this delta around the call. */
static int32_t lazy_pc_delta;
/* A region charges fetches up front; an exit returns the unrun share. */
static int32_t lazy_exit_refund;

static bool emit_pc_sync(gpsp_rv32_code *code, int32_t delta)
{
  return !delta || gpsp_rv32_addi(code, RV_PC, RV_PC, delta);
}

/* An always-exiting helper owns its fetch and idle check. Refund first. */
static bool emit_block_sync(gpsp_rv32_code *code)
{
  return emit_pc_sync(code, lazy_pc_delta) &&
      (!lazy_exit_refund ||
       gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, lazy_exit_refund));
}

static bool emit_helper_exit(gpsp_rv32_code *code);

/* The helper already zeroed an idle target's budget; the refund must not
   undo that, so repeat its idle test after refunding. RV_PC is the next PC. */
static bool emit_exit(gpsp_rv32_code *code)
{
  if (!lazy_exit_refund)
    return gpsp_rv32_jalr(code, X0, RA, 0);
  bool idle = idle_loop_target_pc != 0xFFFFFFFFu;
  if (!gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, lazy_exit_refund))
    return false;
  if (idle)
  {
    if (!emit_constant(code, RV_TMP0, idle_loop_target_pc))
      return false;
    uint8_t *not_idle = code->ptr;
    if (!gpsp_rv32_bne(code, RV_PC, RV_TMP0, 0))
      return false;
    uint8_t *spent = code->ptr;
    if (!gpsp_rv32_bge(code, X0, RV_BUDGET, 0) ||
        !gpsp_rv32_addi(code, RV_BUDGET, X0, 0) ||
        !gpsp_rv32_patch_branch(code, spent, code->ptr) ||
        !gpsp_rv32_patch_branch(code, not_idle, code->ptr))
      return false;
  }
  return gpsp_rv32_jalr(code, X0, RA, 0);
}

/* Continue when the helper reports no exit in A1. */
static bool emit_helper_exit(gpsp_rv32_code *code)
{
  uint8_t *skip = code->ptr;
  return gpsp_rv32_beq(code, A1, X0, 0) && emit_exit(code) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

/* RV_TMP1 is a 17-bit VRAM offset. Bit 16 and bit 15 set: clear bit 15. */
static bool emit_vram_mirror(gpsp_rv32_code *code)
{
  if (!gpsp_rv32_srli(code, RV_TMP0, RV_TMP1, 16) ||
      !gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, 15))
    return false;
#if GPSP_RV32_USE_ZBB
  return gpsp_rv32_andn(code, RV_TMP1, RV_TMP1, RV_TMP0);
#else
  return gpsp_rv32_and(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_sub(code, RV_TMP1, RV_TMP1, RV_TMP0);
#endif
}

/* VRAM stores as write_memory*: mirror 0x18000-0x1ffff down by 0x8000,
   and a byte goes to both halves of its halfword. VRAM holds no code
   tags and raises no alerts. RV_TMP0 holds the region minus 2 and stays so
   on the other paths. */
static bool emit_vram_store(gpsp_rv32_code *code, unsigned kind,
    unsigned value, uint8_t **done)
{
  uintptr_t base = (uintptr_t)vram;
  if (base > UINT32_MAX || (base & 3))
    return true;
  int low = (int)(base & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = (((uint32_t)base + 0x800u) >> 12) & 0xfffff;
  bool byte = kind == GPSP_RV32_MEM_STORE_U8;
  if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -4))
    return false;
  uint8_t *skip = code->ptr;
  if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0) ||
      (byte && (!gpsp_rv32_andi(code, A0, value, 0xff) ||
                !gpsp_rv32_slli(code, RV_TMP0, A0, 8) ||
                !gpsp_rv32_or(code, A0, A0, RV_TMP0))) ||
      !gpsp_rv32_slli(code, RV_TMP1, A1, 15) ||
      !gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, 15) ||
      (byte && !gpsp_rv32_andi(code, RV_TMP1, RV_TMP1, -2)) ||
      !emit_vram_mirror(code) ||
      !gpsp_rv32_lui(code, RV_TMP0, upper) ||
      !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, RV_TMP1) ||
      !emit_native_access(code,
          byte ? GPSP_RV32_MEM_STORE_U16 : kind, byte ? A0 : value, low) ||
      !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET,
          -(int)ws_cyc_nseq[6][kind == GPSP_RV32_MEM_STORE_U32]))
    return false;
  *done = code->ptr;
  return gpsp_rv32_jal(code, X0, 0) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_single(gpsp_rv32_code *code,
    const arm_single_transfer *decoded, uint32_t pc)
{
  bool store = decoded->kind >= GPSP_RV32_MEM_STORE_U8;
  unsigned bytes = decoded->kind == GPSP_RV32_MEM_LOAD_U32 ||
      decoded->kind == GPSP_RV32_MEM_STORE_U32 ? 4 :
      decoded->kind == GPSP_RV32_MEM_LOAD_U16 ||
      decoded->kind == GPSP_RV32_MEM_LOAD_S16 ||
      decoded->kind == GPSP_RV32_MEM_STORE_U16 ? 2 : 1;
  int displacement = decoded->up ? (int)decoded->offset :
      -(int)decoded->offset;
  bool fold_immediate = !decoded->register_offset &&
      (decoded->rn == 15 || (displacement >= -2048 &&
          displacement <= 2047));
  /* RAM paths store from the guest register unless writeback replaces it
     first; other paths take the value in A2. */
  bool copy_value = store && (decoded->rd == 15 ||
      (decoded->writeback && decoded->rd == decoded->rn));
  unsigned value = !store ? 0 : copy_value ? A2 :
      guest_reg_map[decoded->rd];
  /* RAM paths load into the guest register; other paths return A0. */
  unsigned target = store || decoded->rd == 15 ? A0 :
      guest_reg_map[decoded->rd];
  if (fold_immediate)
  {
    if (decoded->pre)
    {
      if (!(decoded->rn == 15 ? emit_constant(code, A1,
                pc + 8 + (uint32_t)displacement) :
              gpsp_rv32_addi(code, A1, guest_reg_map[decoded->rn],
                  displacement)) ||
          (copy_value && !emit_guest_read(code, A2, decoded->rd, pc, 12)) ||
          (decoded->writeback && displacement &&
           !gpsp_rv32_addi(code, guest_reg_map[decoded->rn], A1, 0)))
        return false;
    }
    else if (!emit_guest_read(code, A1, decoded->rn, pc, 8) ||
        (copy_value && !emit_guest_read(code, A2, decoded->rd, pc, 12)) ||
        (displacement && !gpsp_rv32_addi(code,
            guest_reg_map[decoded->rn], guest_reg_map[decoded->rn],
            displacement)))
      return false;
  }
  else
  {
    /* LSL #0 uses Rm as is; other shifts and large immediates go to RV_TMP0. */
    unsigned offset = RV_TMP0;
    unsigned scale = 0;
#if GPSP_RV32_USE_ZBA
    /* An added Rm, LSL #1-3 folds the shift into the add. */
    if (decoded->register_offset && decoded->up && !decoded->shift_type &&
        decoded->shift <= 3 && decoded->rm != 15)
      scale = decoded->shift;
#endif
    if (scale ||
        (decoded->register_offset && !decoded->shift_type && !decoded->shift))
      offset = guest_reg_map[decoded->rm];
    else if (!emit_offset(code, decoded, pc))
      return false;
    unsigned base = A1;
    if (decoded->rn != 15)
      base = guest_reg_map[decoded->rn];
    else if (!emit_guest_read(code, A1, 15, pc, 8))
      return false;
    if (!(decoded->pre ?
            (scale ? gpsp_rv32_shadd(code, scale, A1, offset, base) :
             decoded->up ? gpsp_rv32_add(code, A1, base, offset) :
                           gpsp_rv32_sub(code, A1, base, offset)) :
            (base == A1 || gpsp_rv32_addi(code, A1, base, 0))) ||
        (copy_value && !emit_guest_read(code, A2, decoded->rd, pc, 12)))
      return false;
    if (decoded->writeback)
    {
      unsigned rn = guest_reg_map[decoded->rn];
      if (!(decoded->pre ? gpsp_rv32_addi(code, rn, A1, 0) :
              scale ? gpsp_rv32_shadd(code, scale, rn, offset, rn) :
              decoded->up ? gpsp_rv32_add(code, rn, rn, offset) :
                            gpsp_rv32_sub(code, rn, rn, offset)))
        return false;
    }
  }
  if (store && bytes != 1 && !gpsp_rv32_andi(code, A1, A1, -(int)bytes))
    return false;

  uint8_t *align_slow = NULL;
  if (!store && bytes != 1)
  {
    if (!gpsp_rv32_andi(code, RV_TMP1, A1, bytes - 1))
      return false;
    align_slow = code->ptr;
    if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0))
      return false;
  }

  /* IWRAM falls through; EWRAM and the rest follow it. RV_TMP0 holds the
     region minus 2 on the paths after EWRAM. */
  uint8_t *iwram_skip = NULL;
  if (!gpsp_rv32_srli(code, RV_TMP0, A1, 24) ||
      !gpsp_rv32_addi(code, RV_TMP0, RV_TMP0, -3))
    return false;
  iwram_skip = code->ptr;
  if (!gpsp_rv32_bne(code, RV_TMP0, X0, 0))
    return false;
  uint8_t *iwram_tag = NULL;
  if (!emit_ram_path(code, decoded->kind, false, store ? value : target,
          &iwram_tag))
    return false;
  uint8_t *iwram_done = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0) ||
      !gpsp_rv32_patch_branch(code, iwram_skip, code->ptr) ||
      !gpsp_rv32_addi(code, RV_TMP0, RV_TMP0, 1))
    return false;
  uint8_t *ewram_skip = code->ptr;
  if (!gpsp_rv32_bne(code, RV_TMP0, X0, 0))
    return false;
  uint8_t *ewram_tag = NULL;
  if (!emit_ram_path(code, decoded->kind, true, store ? value : target,
          &ewram_tag))
    return false;
  uint8_t *ewram_done = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0) ||
      !gpsp_rv32_patch_branch(code, ewram_skip, code->ptr))
    return false;
  uint8_t *vram_done = NULL;
  if (store && !emit_vram_store(code, decoded->kind, value, &vram_done))
    return false;
  if (store && value != A2 && !gpsp_rv32_addi(code, A2, value, 0))
    return false;


  /* I/O: interrupt registers and plain backing-store registers. */
  uint8_t *io_slow = NULL;
  uint8_t *io_done = NULL;
  uintptr_t io_helper =
      decoded->kind == GPSP_RV32_MEM_LOAD_U16 ?
          (uintptr_t)gpsp_rv32_arm_io_load16 :
      decoded->kind == GPSP_RV32_MEM_LOAD_U32 ?
          (uintptr_t)gpsp_rv32_arm_io_load32 :
      decoded->kind == GPSP_RV32_MEM_STORE_U16 ?
          (uintptr_t)gpsp_rv32_arm_io_store16 : 0;
  if (io_helper && io_helper <= UINT32_MAX &&
      (uintptr_t)io_registers <= UINT32_MAX)
  {
    if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -2))
      return false;
    uint8_t *io_skip = code->ptr;
    if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0) ||
        !emit_fast_call(code, io_helper))
      return false;
    /* A finished access returns past this jump. */
    io_slow = code->ptr;
    if (!gpsp_rv32_fixed_begin(code) || !gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_fixed_end(code))
      return false;
    io_done = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_patch_branch(code, io_skip, code->ptr))
      return false;
  }

  uint8_t *rom_done = NULL;
  if (!store)
  {
    if (!emit_rom_probe(code, decoded->kind))
      return false;
    /* A ROM miss returns past this jump. */
    rom_done = code->ptr;
    if (!gpsp_rv32_fixed_begin(code) || !gpsp_rv32_jal(code, X0, 0) ||
        !gpsp_rv32_fixed_end(code))
      return false;
  }

  uint8_t *palette_slow = NULL;
  uint8_t *palette_done = NULL;
  bool palette_compact = (uintptr_t)palette_ram >=
      (uintptr_t)palette_ram_converted &&
      (uintptr_t)palette_ram - (uintptr_t)palette_ram_converted == 0x400 &&
      (uintptr_t)gpsp_rv32_arm_palette_store32_compact <= UINT32_MAX;
  uintptr_t palette_helper = palette_compact ?
      (uintptr_t)gpsp_rv32_arm_palette_store32_compact :
      (uintptr_t)gpsp_rv32_arm_palette_store32;
  bool palette_fast = decoded->kind == GPSP_RV32_MEM_STORE_U32 &&
      palette_helper <= UINT32_MAX &&
      (uintptr_t)palette_ram <= UINT32_MAX &&
      (uintptr_t)palette_ram_converted <= UINT32_MAX &&
      ((uintptr_t)palette_ram & 3) == 0 &&
      ((uintptr_t)palette_ram_converted & 3) == 0;
  if (palette_fast)
  {
    if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -3))
      return false;
    palette_slow = code->ptr;
    if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0) ||
        !emit_fast_call(code, palette_helper))
      return false;
    palette_done = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0))
      return false;
  }

  uint8_t *oam_region = code->ptr;
  uint8_t *oam_slow = NULL;
  uint8_t *oam_done = NULL;
  bool oam_fast = decoded->kind == GPSP_RV32_MEM_STORE_U32 &&
      (uintptr_t)oam_ram <= UINT32_MAX &&
      ((uintptr_t)oam_ram & 3) == 0;
  if (oam_fast)
  {
    if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -5))
      return false;
    oam_slow = code->ptr;
    /* OAM mirrors every 1 KiB and marks sprite state dirty. */
    if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0) ||
        !gpsp_rv32_andi(code, RV_TMP1, A1, 0x3ff) ||
        !emit_constant(code, RV_TMP0, (uint32_t)(uintptr_t)oam_ram) ||
        !gpsp_rv32_add(code, RV_TMP0, RV_TMP0, RV_TMP1) ||
        !gpsp_rv32_addi(code, RV_TMP1, X0, 1) ||
        !gpsp_rv32_sw(code, RV_TMP1, RV_STATE, OAM_UPDATED * 4) ||
        !gpsp_rv32_sw(code, A2, RV_TMP0, 0) ||
        !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -2))
      return false;
    oam_done = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0))
      return false;
  }

  uint8_t *slow = code->ptr;
  if (store && value != A2 && !gpsp_rv32_addi(code, A2, value, 0))
    return false;
  uintptr_t callback = (uintptr_t)arm_memory_helpers[decoded->kind];
  uintptr_t bridge = (uintptr_t)gpsp_rv32_call_helper;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  if (!store)
    bridge = (uintptr_t)gpsp_rv32_call_memory_read_helper;
#endif
  if (callback > UINT32_MAX ||
      !emit_pc_sync(code, lazy_pc_delta) ||
      !emit_constant(code, A0, (uint32_t)callback) ||
      !emit_call(code, bridge) ||
      !emit_helper_exit(code) ||
      !emit_pc_sync(code, -lazy_pc_delta))
    return false;

  /* Non-RAM loads return A0. */
  uint8_t *join = code->ptr;
  if (target != A0 && !gpsp_rv32_addi(code, target, A0, 0))
    return false;
  uint8_t *done = code->ptr;
  if ((palette_slow && !gpsp_rv32_patch_branch(code, palette_slow,
          oam_region)) ||
      (palette_done && !gpsp_rv32_patch_branch(code, palette_done,
          join)) ||
      (io_slow && !gpsp_rv32_patch_branch(code, io_slow, slow)) ||
      (io_done && !gpsp_rv32_patch_branch(code, io_done, join)) ||
      (oam_slow && !gpsp_rv32_patch_branch(code, oam_slow, slow)) ||
      (oam_done && !gpsp_rv32_patch_branch(code, oam_done, join)) ||
      (align_slow && !gpsp_rv32_patch_branch(code, align_slow, slow)) ||
      (rom_done && !gpsp_rv32_patch_branch(code, rom_done, join)) ||
      (ewram_tag && !gpsp_rv32_patch_branch(code, ewram_tag, slow)) ||
      (iwram_tag && !gpsp_rv32_patch_branch(code, iwram_tag, slow)) ||
      !gpsp_rv32_patch_branch(code, ewram_done, done) ||
      (vram_done && !gpsp_rv32_patch_branch(code, vram_done, done)) ||
      !gpsp_rv32_patch_branch(code, iwram_done, done))
    return false;
  return true;
}

typedef struct
{
  unsigned rn;
  unsigned count;
  uint16_t reg_list;
  bool load;
  bool user_bank;
  bool restore_cpsr;
  bool up;
  bool pre;
  bool writeback;
  bool pc_exit;
  bool base_first_store;
  int start_offset;
  int end_offset;
} arm_block_transfer;

static bool decode_block(uint32_t opcode, arm_block_transfer *decoded)
{
  if ((opcode & 0x0e000000) != 0x08000000)
    return false;
  decoded->rn = (opcode >> 16) & 15;
  decoded->reg_list = opcode & 0xffff;
  /* ARM7TDMI excludes PC bases and empty register lists. */
  if (decoded->rn == 15 || !decoded->reg_list)
    return false;
  decoded->count = __builtin_popcount(decoded->reg_list);
  decoded->load = (opcode & 0x00100000) != 0;
  decoded->restore_cpsr = (opcode & 0x00400000) &&
      decoded->load && (decoded->reg_list & 0x8000);
  decoded->user_bank = (opcode & 0x00400000) &&
      (!decoded->load || !(decoded->reg_list & 0x8000));
  decoded->up = (opcode & 0x00800000) != 0;
  decoded->pre = (opcode & 0x01000000) != 0;
  decoded->writeback = (opcode & 0x00200000) != 0;
  decoded->pc_exit = decoded->load && (decoded->reg_list & 0x8000);
  decoded->base_first_store = !decoded->load && decoded->writeback &&
      (decoded->reg_list & (1u << decoded->rn)) &&
      !(decoded->reg_list & ((1u << decoded->rn) - 1));
  decoded->end_offset = (decoded->up ? 4 : -4) * (int)decoded->count;
  decoded->start_offset = decoded->up ? (decoded->pre ? 4 : 0) :
      decoded->end_offset + (decoded->pre ? 0 : 4);
  return true;
}

static u32 *block_reg_ptr(uint32_t *regs, unsigned index,
    bool user_bank)
{
  if (user_bank &&
      ((index >= 8 && index <= 12 && regs[CPU_MODE] == MODE_FIQ) ||
       (index >= REG_SP && index <= REG_LR &&
        regs[CPU_MODE] != MODE_USER && regs[CPU_MODE] != MODE_SYSTEM)))
    return &REG_MODE(MODE_USER)[index - 8];
  return &((u32 *)(void *)regs)[index];
}

static uint64_t arm_block_access(uint32_t *regs, uint32_t *cycles,
    uint32_t opcode, uint32_t unused)
{
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  uint32_t block_start = arm_memory_cycles_now();
  gpsp_rv32_jit_arm_block_calls++;
#endif
  (void)unused;
  arm_block_transfer decoded = {0};
  if (!decode_block(opcode, &decoded))
  {
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    gpsp_rv32_jit_arm_block_cycles +=
        (uint32_t)(arm_memory_cycles_now() - block_start);
#endif
    return GPSP_RV32_HELPER_EXIT;
  }

  uint32_t pc = regs[REG_PC];
  uint32_t old_cpsr = regs[REG_CPSR];
  uint32_t old_mode = regs[CPU_MODE];
  uint32_t old_halt = regs[CPU_HALT_STATE];
  uint32_t base = regs[decoded.rn];
  uint32_t end = base + decoded.end_offset;
  uint32_t address = (base + decoded.start_offset) & ~3u;
  cpu_alert_type alert = CPU_ALERT_NONE;
  bool skip_fetch = false;

  if (decoded.writeback && !decoded.base_first_store)
    regs[decoded.rn] = end;
  regs[REG_PC] = pc + 4;
  for (unsigned i = 0; i < 16; i++)
  {
    if (!(decoded.reg_list & (1u << i)))
      continue;
    if (address < 0x10000000u)
      *cycles -= ws_cyc_seq[address >> 24][1];
    if (decoded.load)
    {
      bool io_handler = (address >> 24) == 4 &&
          io_read_requires_handler(address);
      uint32_t prior_pc = regs[REG_PC];
      if (io_handler)
        regs[REG_PC] = prior_pc - 4;
      uint32_t value = read_memory32(address);
      if (io_handler)
        regs[REG_PC] = prior_pc;
      *block_reg_ptr(regs, i, decoded.user_bank) = value;
    }
    else
    {
      uint32_t value = i == REG_PC ? pc + 12 :
          *block_reg_ptr(regs, i, decoded.user_bank);
      if (gpsp_rv32_store_changes_code(address, 4, value))
        alert |= CPU_ALERT_SMC;
      alert |= write_memory32(address, value);
    }
    address += 4;
  }
  if (decoded.base_first_store)
    regs[decoded.rn] = end;

  if (decoded.restore_cpsr && regs[CPU_MODE] != MODE_USER &&
      regs[CPU_MODE] != MODE_SYSTEM)
  {
    regs[REG_CPSR] = REG_SPSR(regs[CPU_MODE]);
    set_cpu_mode(cpu_modes[regs[REG_CPSR] & 15]);
    if ((read_ioreg(REG_IE) & read_ioreg(REG_IF)) &&
        read_ioreg(REG_IME) && !(regs[REG_CPSR] & 0x80))
    {
      REG_MODE(MODE_IRQ)[6] = regs[REG_PC] + 4;
      REG_SPSR(MODE_IRQ) = regs[REG_CPSR];
      regs[REG_CPSR] = 0xd2;
      regs[REG_PC] = 0x18;
      set_cpu_mode(MODE_IRQ);
      skip_fetch = true;
    }
    else if (regs[REG_CPSR] & 0x20)
      skip_fetch = true;
  }

  /* Exception and Thumb returns enter another interpreter loop before fetch. */
  if (!skip_fetch)
    *cycles -= ws_cyc_seq[(regs[REG_PC] >> 24) & 15][1];
  if (regs[REG_PC] == idle_loop_target_pc && (int32_t)*cycles > 0)
    *cycles = 0;
  if (alert || regs[REG_CPSR] != old_cpsr ||
      regs[CPU_MODE] != old_mode || regs[CPU_HALT_STATE] != old_halt)
    gpsp_rv32_add_memory_alert(alert);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  gpsp_rv32_jit_arm_block_cycles +=
      (uint32_t)(arm_memory_cycles_now() - block_start);
#endif
  return GPSP_RV32_HELPER_EXIT;
}

static bool emit_block_region(gpsp_rv32_code *code,
    const arm_block_transfer *decoded, bool ewram_region,
    uint8_t *slow_sites[24], unsigned *slow_count)
{
  unsigned low_shift = ewram_region ? 14 : 17;
  uint32_t region_size = ewram_region ? 0x40000 : 0x8000;
  uintptr_t data_base = ewram_region ? (uintptr_t)ewram :
      (uintptr_t)(iwram + 0x8000);
  if (data_base > UINT32_MAX ||
      !gpsp_rv32_slli(code, RV_TMP1, A1, low_shift) ||
      !gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, low_shift) ||
      !emit_constant(code, RV_TMP0, region_size - decoded->count * 4 + 1))
    return false;
  slow_sites[(*slow_count)++] = code->ptr;
  if (!gpsp_rv32_bgeu(code, RV_TMP1, RV_TMP0, 0) ||
      !emit_constant(code, A0, (uint32_t)data_base) ||
      !gpsp_rv32_add(code, A0, A0, RV_TMP1) ||
      !emit_constant(code, A2, decoded->count * (ewram_region ? 6 : 1)) ||
      (!decoded->load && !emit_constant(code, RV_TMP0,
          ewram_region ? 0x40000 : (uint32_t)-0x8000)))
    return false;
  return true;
}

/* STM to VRAM inside one 32 KiB run, so no word crosses the mirror at
   0x18000 or the wrap at 0x20000; other runs take the slow path. Same
   order as the RAM path; RV_TMP0 holds the region minus 2. */
static bool emit_block_vram_store(gpsp_rv32_code *code,
    const arm_block_transfer *decoded, uint32_t pc,
    uint8_t *slow_sites[24], unsigned *slow_count, uint8_t **done)
{
  uintptr_t base = (uintptr_t)vram;
  if (base > UINT32_MAX || (base & 3))
  {
    slow_sites[(*slow_count)++] = code->ptr;
    return gpsp_rv32_jal(code, X0, 0);
  }
  if (!gpsp_rv32_addi(code, RV_TMP1, RV_TMP0, -4))
    return false;
  slow_sites[(*slow_count)++] = code->ptr;
  if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0) ||
      !gpsp_rv32_slli(code, RV_TMP1, A1, 17) ||
      !gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, 17) ||
      !emit_constant(code, RV_TMP0, 0x8000 - decoded->count * 4 + 1))
    return false;
  slow_sites[(*slow_count)++] = code->ptr;
  if (!gpsp_rv32_bgeu(code, RV_TMP1, RV_TMP0, 0) ||
      !gpsp_rv32_slli(code, RV_TMP1, A1, 15) ||
      !gpsp_rv32_srli(code, RV_TMP1, RV_TMP1, 15) ||
      !emit_vram_mirror(code) ||
      !emit_constant(code, A0, (uint32_t)base) ||
      !gpsp_rv32_add(code, A0, A0, RV_TMP1))
    return false;

  if (decoded->writeback && !decoded->base_first_store &&
      !gpsp_rv32_addi(code, guest_reg_map[decoded->rn],
          guest_reg_map[decoded->rn], decoded->end_offset))
    return false;
  unsigned offset = 0;
  for (unsigned i = 0; i < 16; i++)
  {
    if (!(decoded->reg_list & (1u << i)))
      continue;
    if (i == 15)
    {
      if (!emit_constant(code, RV_TMP1, pc + 12) ||
          !gpsp_rv32_sw(code, RV_TMP1, A0, offset))
        return false;
    }
    else if (!gpsp_rv32_sw(code, guest_reg_map[i], A0, offset))
      return false;
    offset += 4;
  }
  if ((decoded->base_first_store &&
       !gpsp_rv32_addi(code, guest_reg_map[decoded->rn],
           guest_reg_map[decoded->rn], decoded->end_offset)) ||
      !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET,
          -(int)(decoded->count * ws_cyc_seq[6][1])))
    return false;
  *done = code->ptr;
  return gpsp_rv32_jal(code, X0, 0);
}

static bool emit_block(gpsp_rv32_code *code,
    const arm_block_transfer *decoded, uint32_t opcode, uint32_t pc)
{
  if ((uintptr_t)arm_block_access > UINT32_MAX)
    return false;
  if (decoded->user_bank || decoded->restore_cpsr)
    return emit_block_sync(code) &&
        emit_constant(code, A1, opcode) &&
        gpsp_rv32_addi(code, A2, X0, 0) &&
        emit_constant(code, A0, (uint32_t)(uintptr_t)arm_block_access) &&
        emit_call(code, (uintptr_t)gpsp_rv32_call_helper) &&
        gpsp_rv32_jalr(code, X0, RA, 0);

  uint8_t *slow_sites[24];
  unsigned slow_count = 0;
  if (!gpsp_rv32_addi(code, A1, guest_reg_map[decoded->rn],
          decoded->start_offset) ||
      !gpsp_rv32_andi(code, A1, A1, -4) ||
      !gpsp_rv32_srli(code, RV_TMP0, A1, 24) ||
      !gpsp_rv32_addi(code, RV_TMP0, RV_TMP0, -2) ||
      !gpsp_rv32_sltiu(code, RV_TMP1, RV_TMP0, 2))
    return false;
  /* Stores try VRAM before the slow path. */
  uint8_t *other_site = code->ptr;
  if (decoded->load)
    slow_sites[slow_count++] = other_site;
  if (!gpsp_rv32_beq(code, RV_TMP1, X0, 0))
    return false;

  uint8_t *ewram_site = code->ptr;
  if (!gpsp_rv32_beq(code, RV_TMP0, X0, 0) ||
      !emit_block_region(code, decoded, false, slow_sites, &slow_count))
    return false;
  uint8_t *common_site = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0) ||
      !gpsp_rv32_patch_branch(code, ewram_site, code->ptr) ||
      !emit_block_region(code, decoded, true, slow_sites, &slow_count) ||
      !gpsp_rv32_patch_branch(code, common_site, code->ptr))
    return false;

  if (!decoded->load)
  {
    if (!gpsp_rv32_add(code, RV_TMP0, A0, RV_TMP0))
      return false;
    for (unsigned i = 0; i < decoded->count; i++)
    {
      if (!gpsp_rv32_lw(code, RV_TMP1, RV_TMP0, i * 4))
        return false;
      slow_sites[slow_count++] = code->ptr;
      if (!gpsp_rv32_bne(code, RV_TMP1, X0, 0))
        return false;
    }
  }

  if (decoded->writeback && !decoded->base_first_store &&
      !gpsp_rv32_addi(code, guest_reg_map[decoded->rn],
          guest_reg_map[decoded->rn], decoded->end_offset))
    return false;

  unsigned offset = 0;
  for (unsigned i = 0; i < 16; i++)
  {
    if (!(decoded->reg_list & (1u << i)))
      continue;
    if (decoded->load)
    {
      if (!(i == 15 ? gpsp_rv32_lw(code, A0, A0, offset) :
                       gpsp_rv32_lw(code, guest_reg_map[i], A0, offset)))
        return false;
    }
    else if (i == 15)
    {
      if (!emit_constant(code, RV_TMP1, pc + 12) ||
          !gpsp_rv32_sw(code, RV_TMP1, A0, offset))
        return false;
    }
    else if (!gpsp_rv32_sw(code, guest_reg_map[i], A0, offset))
      return false;
    offset += 4;
  }
  if ((decoded->base_first_store &&
       !gpsp_rv32_addi(code, guest_reg_map[decoded->rn],
           guest_reg_map[decoded->rn], decoded->end_offset)) ||
      !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, A2))
    return false;
  uint8_t *fast_done = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0))
    return false;

  uint8_t *vram_done = NULL;
  if (!decoded->load)
  {
    if (!gpsp_rv32_patch_branch(code, other_site, code->ptr) ||
        !emit_block_vram_store(code, decoded, pc, slow_sites, &slow_count,
            &vram_done))
      return false;
  }

  uint8_t *slow = code->ptr;
  if (!emit_block_sync(code) ||
      !emit_constant(code, A1, opcode) ||
      !gpsp_rv32_addi(code, A2, X0, 0) ||
      !emit_constant(code, A0, (uint32_t)(uintptr_t)arm_block_access) ||
      !emit_call(code, (uintptr_t)gpsp_rv32_call_helper) ||
      !gpsp_rv32_jalr(code, X0, RA, 0))
    return false;
  if (!gpsp_rv32_patch_branch(code, fast_done, code->ptr) ||
      (vram_done && !gpsp_rv32_patch_branch(code, vram_done, code->ptr)))
    return false;
  for (unsigned i = 0; i < slow_count; i++)
    if (!gpsp_rv32_patch_branch(code, slow_sites[i], slow))
      return false;
  return true;
}

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory_lazy(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc,
    int32_t pc_delta, int32_t exit_refund)
{
  lazy_pc_delta = pc_delta;
  lazy_exit_refund = exit_refund;
  gpsp_rv32_arm_memory_result result =
      gpsp_rv32_emit_arm_memory(code, opcode, instruction_pc);
  lazy_pc_delta = 0;
  lazy_exit_refund = 0;
  return result;
}

#if defined(GPSP_RV32_TEST_HOOKS)
bool gpsp_rv32_test_arm_short_branch = true;
size_t gpsp_rv32_test_arm_staging_bytes;
#define ARM_SHORT_BRANCH gpsp_rv32_test_arm_short_branch
#define ARM_STAGING_BYTES(size) (gpsp_rv32_test_arm_staging_bytes && \
    gpsp_rv32_test_arm_staging_bytes < (size) ? \
    gpsp_rv32_test_arm_staging_bytes : (size))
#else
#define ARM_SHORT_BRANCH true
#define ARM_STAGING_BYTES(size) (size)
#endif

#if defined(GPSP_BENCHMARK) || defined(GPSP_RV32_TEST_HOOKS)
gpsp_rv32_arm_short_branch_stats gpsp_rv32_arm_short_branch;
#define short_branch_count(field) (gpsp_rv32_arm_short_branch.field++)
#else
#define short_branch_count(field) ((void)0)
#endif

#if defined(GPSP_BENCHMARK)

/* Totals since boot, printed once when a benchmark ends. */
void gpsp_rv32_arm_short_branch_report(void)
{
  printf("BENCH_JIT_SHORT_BRANCH version=1 attempts=%u retries=%u "
      "retry_failures=%u\n", (unsigned)gpsp_rv32_arm_short_branch.attempts,
      (unsigned)gpsp_rv32_arm_short_branch.retries,
      (unsigned)gpsp_rv32_arm_short_branch.retry_failures);
}
#endif

/* One ARM transfer, with its condition skip, into a staging buffer. */
static bool emit_transfer(gpsp_rv32_code *staged, uint32_t opcode,
    uint32_t instruction_pc, bool single, const arm_single_transfer *decoded,
    const arm_block_transfer *block, bool pc_exit)
{
  gpsp_rv32_arm_condition_skip skip = { { NULL, NULL }, 0 };
  return !((pc_exit && !gpsp_rv32_addi(staged, A1, X0, 0)) ||
      ((opcode >> 28) != 14 &&
       !gpsp_rv32_emit_arm_condition_skip(staged, opcode >> 28, &skip)) ||
      !(single ? emit_single(staged, decoded, instruction_pc) :
                 emit_block(staged, block, opcode, instruction_pc)) ||
      (pc_exit && !gpsp_rv32_addi(staged, A1, X0, 1)) ||
      !gpsp_rv32_patch_arm_condition_skip(staged, &skip, staged->ptr));
}

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc)
{
  if (!code || !code->start || !code->ptr || !code->end ||
      ((uintptr_t)code->ptr & 1) ||
      (uintptr_t)code->ptr < (uintptr_t)code->start ||
      (uintptr_t)code->ptr > (uintptr_t)code->end)
    return GPSP_RV32_ARM_MEMORY_BUFFER_FULL;
  if ((instruction_pc & 3) || (opcode >> 28) == 15)
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;

  arm_single_transfer decoded = {0};
  arm_block_transfer block = {0};
  bool single = decode_single(opcode, &decoded);
  if (!single && !decode_block(opcode, &block))
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;

  uint32_t words[256];
  gpsp_rv32_code staged;
  bool pc_exit = single ?
      decoded.kind < GPSP_RV32_MEM_STORE_U8 && decoded.rd == 15 :
      block.pc_exit;
  /* Short branches first. A target beyond their reach emits the instruction
     again with full-length branches; other compressed forms stay. */
  gpsp_rv32_code_child(&staged, code, words, ARM_STAGING_BYTES(sizeof(words)));
  staged.short_branch = ARM_SHORT_BRANCH;
  bool emitted = emit_transfer(&staged, opcode, instruction_pc, single,
      &decoded, &block, pc_exit);
  bool retried = !emitted && staged.branch_overflow;
  short_branch_count(attempts);
  if (retried)
  {
    short_branch_count(retries);
    gpsp_rv32_code_child(&staged, code, words,
        ARM_STAGING_BYTES(sizeof(words)));
    staged.short_branch = false;
    emitted = emit_transfer(&staged, opcode, instruction_pc, single,
        &decoded, &block, pc_exit);
  }

  size_t size = (size_t)(staged.ptr - staged.start);
  if (!emitted || (uintptr_t)code->end - (uintptr_t)code->ptr < size)
  {
    if (retried)
      short_branch_count(retry_failures);
    return GPSP_RV32_ARM_MEMORY_BUFFER_FULL;
  }
  memcpy(code->ptr, words, size);
  code->ptr += size;
  return pc_exit ? GPSP_RV32_ARM_MEMORY_PC_EXIT :
      GPSP_RV32_ARM_MEMORY_EMITTED;
}

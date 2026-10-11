/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../common.h"
#include "rv32_thumb_memory.h"

#include <string.h>

/* Nonzero asks native budget updates to return to the dispatcher. */
uint32_t gpsp_rv32_pending_alert;

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
extern uint64_t gpsp_rv32_jit_memory_cycles;
extern uint64_t gpsp_rv32_jit_memory_calls;
extern uint64_t gpsp_rv32_jit_memory_rom_loads;
extern uint64_t gpsp_rv32_jit_memory_ram_loads;
extern uint64_t gpsp_rv32_jit_memory_io_loads;
extern uint64_t gpsp_rv32_jit_memory_other_loads;
extern uint64_t gpsp_rv32_jit_memory_stores;
extern uint64_t gpsp_rv32_jit_rom_read_body_cycles;
extern uint64_t gpsp_rv32_jit_rom_read_body_calls;
extern uint64_t gpsp_rv32_jit_rom_read_page_misses;

static inline uint32_t memory_cycles_now(void)
{
  uint32_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles) :: "memory");
  return cycles;
}

static inline void thumb_memory_finish_timing(uint32_t start, bool rom_read)
{
  uint32_t elapsed = memory_cycles_now() - start;
  gpsp_rv32_jit_memory_cycles += elapsed;
  if (rom_read)
    gpsp_rv32_jit_rom_read_body_cycles += elapsed;
}
#endif

void gpsp_rv32_add_memory_alert(uint32_t alert)
{
  gpsp_rv32_pending_alert |= alert;
}

bool gpsp_rv32_store_hits_code(uint32_t address, unsigned bytes)
{
  unsigned region = address >> 24;
  if ((region != 2 && region != 3) ||
      (bytes != 1 && bytes != 2 && bytes != 4))
    return false;

  uint32_t mask = region == 2 ? 0x3ffffu : 0x7fffu;
  const uint8_t *tags = region == 2 ? ewram + 0x40000 : iwram;
  uint32_t offset = ((address & ~(bytes - 1u)) & mask) & ~1u;
  for (unsigned i = 0; i < bytes; i += 2)
  {
    uint16_t tag;
    memcpy(&tag, tags + offset + i, sizeof(tag));
    if (tag)
      return true;
  }
  return false;
}

bool gpsp_rv32_store_changes_code(uint32_t address, unsigned bytes,
    uint32_t value)
{
  if (!gpsp_rv32_store_hits_code(address, bytes))
    return false;

  uint32_t aligned = address & ~(bytes - 1u);
  const uint8_t *data = (address >> 24) == 2 ?
      ewram + (aligned & 0x3ffffu) : iwram + 0x8000 + (aligned & 0x7fffu);
  uint32_t old = 0;
  memcpy(&old, data, bytes);
  if (bytes < 4)
    value &= (1u << (bytes * 8)) - 1u;
  return old != value &&
      !gpsp_rv32_code_write_absorbed(aligned, bytes, old, value);
}

uint32_t gpsp_rv32_take_memory_alert(void)
{
  uint32_t alert = gpsp_rv32_pending_alert;
  gpsp_rv32_pending_alert = 0;
  return alert;
}

static uint64_t thumb_memory_access(uint32_t *regs, uint32_t *cycles,
    uint32_t address, uint32_t value, gpsp_rv32_memory_kind kind)
{
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  uint32_t memory_start = memory_cycles_now();
  gpsp_rv32_jit_memory_calls++;
  if (kind >= GPSP_RV32_MEM_STORE_U8)
    gpsp_rv32_jit_memory_stores++;
  else
  {
    unsigned region = address >> 24;
    if (region >= 0x08 && region <= 0x0d)
      gpsp_rv32_jit_memory_rom_loads++;
    else if (region == 0x02 || region == 0x03)
      gpsp_rv32_jit_memory_ram_loads++;
    else if (region == 0x04)
      gpsp_rv32_jit_memory_io_loads++;
    else
      gpsp_rv32_jit_memory_other_loads++;
  }
#endif
  bool store = kind >= GPSP_RV32_MEM_STORE_U8;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  unsigned rom_region = address >> 24;
  bool rom_read = !store && rom_region >= 8 && rom_region <= 0x0d &&
      (address & 0x1ffffffu) < gamepak_size &&
      (rom_region != 0x0d || backup_type != BACKUP_EEPROM);
  if (rom_read)
  {
    gpsp_rv32_jit_rom_read_body_calls++;
    if (!memory_map_read[address >> 15])
      gpsp_rv32_jit_rom_read_page_misses++;
  }
#endif
  bool word = kind == GPSP_RV32_MEM_LOAD_U32 ||
      kind == GPSP_RV32_MEM_STORE_U32;
  uint32_t pc = regs[REG_PC];
  uint32_t next_pc = pc + 2;
  uint32_t old_cpsr = regs[REG_CPSR];
  uint32_t old_mode = regs[CPU_MODE];
  uint32_t old_halt = regs[CPU_HALT_STATE];
  uint32_t result = 0;
  cpu_alert_type alert = CPU_ALERT_NONE;

  if (store)
    address &= word ? ~3u : kind == GPSP_RV32_MEM_STORE_U16 ? ~1u : ~0u;
  bool smc = store && gpsp_rv32_store_changes_code(address,
      word ? 4 : kind == GPSP_RV32_MEM_STORE_U16 ? 2 : 1, value);
  if (address < 0x10000000u)
    *cycles -= ws_cyc_nseq[address >> 24][word ? 1 : 0];

  /* The interpreter advances PC before memory access. Handler-only I/O
   * reads use the instruction PC for open-bus values. */
  uint32_t access_pc = next_pc;
  if (!store && (address >> 24) == 0x04 &&
      io_read_requires_handler(address))
    access_pc = pc;
  regs[REG_PC] = access_pc;

  switch (kind)
  {
    case GPSP_RV32_MEM_LOAD_U8:
      result = read_memory8(address);
      break;
    case GPSP_RV32_MEM_LOAD_U16:
      result = (uint16_t)read_memory16(address);
      break;
    case GPSP_RV32_MEM_LOAD_U32:
      result = read_memory32(address);
      break;
    case GPSP_RV32_MEM_LOAD_S8:
      result = read_memory8s(address);
      break;
    case GPSP_RV32_MEM_LOAD_S16:
      result = read_memory16s(address);
      break;
    case GPSP_RV32_MEM_STORE_U8:
      alert = write_memory8(address, value);
      break;
    case GPSP_RV32_MEM_STORE_U16:
      alert = write_memory16(address, value);
      break;
    case GPSP_RV32_MEM_STORE_U32:
      alert = write_memory32(address, value);
      break;
    default:
      break;
  }
  if (smc)
    alert |= CPU_ALERT_SMC;

  if (alert || regs[REG_PC] != access_pc ||
      regs[REG_CPSR] != old_cpsr || regs[CPU_MODE] != old_mode ||
      regs[CPU_HALT_STATE] != old_halt)
  {
    if (regs[REG_PC] == access_pc)
      regs[REG_PC] = next_pc;
    *cycles -= ws_cyc_seq[(regs[REG_PC] >> 24) & 15][0];
    if (regs[REG_PC] == idle_loop_target_pc && (int32_t)*cycles > 0)
      *cycles = 0;
    gpsp_rv32_add_memory_alert(alert);
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
    thumb_memory_finish_timing(memory_start, rom_read);
#endif
    return (uint64_t)result | GPSP_RV32_HELPER_EXIT;
  }

  regs[REG_PC] = pc;
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
  thumb_memory_finish_timing(memory_start, rom_read);
#endif
  return result;
}

#define thumb_memory_helper(name, kind)                                      \
static uint64_t name(uint32_t *regs, uint32_t *cycles,                       \
    uint32_t address, uint32_t value)                                         \
{                                                                            \
  return thumb_memory_access(regs, cycles, address, value, kind);            \
}

thumb_memory_helper(thumb_load_u8, GPSP_RV32_MEM_LOAD_U8)
thumb_memory_helper(thumb_load_u16, GPSP_RV32_MEM_LOAD_U16)
thumb_memory_helper(thumb_load_u32, GPSP_RV32_MEM_LOAD_U32)
thumb_memory_helper(thumb_load_s8, GPSP_RV32_MEM_LOAD_S8)
thumb_memory_helper(thumb_load_s16, GPSP_RV32_MEM_LOAD_S16)
thumb_memory_helper(thumb_store_u8, GPSP_RV32_MEM_STORE_U8)
thumb_memory_helper(thumb_store_u16, GPSP_RV32_MEM_STORE_U16)
thumb_memory_helper(thumb_store_u32, GPSP_RV32_MEM_STORE_U32)

const gpsp_rv32_helper_fn
    gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] =
{
  thumb_load_u8,
  thumb_load_u16,
  thumb_load_u32,
  thumb_load_s8,
  thumb_load_s16,
  thumb_store_u8,
  thumb_store_u16,
  thumb_store_u32,
};

uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
    uint32_t opcode, uint32_t unused)
{
  (void)unused;
  bool push = (opcode & 0xfe00u) == 0xb400u;
  bool pop = (opcode & 0xfe00u) == 0xbc00u;
  bool load = pop || (opcode & 0xf800u) == 0xc800u;
  bool extra = (push || pop) && (opcode & 0x0100u);
  unsigned base_reg = push || pop ? REG_SP : (opcode >> 8) & 7;
  unsigned mask = opcode & 0xffu;
  unsigned count = extra ? 1 : 0;
  for (unsigned i = 0; i < 8; i++)
    count += (mask >> i) & 1;

  uint32_t base = regs[base_reg];
  uint32_t end = push ? base - count * 4 : base + count * 4;
  uint32_t address = (push ? end : base) & ~3u;
  bool base_first = (mask & (1u << base_reg)) &&
      !(mask & ((1u << base_reg) - 1u));
  bool writeback_first = load || !base_first;
  if (writeback_first)
    regs[base_reg] = end;

  uint32_t instruction_pc = regs[REG_PC];
  regs[REG_PC] = instruction_pc + 2;
  uint32_t alert = CPU_ALERT_NONE;
  for (unsigned i = 0; i < 8; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    if (address < 0x10000000u)
      *cycles -= ws_cyc_seq[address >> 24][1];
    if (load)
    {
      uint32_t access_pc = regs[REG_PC];
      if ((address >> 24) == 4 && io_read_requires_handler(address))
        regs[REG_PC] = instruction_pc;
      uint32_t value = read_memory32(address);
      regs[REG_PC] = access_pc;
      regs[i] = value;
    }
    else
      alert |= write_memory32(address, regs[i]);
    address += 4;
  }
  if (extra)
  {
    if (address < 0x10000000u)
      *cycles -= ws_cyc_seq[address >> 24][1];
    if (pop)
    {
      if ((address >> 24) == 4 && io_read_requires_handler(address))
        regs[REG_PC] = instruction_pc;
      uint32_t value = read_memory32(address);
      regs[REG_PC] = value & ~1u;
    }
    else
      alert |= write_memory32(address, regs[REG_LR]);
  }
  if (!writeback_first)
    regs[base_reg] = end;

  uint32_t pc = regs[REG_PC];
  unsigned region = (pc >> 24) & 15;
  *cycles -= ws_cyc_seq[region][0];
  if (pc == idle_loop_target_pc && (int32_t)*cycles > 0)
    *cycles = 0;
  gpsp_rv32_add_memory_alert(alert);
  return GPSP_RV32_HELPER_EXIT;
}

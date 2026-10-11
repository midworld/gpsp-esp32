/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_block_emit.h"

#include "rv32_arm_alu.h"
#include "rv32_arm_branch.h"
#include "rv32_arm_memory.h"
#include "rv32_arm_system.h"
#include "rv32_arm_mul.h"
#include "rv32_block_link.h"
#include "rv32_code_arena.h"
#include "rv32_codegen.h"
#include "rv32_fallback.h"
#include "rv32_interpreter_stub.h"
#include "rv32_register_map.h"
#include "rv32_rom_load.h"
#include "rv32_stub.h"
#include "rv32_thumb_alu.h"
#include "rv32_thumb_memory.h"
#include "rv32_thumb_multi_ram.h"
#include "rv32_thumb_ram_helper.h"
#include "rv32_thumb_ram_store.h"

#include <string.h>

extern void gpsp_rv32_thumb_pop_exit_shared(void);

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
static struct gpsp_rv32_emit_byte_stats emit_byte_stats;

void gpsp_rv32_emit_get_byte_stats(struct gpsp_rv32_emit_byte_stats *stats)
{
  *stats = emit_byte_stats;
}

#define emit_count_bytes(field, bytes) (emit_byte_stats.field += (bytes))
#else
#define emit_count_bytes(field, bytes) ((void)0)
#endif

extern uint8_t ws_cyc_seq[16][2];
extern uint8_t ws_cyc_nseq[16][2];
extern uint32_t idle_loop_target_pc;
extern uint8_t *rom_translation_ptr;
extern uint8_t *last_rom_translation_ptr;
extern uint32_t rom_cache_watermark;

void init_emitter(bool must_swap)
{
  (void)must_swap;
  if (!gpsp_rv32_rom_arena.count)
    return;
  /* Offset zero is the hash table's empty-entry marker. */
  gpsp_rv32_rom_arena.current = 0;
  rom_translation_ptr = gpsp_rv32_arena_at(&gpsp_rv32_rom_arena,
      rom_cache_watermark);
  last_rom_translation_ptr = rom_translation_ptr;
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

static bool emit_absolute_load(gpsp_rv32_code *code, unsigned rd,
    unsigned scratch, uint32_t address, bool word)
{
  int low = (int)(address & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((address + 0x800u) >> 12) & 0xfffff;
  if (upper && !gpsp_rv32_lui(code, scratch, upper))
    return false;
  return word ? gpsp_rv32_lw(code, rd, upper ? scratch : X0, low) :
      gpsp_rv32_lbu(code, rd, upper ? scratch : X0, low);
}

static bool emit_indexed_wait_load(gpsp_rv32_code *code,
    uint32_t address)
{
  int low = (int)(address & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((address + 0x800u) >> 12) & 0xfffff;
#if GPSP_RV32_USE_ZBA
  /* RV_TMP0 holds the region; entries are two bytes apart. */
  return (!upper || gpsp_rv32_lui(code, RV_TMP1, upper)) &&
      gpsp_rv32_sh1add(code, RV_TMP1, RV_TMP0, upper ? RV_TMP1 : X0) &&
      gpsp_rv32_lbu(code, RV_TMP1, RV_TMP1, low);
#else
  if (upper && (!gpsp_rv32_lui(code, RV_TMP1, upper) ||
      !gpsp_rv32_add(code, RV_TMP1, RV_TMP1, RV_TMP0)))
    return false;
  return gpsp_rv32_lbu(code, RV_TMP1, upper ? RV_TMP1 : RV_TMP0, low);
#endif
}

static bool emit_absolute_call(gpsp_rv32_code *code, unsigned link,
    unsigned scratch, uint32_t target)
{
  int low = (int)(target & 0xfff);
  if (low >= 0x800)
    low -= 0x1000;
  uint32_t upper = ((target + 0x800u) >> 12) & 0xfffff;
  return gpsp_rv32_lui(code, scratch, upper) &&
      gpsp_rv32_jalr(code, link, scratch, low);
}

static bool emit_return(gpsp_rv32_code *code)
{
  return gpsp_rv32_jalr(code, X0, RA, 0);
}

static bool emit_advance(gpsp_rv32_code *code, uint32_t pc, bool thumb);
static bool emit_advance_wait(gpsp_rv32_code *code, uint32_t pc,
    bool thumb, uintptr_t wait_addr);

static bool emit_guard(gpsp_rv32_code *code)
{
  /* A block can stop at a video/timer boundary between two instructions. */
  /* HALT changes in C paths, which exit before the next instruction. */
  /* Return the next native PC in a1; the wrapper skips its a1 reset. */
  uint8_t *skip = code->ptr;
  return gpsp_rv32_blt(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_jalr(code, A1, RA, 4) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

/* At the idle loop target a positive budget ends. */
static bool emit_idle_clamp(gpsp_rv32_code *code)
{
  uint8_t *skip = code->ptr;
  return gpsp_rv32_bge(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_addi(code, RV_BUDGET, X0, 0) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

bool gpsp_rv32_thumb_batch_alu(uint16_t opcode)
{
  if (gpsp_rv32_thumb_needs_helper(opcode) ||
      (opcode & 0xf000) == 0xd000 ||
      (opcode & 0xf800) == 0xe000 ||
      (opcode & 0xf000) == 0xf000)
    return false;

  if ((opcode & 0xfc00) == 0x4400)
  {
    unsigned operation = (opcode >> 8) & 3;
    unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
    if (operation == 3 || (rd == 15 && operation != 1))
      return false;
  }

  if ((opcode & 0xfc00) == 0x4000)
  {
    unsigned operation = (opcode >> 6) & 15;
    if (operation == 2 || operation == 3 || operation == 4 ||
        operation == 7 || operation == 13)
      return false;
  }
  return true;
}

bool gpsp_rv32_thumb_batch_pc_sensitive(uint16_t opcode)
{
  /* These ALU forms can read the Thumb PC. */
  return (opcode & 0xfc00) == 0x4400 ||
      (opcode & 0xf000) == 0xa000;
}

static bool emit_scaled_wait(gpsp_rv32_code *code, unsigned count)
{
  if (count == 1)
    return true;
  if (!(count & (count - 1)))
  {
    unsigned shift = 0;
    while ((1u << shift) != count)
      shift++;
    return gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, shift);
  }
#if GPSP_RV32_USE_ZBA
  if (count == 3 || count == 5 || count == 9)
    return gpsp_rv32_shadd(code, count == 3 ? 1 : count == 5 ? 2 : 3,
        RV_TMP0, RV_TMP0, RV_TMP0);
#endif
  if (count == 3)
    return gpsp_rv32_slli(code, RV_TMP1, RV_TMP0, 1) &&
        gpsp_rv32_add(code, RV_TMP0, RV_TMP0, RV_TMP1);
  return gpsp_rv32_addi(code, RV_TMP1, X0, count) &&
      gpsp_rv32_mul(code, RV_TMP0, RV_TMP0, RV_TMP1);
}

static unsigned fixed_fetch_wait(unsigned region, bool thumb)
{
  /* WAITCNT changes ROM waits only. */
  if (region == 0 || region == 3)
    return 1;
  if (region == 2)
    return thumb ? 3 : 6;
  return 0;
}

static unsigned fixed_thumb_wait_address(uintptr_t address)
{
  if (address == (uintptr_t)&ws_cyc_seq[0][0] ||
      address == (uintptr_t)&ws_cyc_seq[3][0])
    return 1;
  return address == (uintptr_t)&ws_cyc_seq[2][0] ? 3 : 0;
}

static bool emit_thumb_preflight_code(uint8_t **translation_ptr,
    uint8_t *translation_limit, uintptr_t fetch_wait_addr,
    unsigned instruction_count, uint8_t **slow_site)
{
  unsigned fixed_wait = fixed_thumb_wait_address(fetch_wait_addr);
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      !slow_site ||
      ((uintptr_t)*translation_ptr & 3) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      instruction_count == 0 || instruction_count > 32 ||
      !fetch_wait_addr || (!fixed_wait && fetch_wait_addr > UINT32_MAX))
    return false;

  uint32_t words[32];
  gpsp_rv32_code staged;
  gpsp_rv32_code_init(&staged, words, sizeof(words));

  /* No guest side effect can change WAITCNT within this span. */
  if (!emit_guard(&staged) ||
      !(fixed_wait ?
        gpsp_rv32_addi(&staged, RV_TMP0, X0, fixed_wait * instruction_count) :
        (emit_absolute_load(&staged, RV_TMP0, RV_TMP0,
            (uint32_t)fetch_wait_addr, false) &&
         emit_scaled_wait(&staged, instruction_count))))
    return false;
  uint8_t *fast_path = staged.ptr;
  uint8_t *slot = NULL;
  if (!gpsp_rv32_bge(&staged, RV_BUDGET, RV_TMP0, 0) ||
      !gpsp_rv32_emit_link_slot(&staged, &slot) ||
      !gpsp_rv32_patch_branch(&staged, fast_path, staged.ptr) ||
      !gpsp_rv32_sub(&staged, RV_BUDGET, RV_BUDGET, RV_TMP0))
    return false;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)translation_limit - (uintptr_t)*translation_ptr < size)
    return false;
  uint8_t *output = *translation_ptr;
  memcpy(output, words, size);
  *slow_site = output + (slot - staged.start);
  *translation_ptr += size;
  emit_count_bytes(batch_preflight, size);
  return true;
}

bool gpsp_rv32_emit_thumb_preflight(uint8_t **translation_ptr,
    uint8_t *translation_limit, uintptr_t fetch_wait_addr,
    unsigned instruction_count, uint8_t **slow_site)
{
  return emit_thumb_preflight_code(translation_ptr, translation_limit,
      fetch_wait_addr, instruction_count, slow_site);
}

static bool emit_thumb_batch_slow_code(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uintptr_t wait_addr)
{
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      ((uintptr_t)*translation_ptr & 3) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      (pc & 1) || !gpsp_rv32_thumb_batch_alu(opcode))
    return false;

  uint32_t words[256];
  gpsp_rv32_code staged;
  gpsp_rv32_code_init(&staged, words, sizeof(words));

  /* This path can stop after one instruction, so all live flags must survive. */
  if (gpsp_rv32_emit_thumb(&staged, opcode, pc, 0xF) !=
          GPSP_RV32_THUMB_EMITTED ||
      !emit_advance_wait(&staged, pc, true, wait_addr) ||
      !emit_return(&staged))
    return false;

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)translation_limit - (uintptr_t)*translation_ptr < size)
    return false;
  memcpy(*translation_ptr, words, size);
  *translation_ptr += size;
  emit_count_bytes(batch_slow, size);
  return true;
}

bool gpsp_rv32_emit_thumb_batch_slow(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode)
{
  uintptr_t wait_addr =
      (uintptr_t)&ws_cyc_seq[((pc + 2) >> 24) & 15][0];
  return emit_thumb_batch_slow_code(translation_ptr, translation_limit,
      pc, opcode, wait_addr);
}

bool gpsp_rv32_emit_thumb_side_entry(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uintptr_t fetch_wait_addr, unsigned prefix_count,
    unsigned remaining_count, uint8_t *body,
    uintptr_t preflight_stub, uint8_t **slow_literal)
{
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      !body || !slow_literal || !fetch_wait_addr || !preflight_stub ||
      (pc & 1) || !prefix_count ||
      !remaining_count ||
      prefix_count + remaining_count > 32 ||
      !gpsp_rv32_thumb_batch_alu(opcode) ||
      ((uintptr_t)*translation_ptr & 3) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      ((uintptr_t)body & 3) || (preflight_stub & 3) ||
      (uintptr_t)body > UINT32_MAX || preflight_stub > UINT32_MAX ||
      fetch_wait_addr > UINT32_MAX ||
      (uintptr_t)translation_limit - (uintptr_t)*translation_ptr < 24)
    return false;

  uint32_t words[6];
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, words, sizeof(words));
  /* The stub reads data at the return address and retries 12 bytes back.
     The shared batch finish advances from its original head. */
  unsigned counts = (remaining_count << 5) | prefix_count;
  if (!gpsp_rv32_fixed_begin(&code) ||
      !gpsp_rv32_addi(&code, A0, X0, counts) ||
      !emit_absolute_call(&code, RV_TMP1, RV_TMP1, (uint32_t)preflight_stub))
    return false;
  words[3] = (uint32_t)(uintptr_t)body;
  words[4] = 0;
  words[5] = (uint32_t)fetch_wait_addr;
  uint8_t *output = *translation_ptr;
  memcpy(output, words, sizeof(words));
  *slow_literal = output + 16;
  *translation_ptr = output + sizeof(words);
  emit_count_bytes(batch_preflight, sizeof(words));
  return true;
}

bool gpsp_rv32_emit_thumb_batch_finish(uint8_t **translation_ptr,
    uint8_t *translation_limit, unsigned instruction_count)
{
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      ((uintptr_t)*translation_ptr & 1) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      instruction_count == 0 || instruction_count > 32 ||
      (uintptr_t)translation_limit - (uintptr_t)*translation_ptr < 4)
    return false;

  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, *translation_ptr, 4);
  if (!gpsp_rv32_addi(&code, RV_PC, RV_PC, instruction_count * 2))
    return false;
  *translation_ptr = code.ptr;
  emit_count_bytes(batch_preflight, 4);
  return true;
}

static bool emit_advance(gpsp_rv32_code *code, uint32_t pc, bool thumb)
{
  uint32_t next_pc = pc + (thumb ? 2 : 4);
  unsigned region = (next_pc >> 24) & 15;
  uintptr_t wait_addr = (uintptr_t)&ws_cyc_seq[region][thumb ? 0 : 1];
  return emit_advance_wait(code, pc, thumb, wait_addr);
}

static bool emit_advance_wait(gpsp_rv32_code *code, uint32_t pc,
    bool thumb, uintptr_t wait_addr)
{
  uint32_t next_pc = pc + (thumb ? 2 : 4);
  unsigned fixed_wait = fixed_fetch_wait((next_pc >> 24) & 15, thumb);
  if (!fixed_wait && wait_addr > UINT32_MAX)
    return false;

  /* The ROM loader fixes the idle target before translating blocks. */
  return gpsp_rv32_addi(code, RV_PC, RV_PC, thumb ? 2 : 4) &&
      (fixed_wait ?
       gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)fixed_wait) :
       (emit_absolute_load(code, RV_TMP1, RV_TMP0, (uint32_t)wait_addr,
            false) &&
        gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1))) &&
      (next_pc != idle_loop_target_pc || emit_idle_clamp(code));
}

/* Charge the fetch wait known at translation time, like flow regions. */
static bool emit_advance_static(gpsp_rv32_code *code, uint32_t pc,
    bool thumb)
{
  uint32_t next_pc = pc + (thumb ? 2 : 4);
  unsigned region = (next_pc >> 24) & 15;
  unsigned wait = fixed_fetch_wait(region, thumb);
  if (!wait)
    wait = ws_cyc_seq[region][thumb ? 0 : 1];
  return gpsp_rv32_addi(code, RV_PC, RV_PC, thumb ? 2 : 4) &&
      (!wait || gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)wait)) &&
      (next_pc != idle_loop_target_pc || emit_idle_clamp(code));
}

static bool emit_branch_wait(gpsp_rv32_code *code, uint32_t target,
    bool thumb)
{
  unsigned region = (target >> 24) & 15;
  unsigned size = thumb ? 0 : 1;
  unsigned fixed_wait = fixed_fetch_wait(region, thumb);
  uintptr_t nseq_addr = (uintptr_t)&ws_cyc_nseq[region][size];
  uintptr_t seq_addr = (uintptr_t)&ws_cyc_seq[region][size];
  if (!fixed_wait && (nseq_addr > UINT32_MAX || seq_addr > UINT32_MAX))
    return false;

  return (fixed_wait ?
       gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)(fixed_wait * 2)) :
       (emit_absolute_load(code, RV_TMP1, RV_TMP0,
            (uint32_t)nseq_addr, false) &&
        gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1) &&
        emit_absolute_load(code, RV_TMP1, RV_TMP0,
            (uint32_t)seq_addr, false) &&
        gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1))) &&
      (target != idle_loop_target_pc || emit_idle_clamp(code));
}

static bool emit_branch_advance(gpsp_rv32_code *code, uint32_t pc,
    uint32_t target, bool thumb)
{
  int32_t delta = (int32_t)(target - pc);
  return (delta >= -2048 && delta <= 2047 ?
      gpsp_rv32_addi(code, RV_PC, RV_PC, delta) :
      emit_constant(code, RV_PC, target)) &&
      emit_branch_wait(code, target, thumb);
}

static bool emit_thumb_bcc_advance(gpsp_rv32_code *code, uint32_t pc,
    uint32_t target)
{
  /* Thumb Bcc targets are within one RV32 ADDI of the current PC. */
  return gpsp_rv32_addi(code, RV_PC, RV_PC, (int32_t)(target - pc)) &&
      emit_branch_wait(code, target, true);
}

static bool emit_dynamic_wait(gpsp_rv32_code *code, bool thumb,
    bool nonsequential, bool sequential)
{
  uintptr_t nseq_addr = (uintptr_t)ws_cyc_nseq;
  uintptr_t seq_addr = (uintptr_t)ws_cyc_seq;
  if (nseq_addr > UINT32_MAX || seq_addr > UINT32_MAX)
    return false;

  if (!gpsp_rv32_srli(code, RV_TMP0, RV_PC, 24) ||
      !gpsp_rv32_andi(code, RV_TMP0, RV_TMP0, 15) ||
      (!GPSP_RV32_USE_ZBA && !gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, 1)))
    return false;
  if (nonsequential &&
      (!emit_indexed_wait_load(code,
          (uint32_t)nseq_addr + (thumb ? 0 : 1)) ||
       !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1)))
    return false;
  return !sequential ||
      (emit_indexed_wait_load(code,
           (uint32_t)seq_addr + (thumb ? 0 : 1)) &&
       gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1));
}

static bool emit_dynamic_idle(gpsp_rv32_code *code)
{
  uintptr_t idle_addr = (uintptr_t)&idle_loop_target_pc;
  if (idle_addr > UINT32_MAX ||
      !emit_absolute_load(code, RV_TMP0, RV_TMP0, (uint32_t)idle_addr, true))
    return false;
  uint8_t *skip = code->ptr;
  return gpsp_rv32_bne(code, RV_PC, RV_TMP0, 0) && emit_idle_clamp(code) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_thumb_state(gpsp_rv32_code *code)
{
  return gpsp_rv32_lw(code, RV_TMP0, RV_STATE, GPSP_RV32_CPSR_OFFSET) &&
      gpsp_rv32_andi(code, RV_TMP0, RV_TMP0, -33) &&
      gpsp_rv32_slli(code, RV_TMP1, A1, 5) &&
      gpsp_rv32_or(code, RV_TMP0, RV_TMP0, RV_TMP1) &&
      gpsp_rv32_sw(code, RV_TMP0, RV_STATE, GPSP_RV32_CPSR_OFFSET);
}

static bool emit_dynamic_thumb_exit(gpsp_rv32_code *code,
    uint16_t opcode, bool target_ready)
{
  bool bx = (opcode & 0xff00) == 0x4700;
  bool bl_second = (opcode & 0xf800) == 0xf800;
  if ((!target_ready && !gpsp_rv32_addi(code, RV_PC, A0, 0)) ||
      (bx && !emit_thumb_state(code)))
    return false;

  uint8_t *arm_bx = NULL;
  if (bx)
  {
    arm_bx = code->ptr;
    if (!gpsp_rv32_beq(code, A1, X0, 0))
      return false;
  }
  if (!emit_dynamic_wait(code, true, bl_second, true) ||
      !emit_dynamic_idle(code) ||
      (arm_bx && !gpsp_rv32_patch_branch(code, arm_bx, code->ptr)))
    return false;
  return emit_return(code);
}

static bool emit_dynamic_arm_bx(gpsp_rv32_code *code)
{
  if (!gpsp_rv32_addi(code, RV_PC, A0, 0) ||
      !emit_thumb_state(code))
    return false;
  uint8_t *thumb_target = code->ptr;
  if (!gpsp_rv32_bne(code, A1, X0, 0) ||
      !emit_dynamic_wait(code, false, true, true) ||
      !emit_dynamic_idle(code) ||
      !gpsp_rv32_patch_branch(code, thumb_target, code->ptr) ||
      !emit_return(code))
    return false;
  return true;
}

static bool emit_dynamic_arm_pc_exit(gpsp_rv32_code *code, uint32_t pc)
{
  uint8_t *not_taken = code->ptr;
  if (!gpsp_rv32_beq(code, A1, X0, 0) ||
      !gpsp_rv32_addi(code, RV_PC, A0, 0) ||
      !emit_dynamic_wait(code, false, false, true) ||
      !emit_dynamic_idle(code) ||
      !emit_return(code) ||
      !gpsp_rv32_patch_branch(code, not_taken, code->ptr))
    return false;
  return emit_advance(code, pc, false) && emit_return(code);
}

static uint32_t arm_branch_target(uint32_t pc, uint32_t opcode)
{
  int32_t offset = (int32_t)(opcode & 0xffffff);
  if (offset & 0x800000)
    offset -= 0x1000000;
  return pc + 8 + (uint32_t)(offset * 4);
}

static uint32_t thumb_branch_target(uint32_t pc, uint16_t opcode,
    bool conditional)
{
  int32_t offset = opcode & (conditional ? 0xff : 0x7ff);
  if (offset & (conditional ? 0x80 : 0x400))
    offset -= conditional ? 0x100 : 0x800;
  return pc + 4 + (uint32_t)(offset * 2);
}

static bool emit_thumb_dynamic_target(gpsp_rv32_code *code,
    uint16_t opcode)
{
  if ((opcode & 0xf800) == 0xf800) /* BL second half */
  {
    unsigned offset = (opcode & 0x7ff) * 2;
    if (offset <= 2047)
      return gpsp_rv32_addi(code, RV_PC, RV_GUEST_LR, offset);
    return emit_constant(code, A2, offset) &&
        gpsp_rv32_add(code, RV_PC, RV_GUEST_LR, A2);
  }

  unsigned operation = (opcode >> 8) & 3;
  unsigned rs = (opcode >> 3) & 15;
  if (operation == 2) /* MOV PC,Rs */
    return rs == 15 ? gpsp_rv32_addi(code, RV_PC, RV_PC, 4) :
        gpsp_rv32_andi(code, RV_PC, guest_reg_map[rs], -2);

  /* ADD PC,Rs reads the current Thumb PC plus four. */
  if (!gpsp_rv32_addi(code, RV_PC, RV_PC, 4) ||
      !(rs == 15 ? gpsp_rv32_add(code, RV_PC, RV_PC, RV_PC) :
                    gpsp_rv32_add(code, RV_PC, RV_PC, guest_reg_map[rs])))
    return false;
  return rs == 15 || gpsp_rv32_andi(code, RV_PC, RV_PC, -2);
}

static bool emit_thumb_pc_add(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode)
{
  unsigned rd = (opcode >> 8) & 7;
  int32_t delta = (int32_t)((((pc + 4) & ~3u) +
      ((opcode & 0xff) << 2)) - pc);
  return gpsp_rv32_addi(code, guest_reg_map[rd], RV_PC, delta);
}

static int32_t thumb_literal_delta(uint32_t pc, uint16_t opcode)
{
  return (int32_t)(((pc & ~3u) + 4 + ((opcode & 0xff) << 2)) - pc);
}

bool gpsp_rv32_thumb_rom_literal_address(uint32_t pc, uint16_t opcode,
    uint32_t gamepak_size, uint32_t *address)
{
  if (!address || (pc & 1) || (pc >> 24) < 8 || (pc >> 24) > 12 ||
      (opcode & 0xf800) != 0x4800)
    return false;

  uint32_t target = (pc & ~3U) + 4 + ((opcode & 0xff) << 2);
  uint32_t offset = target & 0x1ffffff;
  if (((target + 3) >> 15) != (pc >> 15) ||
      offset > gamepak_size || gamepak_size - offset < 4)
    return false;
  /* Cartridge GPIO can change these ROM-mapped words at runtime. */
  if (offset >= 0xc4 && offset < 0xcc)
    return false;

  *address = target;
  return true;
}

static bool emit_thumb_literal_constant_wait(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uint32_t value, uintptr_t data_wait_addr, uintptr_t fetch_wait_addr)
{
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      ((uintptr_t)*translation_ptr & 1) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      (pc & 1) || (pc >> 24) < 8 || (pc >> 24) > 12 ||
      (opcode & 0xf800) != 0x4800)
    return false;

  if (!data_wait_addr || !fetch_wait_addr ||
      data_wait_addr > UINT32_MAX || fetch_wait_addr > UINT32_MAX)
    return false;
  uint32_t words[16];
  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, words, sizeof(words));
  if (!emit_guard(&code))
    return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  uint8_t *body = code.ptr;
#endif
  if (!emit_constant(&code, guest_reg_map[(opcode >> 8) & 7], value))
    return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  uint8_t *timing = code.ptr;
#endif
  if (!emit_absolute_load(&code, RV_TMP1, RV_TMP0,
          (uint32_t)data_wait_addr, false) ||
      !gpsp_rv32_sub(&code, RV_BUDGET, RV_BUDGET, RV_TMP1) ||
      !emit_advance_wait(&code, pc, true, fetch_wait_addr))
    return false;
  size_t size = (size_t)(code.ptr - code.start);
  if ((uintptr_t)translation_limit - (uintptr_t)*translation_ptr < size)
    return false;
  memcpy(*translation_ptr, words, size);
  *translation_ptr += size;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  emit_count_bytes(guard, (size_t)(body - code.start));
  emit_count_bytes(body, (size_t)(timing - body));
  emit_count_bytes(timing, (size_t)(code.ptr - timing));
#endif
  return true;
}

bool gpsp_rv32_emit_thumb_literal_constant(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint16_t opcode,
    uint32_t value)
{
  if ((pc >> 24) < 8 || (pc >> 24) > 12)
    return false;
  return emit_thumb_literal_constant_wait(translation_ptr,
      translation_limit, pc, opcode, value,
      (uintptr_t)&ws_cyc_nseq[pc >> 24][1],
      (uintptr_t)&ws_cyc_seq[((pc + 2) >> 24) & 15][0]);
}

static bool is_thumb_memory(uint16_t opcode)
{
  return (opcode & 0xf800) == 0x4800 ||
      (opcode >= 0x5000 && opcode < 0xa000);
}

static bool is_thumb_multi(uint16_t opcode)
{
  return (opcode & 0xfe00) == 0xb400 ||
      (opcode & 0xfe00) == 0xbc00 ||
      (opcode & 0xf000) == 0xc000;
}

static unsigned thumb_multi_count(uint16_t opcode)
{
  unsigned mask = opcode & 0xff;
  unsigned count = 0;
  for (unsigned i = 0; i < 8; i++)
    count += (mask >> i) & 1;
  return count + (((opcode & 0xf000) == 0xb000) &&
      ((opcode & 0x0100) != 0));
}

bool gpsp_rv32_thumb_multi_native(uint16_t opcode)
{
  if (!is_thumb_multi(opcode) || !thumb_multi_count(opcode))
    return false;

  /* A nonfirst STMIA base in the list is unpredictable on ARM7TDMI. */
  if ((opcode & 0xf800) == 0xc000)
  {
    unsigned base = (opcode >> 8) & 7;
    unsigned mask = opcode & 0xff;
    if ((mask & (1u << base)) && (mask & ((1u << base) - 1)))
      return false;
  }
  return true;
}

static bool emit_thumb_multi(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode, bool *exits)
{
  bool push = (opcode & 0xfe00) == 0xb400;
  bool pop = (opcode & 0xfe00) == 0xbc00;
  bool load = pop || (opcode & 0xf800) == 0xc800;
  bool extra = (push || pop) && (opcode & 0x0100);
  unsigned base = push || pop ? 13 : (opcode >> 8) & 7;
  unsigned mask = opcode & 0xff;
  unsigned count = thumb_multi_count(opcode);
  bool base_first_store = !load && !push &&
      (mask & (1u << base)) && !(mask & ((1u << base) - 1));
  int adjust = (push ? -1 : 1) * (int)(count * 4);
  unsigned base_reg = guest_reg_map[base];
  uintptr_t resolver = (uintptr_t)(load ? gpsp_rv32_thumb_multi_ram :
      gpsp_rv32_thumb_multi_ram_store);
  if (resolver > UINT32_MAX ||
      !gpsp_rv32_addi(code, A1, base_reg, push ? adjust : 0) ||
      !gpsp_rv32_andi(code, A1, A1, -4) ||
      !gpsp_rv32_addi(code, A2, X0, count) ||
      !emit_absolute_call(code, RV_TMP1, A0, (uint32_t)resolver))
    return false;

  uint8_t *slow = code->ptr;
  if (!gpsp_rv32_beq(code, A0, X0, 0) ||
      (!base_first_store &&
       !gpsp_rv32_addi(code, base_reg, base_reg, adjust)))
    return false;

  unsigned offset = 0;
  for (unsigned i = 0; i < 8; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    if (!(load ? gpsp_rv32_lw(code, guest_reg_map[i], A0, offset) :
                 gpsp_rv32_sw(code, guest_reg_map[i], A0, offset)))
      return false;
    offset += 4;
  }
  if (extra &&
      !(pop ? gpsp_rv32_lw(code, RV_PC, A0, offset) :
               gpsp_rv32_sw(code, RV_GUEST_LR, A0, offset)))
    return false;
  if ((base_first_store &&
       !gpsp_rv32_addi(code, base_reg, base_reg, adjust)) ||
      !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, A1))
    return false;

  *exits = pop && extra;
  if (*exits)
  {
    uintptr_t shared = (uintptr_t)gpsp_rv32_thumb_pop_exit_shared;
    if (shared > UINT32_MAX ||
        !gpsp_rv32_andi(code, RV_PC, RV_PC, -2) ||
        !emit_absolute_call(code, X0, RV_TMP1, (uint32_t)shared))
      return false;
  }
  else if (!emit_advance_static(code, pc, true))
    return false;

  uint8_t *skip_slow = NULL;
  if (!*exits)
  {
    skip_slow = code->ptr;
    if (!gpsp_rv32_jal(code, X0, 0))
      return false;
  }
  uint8_t *slow_target = code->ptr;
  uintptr_t bridge = (uintptr_t)gpsp_rv32_thumb_multi_slow_bridge;
  if (bridge > UINT32_MAX ||
      !emit_constant(code, A1, opcode) ||
      !emit_absolute_call(code, X0, RV_TMP1, (uint32_t)bridge) ||
      !gpsp_rv32_patch_branch(code, slow, slow_target) ||
      (skip_slow && !gpsp_rv32_patch_branch(code, skip_slow, code->ptr)))
    return false;
  return true;
}

static bool emit_thumb_memory_code(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode, bool resume_on_budget, bool charge_fetch,
    bool *advanced)
{
  *advanced = false;
  gpsp_rv32_memory_kind kind;
  unsigned rd;
  /* a1 = base + index, or base + offset when index is X0. */
  unsigned base;
  unsigned index = X0;
  int32_t offset = 0;

  if ((opcode & 0xf800) == 0x4800)
  {
    kind = GPSP_RV32_MEM_LOAD_U32;
    rd = (opcode >> 8) & 7;
    base = RV_PC;
    offset = thumb_literal_delta(pc, opcode);
  }
  else if ((opcode & 0xf000) == 0x5000)
  {
    static const gpsp_rv32_memory_kind kinds[8] =
    {
      GPSP_RV32_MEM_STORE_U32, GPSP_RV32_MEM_STORE_U16,
      GPSP_RV32_MEM_STORE_U8, GPSP_RV32_MEM_LOAD_S8,
      GPSP_RV32_MEM_LOAD_U32, GPSP_RV32_MEM_LOAD_U16,
      GPSP_RV32_MEM_LOAD_U8, GPSP_RV32_MEM_LOAD_S16,
    };
    kind = kinds[(opcode >> 9) & 7];
    rd = opcode & 7;
    base = guest_reg_map[(opcode >> 3) & 7];
    index = guest_reg_map[(opcode >> 6) & 7];
  }
  else if ((opcode & 0xe000) == 0x6000)
  {
    static const gpsp_rv32_memory_kind kinds[4] =
    {
      GPSP_RV32_MEM_STORE_U32, GPSP_RV32_MEM_LOAD_U32,
      GPSP_RV32_MEM_STORE_U8, GPSP_RV32_MEM_LOAD_U8,
    };
    unsigned operation = (opcode >> 11) & 3;
    kind = kinds[operation];
    rd = opcode & 7;
    unsigned field = (opcode >> 6) & 31;
    base = guest_reg_map[(opcode >> 3) & 7];
    offset = operation < 2 ? field << 2 : field;
  }
  else if ((opcode & 0xf000) == 0x8000)
  {
    kind = (opcode & 0x0800) ? GPSP_RV32_MEM_LOAD_U16 :
        GPSP_RV32_MEM_STORE_U16;
    rd = opcode & 7;
    base = guest_reg_map[(opcode >> 3) & 7];
    offset = ((opcode >> 6) & 31) << 1;
  }
  else if ((opcode & 0xf000) == 0x9000)
  {
    kind = (opcode & 0x0800) ? GPSP_RV32_MEM_LOAD_U32 :
        GPSP_RV32_MEM_STORE_U32;
    rd = (opcode >> 8) & 7;
    base = RV_GUEST_SP;
    offset = (opcode & 0xff) << 2;
  }
  else
    return false;

  bool store = kind >= GPSP_RV32_MEM_STORE_U8;
  bool normal_successor = pc + 2 != idle_loop_target_pc;
  /* PC-relative literals read ROM directly, except in code run from RAM. */
  bool ram_code = (pc >> 24) == 0x02 || (pc >> 24) == 0x03;
  bool ram_load = normal_successor && !store &&
      ((opcode & 0xf800) != 0x4800 || ram_code);
  bool ram_store = normal_successor && store;
  /* A flow RAM helper retries from 12 (load) or 16 (store) bytes before
     its return address: the address, store value, and call. */
  bool retry = resume_on_budget && (ram_load || ram_store);
  if ((retry && !gpsp_rv32_fixed_begin(code)) ||
      !(index != X0 ? gpsp_rv32_add(code, A1, base, index) :
        gpsp_rv32_addi(code, A1, base, offset)) ||
      (store && !gpsp_rv32_addi(code, A2, guest_reg_map[rd], 0)))
    return false;

  static void (*const ram_loads[5])(void) =
  {
    gpsp_rv32_ram_load_u8,
    gpsp_rv32_ram_load_u16,
    gpsp_rv32_ram_load_u32,
    gpsp_rv32_ram_load_s8,
    gpsp_rv32_ram_load_s16,
  };
  static void (*const ram_stores[3])(void) =
  {
    gpsp_rv32_ram_store_u8,
    gpsp_rv32_ram_store_u16,
    gpsp_rv32_ram_store_u32,
  };
  static void (*const flow_loads[5])(void) =
  {
    gpsp_rv32_ram_load_u8_flow,
    gpsp_rv32_ram_load_u16_flow,
    gpsp_rv32_ram_load_u32_flow,
    gpsp_rv32_ram_load_s8_flow,
    gpsp_rv32_ram_load_s16_flow,
  };
  static void (*const flow_stores[3])(void) =
  {
    gpsp_rv32_ram_store_u8_flow,
    gpsp_rv32_ram_store_u16_flow,
    gpsp_rv32_ram_store_u32_flow,
  };
  if (resume_on_budget && !(ram_load || ram_store))
    return false;
  uintptr_t callback = (uintptr_t)gpsp_rv32_thumb_memory_helpers[kind];
  uintptr_t bridge = (uintptr_t)gpsp_rv32_call_helper;
  if (!store)
  {
    static void (*const rom_loads[5])(void) =
    {
      gpsp_rv32_rom_load_u8, gpsp_rv32_rom_load_u16,
      gpsp_rv32_rom_load_u32, gpsp_rv32_rom_load_s8,
      gpsp_rv32_rom_load_s16,
    };
    bridge = (uintptr_t)rom_loads[kind];
  }
  uintptr_t target = ram_load ? (uintptr_t)ram_loads[kind] :
      ram_store ? (uintptr_t)ram_stores[kind - GPSP_RV32_MEM_STORE_U8] :
                  bridge;
  if (resume_on_budget)
    target = ram_load ? (uintptr_t)flow_loads[kind] :
        (uintptr_t)flow_stores[kind - GPSP_RV32_MEM_STORE_U8];
  uintptr_t wait_addr = (uintptr_t)&ws_cyc_seq[((pc + 2) >> 24) & 15][0];
  if (callback > UINT32_MAX || target > UINT32_MAX ||
      (ram_load || ram_store ? wait_addr > UINT32_MAX : false) ||
      (!(ram_load || ram_store) &&
       !emit_constant(code, A0, (uint32_t)callback)))
    return false;
  if (!emit_absolute_call(code, ram_load || ram_store ? RV_TMP0 : RA,
          RV_TMP1, (uint32_t)target) ||
      (retry && !gpsp_rv32_fixed_end(code)))
    return false;
  if (!(ram_load || ram_store))
  {
    uint8_t *skip = code->ptr;
    if (!gpsp_rv32_beq(code, A1, X0, 0) || !emit_return(code) ||
        !gpsp_rv32_patch_branch(code, skip, code->ptr))
      return false;
  }
  if (ram_load || ram_store)
  {
    /* The stub returns here after its data access; charge the fetch. */
    if (charge_fetch &&
        !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET,
            -(int)*(const uint8_t *)wait_addr))
      return false;
    *advanced = true;
  }
  return store || gpsp_rv32_addi(code, guest_reg_map[rd], A0, 0);
}

static bool emit_thumb_memory(gpsp_rv32_code *code, uint32_t pc,
    uint16_t opcode, bool resume_on_budget, bool charge_fetch,
    bool *advanced)
{
  gpsp_rv32_code_mark mark = gpsp_rv32_code_save(code);
  if (emit_thumb_memory_code(code, pc, opcode, resume_on_budget,
          charge_fetch, advanced))
    return true;
  gpsp_rv32_code_restore(code, &mark);
  return false;
}

bool gpsp_rv32_emit_block_instruction(uint8_t **translation_ptr,
    uint8_t *translation_limit, uint32_t pc, uint32_t opcode, bool thumb,
    bool allow_native, uint8_t needed_flags, bool batch_timing,
    bool *ends_block, gpsp_rv32_block_link *link)
{
  if (link)
    memset(link, 0, sizeof(*link));
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      !ends_block || ((uintptr_t)*translation_ptr & 3) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit ||
      (thumb ? (pc & 1) || opcode > UINT16_MAX : (pc & 3)) ||
      (batch_timing && (!thumb || !allow_native ||
          !gpsp_rv32_thumb_batch_alu((uint16_t)opcode))))
    return false;

  uint32_t words[256];
  gpsp_rv32_code staged;
  gpsp_rv32_code_init(&staged, words, sizeof(words));
  bool memory_guard_in_helper = thumb && allow_native &&
      is_thumb_memory((uint16_t)opcode) &&
      (opcode & 0xf800) != 0x4800 && pc + 2 != idle_loop_target_pc;
  if (!batch_timing && !memory_guard_in_helper && !emit_guard(&staged))
    return false;
  gpsp_rv32_code_mark body = gpsp_rv32_code_save(&staged);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  size_t guard_bytes = (size_t)(body.ptr - staged.start);
  size_t timing_bytes = 0;
  size_t link_exit_bytes = 0;
  uint8_t *phase_start;
#endif
  bool native = false;
  bool native_branch = false;
  bool native_exit = false;
  bool memory_advanced = false;
  bool multi_emitted = false;
  bool multi_exits = false;
  bool conditional_branch = false;
  bool shared_thumb_branch_wait = false;
  bool arm_bx = false;
  bool arm_pc_exit = false;
  bool arm_system_exit = false;
  bool thumb_target_ready = false;
  gpsp_rv32_arm_condition_skip arm_skip = { { NULL, NULL }, 0 };
  uint32_t branch_target = 0;
  uint8_t *link_site = NULL;

  if (allow_native && thumb &&
      gpsp_rv32_thumb_multi_native((uint16_t)opcode))
  {
    if (!emit_thumb_multi(&staged, pc, (uint16_t)opcode, &multi_exits))
      return false;
    multi_emitted = true;
    native = true;
  }
  else if (allow_native && thumb)
  {
    if ((opcode & 0xff00u) == 0xdf00u)
    {
      gpsp_rv32_arm_system_result system =
          gpsp_rv32_emit_thumb_swi(&staged, (uint16_t)opcode, pc);
      if (system == GPSP_RV32_ARM_SYSTEM_BUFFER_FULL ||
          system == GPSP_RV32_ARM_SYSTEM_UNSUPPORTED)
        return false;
      native = system == GPSP_RV32_ARM_SYSTEM_EMITTED;
      arm_system_exit = system == GPSP_RV32_ARM_SYSTEM_EXIT;
      native_exit = arm_system_exit;
    }
    else
    {
      bool conditional = (opcode & 0xf000) == 0xd000 &&
          ((opcode >> 8) & 15) < 14;
      gpsp_rv32_thumb_result result;
      if (conditional)
      {
        uint32_t taken = thumb_branch_target(pc, (uint16_t)opcode, true);
        /* Both paths use the same wait entry within one fetch region. */
        shared_thumb_branch_wait = (taken >> 24) == ((pc + 2) >> 24) &&
            taken != idle_loop_target_pc &&
            pc + 2 != idle_loop_target_pc;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
        phase_start = staged.ptr;
#endif
        if (shared_thumb_branch_wait &&
            !emit_branch_wait(&staged, taken, true))
          return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
        timing_bytes += (size_t)(staged.ptr - phase_start);
#endif
        result = gpsp_rv32_emit_arm_condition_skip(&staged,
            (opcode >> 8) & 15, &arm_skip) ? GPSP_RV32_THUMB_EXIT :
                GPSP_RV32_THUMB_BUFFER_FULL;
      }
      else if ((opcode & 0xf800) == 0xe000)
        result = GPSP_RV32_THUMB_EXIT;
      else if (!batch_timing && (opcode & 0xf800) == 0xa000)
        result = emit_thumb_pc_add(&staged, pc, (uint16_t)opcode) ?
            GPSP_RV32_THUMB_EMITTED : GPSP_RV32_THUMB_BUFFER_FULL;
      else if ((opcode & 0xf800) == 0xf800 ||
          ((opcode & 0xfc00) == 0x4400 &&
           ((opcode >> 8) & 3) != 1 &&
           ((opcode >> 8) & 3) != 3 &&
           (((opcode >> 4) & 8) | (opcode & 7)) == 15))
      {
        result = emit_thumb_dynamic_target(&staged, (uint16_t)opcode) &&
            ((opcode & 0xf800) != 0xf800 ||
             emit_constant(&staged, RV_GUEST_LR, (pc + 2) | 1)) ?
            GPSP_RV32_THUMB_EXIT : GPSP_RV32_THUMB_BUFFER_FULL;
        thumb_target_ready = true;
      }
      else
        result = gpsp_rv32_emit_thumb(&staged,
            (uint16_t)opcode, pc, needed_flags);
      if (result == GPSP_RV32_THUMB_BUFFER_FULL)
        return false;
      native = result == GPSP_RV32_THUMB_EMITTED;
      native_branch = result == GPSP_RV32_THUMB_EXIT &&
          (conditional || (opcode & 0xf800) == 0xe000);
      native_exit = result == GPSP_RV32_THUMB_EXIT && !native_branch;
      if (result == GPSP_RV32_THUMB_NEEDS_HELPER &&
          is_thumb_memory((uint16_t)opcode))
      {
        if (!emit_thumb_memory(&staged, pc, (uint16_t)opcode,
                false, true, &memory_advanced))
          return false;
        native = true;
      }
      if (native_branch)
      {
        conditional_branch = conditional;
        branch_target = thumb_branch_target(pc, (uint16_t)opcode,
            conditional_branch);
      }
    }
  }
  else if (allow_native)
  {
    gpsp_rv32_arm_system_result system =
        gpsp_rv32_emit_arm_system(&staged, opcode, pc);
    if (system == GPSP_RV32_ARM_SYSTEM_BUFFER_FULL)
      return false;
    native = system == GPSP_RV32_ARM_SYSTEM_EMITTED;
    arm_system_exit = system == GPSP_RV32_ARM_SYSTEM_EXIT;
    native_exit = arm_system_exit;
    if (!native && !native_exit)
    {
      gpsp_rv32_arm_alu_result alu =
          gpsp_rv32_emit_arm_alu_block(&staged, opcode, pc);
      if (alu == GPSP_RV32_ARM_ALU_BUFFER_FULL)
        return false;
      native = alu == GPSP_RV32_ARM_ALU_EMITTED;
      arm_pc_exit = alu == GPSP_RV32_ARM_ALU_EXIT;
      native_exit = arm_pc_exit;
    }
    if (!native && !native_exit)
    {
      gpsp_rv32_arm_mul_result mul =
          gpsp_rv32_emit_arm_mul(&staged, opcode, pc);
      if (mul == GPSP_RV32_ARM_MUL_BUFFER_FULL)
        return false;
      native = mul == GPSP_RV32_ARM_MUL_EMITTED;
    }
    if (!native && !native_exit)
    {
      gpsp_rv32_arm_memory_result memory =
          gpsp_rv32_emit_arm_memory(&staged, opcode, pc);
      if (memory == GPSP_RV32_ARM_MEMORY_BUFFER_FULL)
        return false;
      native = memory == GPSP_RV32_ARM_MEMORY_EMITTED;
      arm_pc_exit = memory == GPSP_RV32_ARM_MEMORY_PC_EXIT;
      native_exit = arm_pc_exit;
    }
    arm_bx = (opcode & 0x0ffffff0) == 0x012fff10;
    if (!native && !native_exit &&
        ((opcode & 0x0e000000) == 0x0a000000 || arm_bx) &&
        (opcode >> 28) != 15)
    {
      conditional_branch = (opcode >> 28) != 14;
      if (conditional_branch &&
          !gpsp_rv32_emit_arm_condition_skip(&staged, opcode >> 28,
              &arm_skip))
        return false;
      gpsp_rv32_arm_branch_result result = GPSP_RV32_ARM_BRANCH_EXIT;
      if (arm_bx)
        result = gpsp_rv32_emit_arm_branch_taken(&staged, opcode, pc);
      else if ((opcode & 0x01000000) &&
          !emit_constant(&staged, RV_GUEST_LR, pc + 4))
        result = GPSP_RV32_ARM_BRANCH_BUFFER_FULL;
      if (result == GPSP_RV32_ARM_BRANCH_BUFFER_FULL)
        return false;
      native_branch = result == GPSP_RV32_ARM_BRANCH_EXIT && !arm_bx;
      native_exit = result == GPSP_RV32_ARM_BRANCH_EXIT && arm_bx;
      if (native_branch || native_exit)
      {
        if (native_branch)
          branch_target = arm_branch_target(pc, opcode);
      }
    }
  }

  if (batch_timing && !native)
    return false;
  if (memory_guard_in_helper && !memory_advanced)
    return false;

  if (multi_emitted)
  {
    /* Both paths finish the instruction here. */
  }
  else if (native_branch)
  {
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    phase_start = staged.ptr;
#endif
    if (!(thumb && conditional_branch ?
          (shared_thumb_branch_wait ?
           gpsp_rv32_addi(&staged, RV_PC, RV_PC,
               (int32_t)(branch_target - pc)) :
           emit_thumb_bcc_advance(&staged, pc, branch_target)) :
          emit_branch_advance(&staged, pc, branch_target, thumb)))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    timing_bytes += (size_t)(staged.ptr - phase_start);
    phase_start = staged.ptr;
#endif
    if (!gpsp_rv32_emit_link_slot(&staged, &link_site))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    link_exit_bytes += (size_t)(staged.ptr - phase_start);
    phase_start = staged.ptr;
#endif
    if (conditional_branch)
    {
      /* gpSP charges Bcc's extra Thumb fetch even when not taken. */
      if (!gpsp_rv32_patch_arm_condition_skip(&staged, &arm_skip,
              staged.ptr) ||
          !(thumb ? (shared_thumb_branch_wait ?
                    gpsp_rv32_addi(&staged, RV_PC, RV_PC, 2) :
                    emit_thumb_bcc_advance(&staged, pc, pc + 2)) :
                     emit_advance(&staged, pc, false)))
        return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
      timing_bytes += (size_t)(staged.ptr - phase_start);
#endif
    }
  }
  else if (native_exit)
  {
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    phase_start = staged.ptr;
#endif
    if (!arm_system_exit &&
        !(thumb ? emit_dynamic_thumb_exit(&staged, (uint16_t)opcode,
              thumb_target_ready) :
          arm_pc_exit ? emit_dynamic_arm_pc_exit(&staged, pc) :
                         emit_dynamic_arm_bx(&staged)))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    link_exit_bytes += (size_t)(staged.ptr - phase_start);
    phase_start = staged.ptr;
#endif
    if (arm_bx && conditional_branch &&
        (!gpsp_rv32_patch_arm_condition_skip(&staged, &arm_skip,
            staged.ptr) || !emit_advance(&staged, pc, false)))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    if (arm_bx && conditional_branch)
    {
      timing_bytes += (size_t)(staged.ptr - phase_start);
      phase_start = staged.ptr;
    }
#endif
    if (arm_bx && conditional_branch && !emit_return(&staged))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    if (arm_bx && conditional_branch)
      link_exit_bytes += (size_t)(staged.ptr - phase_start);
#endif
  }
  else if (native)
  {
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    phase_start = staged.ptr;
#endif
    if (!batch_timing && !memory_advanced &&
        !emit_advance(&staged, pc, thumb))
      return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    timing_bytes += (size_t)(staged.ptr - phase_start);
#endif
  }
  else
  {
    /* Unsupported memory and system instructions use the interpreter. */
    gpsp_rv32_code_restore(&staged, &body);
    if (gpsp_rv32_emit_interpreter_step_stub(&staged,
            (uintptr_t)gpsp_rv32_interpreter_step_stub) !=
        GPSP_RV32_FALLBACK_EMITTED)
      return false;
  }

  size_t size = (size_t)(staged.ptr - staged.start);
  if ((uintptr_t)translation_limit - (uintptr_t)*translation_ptr < size)
    return false;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
  emit_count_bytes(guard, guard_bytes);
  emit_count_bytes(timing, timing_bytes);
  emit_count_bytes(link_exit, link_exit_bytes);
  emit_count_bytes(body,
      size - guard_bytes - timing_bytes - link_exit_bytes);
#endif
  uint8_t *output = *translation_ptr;
  memcpy(*translation_ptr, words, size);
  *translation_ptr += size;
  *ends_block = multi_exits || native_exit ||
      (!native && !(native_branch && conditional_branch));
  if (link_site && link)
  {
    link->site = output + (link_site - staged.start);
    link->target = branch_target;
    link->thumb = thumb;
  }
  return true;
}

bool gpsp_rv32_emit_block_return(uint8_t **translation_ptr,
    uint8_t *translation_limit)
{
  if (!translation_ptr || !*translation_ptr || !translation_limit ||
      ((uintptr_t)*translation_ptr & 1) ||
      (uintptr_t)*translation_ptr > (uintptr_t)translation_limit)
    return false;

  gpsp_rv32_code code;
  gpsp_rv32_code_init(&code, *translation_ptr,
      (uintptr_t)translation_limit - (uintptr_t)*translation_ptr);
  if (!emit_return(&code))
    return false;
  emit_count_bytes(link_exit, (size_t)(code.ptr - *translation_ptr));
  *translation_ptr = code.ptr;
  return true;
}

bool gpsp_rv32_emit_thumb_memory_body(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, bool resume_on_budget,
    bool *advanced)
{
  return emit_thumb_memory(code, pc, opcode, resume_on_budget, true,
      advanced);
}

bool gpsp_rv32_emit_thumb_memory_head(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode)
{
  bool advanced = false;
  return emit_thumb_memory(code, pc, opcode, true, false, &advanced) &&
      advanced;
}

bool gpsp_rv32_emit_thumb_multi_body(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, bool *exits)
{
  return emit_thumb_multi(code, pc, opcode, exits);
}

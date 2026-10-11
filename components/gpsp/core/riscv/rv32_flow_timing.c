/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_flow_timing.h"

#include "rv32_register_map.h"

#include <limits.h>

extern uint8_t ws_cyc_seq[16][2];
extern uint8_t ws_cyc_nseq[16][2];
extern const uint32_t def_seq_cycles[16][2];
extern uint32_t idle_loop_target_pc;

extern void gpsp_rv32_thumb_bl_exit_shared(void);
extern void gpsp_rv32_thumb_bx_exit_shared(void);
extern void gpsp_rv32_arm_bx_exit_shared(void);
extern void gpsp_rv32_arm_pc_exit_shared(void);

static int address_low(uint32_t address)
{
  int low = address & 0xfff;
  return low >= 0x800 ? low - 0x1000 : low;
}

static uint32_t address_upper(uint32_t address)
{
  return ((address + 0x800u) >> 12) & 0xfffff;
}

static bool emit_constant(gpsp_rv32_code *code, unsigned rd,
    uint32_t value)
{
  uint32_t upper = address_upper(value);
  int low = address_low(value);
  return (upper ? gpsp_rv32_lui(code, rd, upper) :
          gpsp_rv32_addi(code, rd, X0, low)) &&
      (!upper || !low || gpsp_rv32_addi(code, rd, rd, low));
}

static bool emit_shared_exit(gpsp_rv32_code *code, uintptr_t target)
{
  if (target > UINT32_MAX)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  uint32_t address = (uint32_t)target;
  if (gpsp_rv32_lui(code, RV_TMP1, address_upper(address)) &&
      gpsp_rv32_jalr(code, X0, RV_TMP1, address_low(address)))
    return true;
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

static bool emit_absolute_load(gpsp_rv32_code *code, unsigned rd,
    unsigned scratch, uint32_t address, bool word)
{
  uint32_t upper = address_upper(address);
  if (upper && !gpsp_rv32_lui(code, scratch, upper))
    return false;
  return word ? gpsp_rv32_lw(code, rd, upper ? scratch : X0,
                    address_low(address)) :
                gpsp_rv32_lbu(code, rd, upper ? scratch : X0,
                    address_low(address));
}

static bool emit_indexed_wait_load(gpsp_rv32_code *code,
    uint32_t address)
{
  uint32_t upper = address_upper(address);
#if GPSP_RV32_USE_ZBA
  /* RV_TMP0 holds the region; entries are two bytes apart. */
  return (!upper || gpsp_rv32_lui(code, RV_TMP1, upper)) &&
      gpsp_rv32_sh1add(code, RV_TMP1, RV_TMP0, upper ? RV_TMP1 : X0) &&
      gpsp_rv32_lbu(code, RV_TMP1, RV_TMP1, address_low(address));
#else
  if (upper && (!gpsp_rv32_lui(code, RV_TMP1, upper) ||
      !gpsp_rv32_add(code, RV_TMP1, RV_TMP1, RV_TMP0)))
    return false;
  return gpsp_rv32_lbu(code, RV_TMP1, upper ? RV_TMP1 : RV_TMP0,
      address_low(address));
#endif
}

static unsigned fixed_fetch_wait(unsigned region, bool thumb)
{
  if (region == 0 || region == 3)
    return 1;
  if (region == 2)
    return thumb ? 3 : 6;
  return 0;
}

static unsigned captured_fetch_wait(unsigned region, bool thumb)
{
  unsigned width = thumb ? 0 : 1;
  unsigned wait = ws_cyc_seq[region][width];
  return wait ? wait : def_seq_cycles[region][width];
}

unsigned gpsp_rv32_flow_fetch_bound(uint32_t next_pc, bool thumb)
{
  unsigned region = (next_pc >> 24) & 15;
  unsigned fixed = fixed_fetch_wait(region, thumb);
  if (fixed)
    return fixed;
  return captured_fetch_wait(region, thumb);
}

static bool fetch_region(uint32_t next_pc, unsigned count, bool thumb,
    unsigned *region)
{
  uint64_t last_pc = (uint64_t)next_pc +
      (uint64_t)(count - 1) * (thumb ? 2 : 4);
  if (!count || count > 32 || last_pc > UINT32_MAX ||
      (next_pc >> 24) != (last_pc >> 24))
    return false;
  *region = (next_pc >> 24) & 15;
  return true;
}

static bool emit_fetch_cost(gpsp_rv32_code *code, uint32_t next_pc,
    unsigned count, bool thumb)
{
  unsigned region;
  if (!fetch_region(next_pc, count, thumb, &region))
    return false;
  unsigned fixed = fixed_fetch_wait(region, thumb);
  if (fixed)
    return gpsp_rv32_addi(code, RV_TMP0, X0, fixed * count);
  return gpsp_rv32_addi(code, RV_TMP0, X0,
      captured_fetch_wait(region, thumb) * count);
}

bool gpsp_rv32_flow_guard(gpsp_rv32_code *code)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  uint8_t *skip = code->ptr;
  if (gpsp_rv32_blt(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_jalr(code, A1, RA, 4) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr))
    return true;
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

bool gpsp_rv32_flow_charge_seq(gpsp_rv32_code *code,
    uint32_t next_pc, unsigned count, bool thumb)
{
  if (!code)
    return false;
  unsigned region;
  if (!fetch_region(next_pc, count, thumb, &region))
    return false;
  unsigned fixed = fixed_fetch_wait(region, thumb);
  if (fixed)
    return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)(fixed * count));
  return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET,
      -(int)(captured_fetch_wait(region, thumb) * count));
}

bool gpsp_rv32_flow_preflight_seq(gpsp_rv32_code *code,
    uint32_t next_pc, unsigned count, bool thumb, uint8_t **cold_site)
{
  if (!code || !cold_site)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  if (!gpsp_rv32_flow_guard(code) ||
      !emit_fetch_cost(code, next_pc, count, thumb))
    goto failed;
  uint8_t *skip = code->ptr;
  if (!gpsp_rv32_bge(code, RV_BUDGET, RV_TMP0, 0))
    goto failed;
  uint8_t *site = code->ptr;
  if (!gpsp_rv32_jal(code, X0, 0) ||
      !gpsp_rv32_patch_branch(code, skip, code->ptr) ||
      !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP0))
    goto failed;
  *cold_site = site;
  return true;

failed:
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

/* At the idle loop target a positive budget ends. */
static bool emit_idle_clamp(gpsp_rv32_code *code)
{
  uint8_t *skip = code->ptr;
  return gpsp_rv32_bge(code, X0, RV_BUDGET, 0) &&
      gpsp_rv32_addi(code, RV_BUDGET, X0, 0) &&
      gpsp_rv32_patch_branch(code, skip, code->ptr);
}

static bool emit_idle(gpsp_rv32_code *code, uint32_t next_pc)
{
  if (next_pc != idle_loop_target_pc)
    return true;
  return emit_idle_clamp(code);
}

bool gpsp_rv32_flow_advance_pc(gpsp_rv32_code *code,
    uint32_t pc, unsigned count, bool thumb)
{
  if (!code || !count || count > 32)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  unsigned bytes = count * (thumb ? 2 : 4);
  uint32_t next_pc = pc + bytes;
  if (gpsp_rv32_addi(code, RV_PC, RV_PC, bytes) &&
      emit_idle(code, next_pc))
    return true;
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

bool gpsp_rv32_flow_advance(gpsp_rv32_code *code,
    uint32_t pc, bool thumb)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  uint32_t next_pc = pc + (thumb ? 2 : 4);
  if (gpsp_rv32_addi(code, RV_PC, RV_PC, thumb ? 2 : 4) &&
      gpsp_rv32_flow_charge_seq(code, next_pc, 1, thumb) &&
      emit_idle(code, next_pc))
    return true;
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

bool gpsp_rv32_flow_literal_body(gpsp_rv32_code *code,
    uint16_t opcode, uint32_t value)
{
  if (!code || (opcode & 0xf800) != 0x4800)
    return false;
  return emit_constant(code, guest_reg_map[(opcode >> 8) & 7], value);
}

bool gpsp_rv32_flow_literal_constant(gpsp_rv32_code *code,
    uint32_t pc, uint16_t opcode, uint32_t value, bool guard_entry)
{
  if (!code || (pc & 1) || (pc >> 24) < 8 ||
      (pc >> 24) > 12 || (opcode & 0xf800) != 0x4800)
    return false;

  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  if ((guard_entry && !gpsp_rv32_flow_guard(code)) ||
      !gpsp_rv32_flow_literal_body(code, opcode, value))
    goto failed;

  /* Charge the data access and the next fetch together. */
  uint32_t next_pc = pc + 2;
  unsigned region;
  if (!fetch_region(next_pc, 1, true, &region))
    goto failed;
  unsigned wait = fixed_fetch_wait(region, true);
  if (!wait)
    wait = captured_fetch_wait(region, true);
  wait += ws_cyc_nseq[pc >> 24][1];
  if (!gpsp_rv32_addi(code, RV_PC, RV_PC, 2) ||
      (wait && !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)wait)) ||
      !emit_idle(code, next_pc))
    goto failed;
  return true;

failed:
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

static bool emit_branch_wait(gpsp_rv32_code *code,
    uint32_t target, bool thumb)
{
  unsigned region = (target >> 24) & 15;
  unsigned fixed = fixed_fetch_wait(region, thumb);
  if (fixed)
    return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)(fixed * 2)) &&
        emit_idle(code, target);

  unsigned width = thumb ? 0 : 1;
  unsigned wait = ws_cyc_nseq[region][width] +
      captured_fetch_wait(region, thumb);
  return gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)wait) &&
      emit_idle(code, target);
}

bool gpsp_rv32_flow_branch_advance(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target, bool thumb)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  int32_t delta = (int32_t)(target - pc);
  if ((delta >= -2048 && delta <= 2047 ?
       gpsp_rv32_addi(code, RV_PC, RV_PC, delta) :
       emit_constant(code, RV_PC, target)) &&
      emit_branch_wait(code, target, thumb))
    return true;
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

bool gpsp_rv32_flow_set_branch_pc(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target)
{
  if (!code)
    return false;
  int32_t delta = (int32_t)(target - pc);
  return delta >= -2048 && delta <= 2047 ?
      gpsp_rv32_addi(code, RV_PC, RV_PC, delta) :
      emit_constant(code, RV_PC, target);
}

bool gpsp_rv32_flow_bl_pair_advance(gpsp_rv32_code *code,
    uint32_t pc, uint32_t target)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  unsigned cost = gpsp_rv32_flow_fetch_bound(pc + 2, true) +
      ws_cyc_nseq[(target >> 24) & 15][0] +
      gpsp_rv32_flow_fetch_bound(target, true);
  if (cost > 48 ||
      !gpsp_rv32_addi(code, RV_BUDGET, RV_BUDGET, -(int)cost) ||
      !gpsp_rv32_flow_set_branch_pc(code, pc, target) ||
      !emit_idle(code, target))
    goto failed;
  return true;

failed:
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

static bool emit_dynamic_wait(gpsp_rv32_code *code, bool thumb,
    bool nonsequential, bool sequential)
{
  uintptr_t nseq = (uintptr_t)ws_cyc_nseq;
  uintptr_t seq = (uintptr_t)ws_cyc_seq;
  if (nseq > UINT32_MAX || seq > UINT32_MAX ||
      !gpsp_rv32_srli(code, RV_TMP0, RV_PC, 24) ||
      !gpsp_rv32_andi(code, RV_TMP0, RV_TMP0, 15) ||
      (!GPSP_RV32_USE_ZBA && !gpsp_rv32_slli(code, RV_TMP0, RV_TMP0, 1)))
    return false;
  if (nonsequential &&
      (!emit_indexed_wait_load(code,
          (uint32_t)nseq + (thumb ? 0 : 1)) ||
       !gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1)))
    return false;
  return !sequential ||
      (emit_indexed_wait_load(code,
           (uint32_t)seq + (thumb ? 0 : 1)) &&
       gpsp_rv32_sub(code, RV_BUDGET, RV_BUDGET, RV_TMP1));
}

static bool emit_dynamic_idle(gpsp_rv32_code *code)
{
  uintptr_t address = (uintptr_t)&idle_loop_target_pc;
  if (address > UINT32_MAX ||
      !emit_absolute_load(code, RV_TMP0, RV_TMP0, (uint32_t)address, true))
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

bool gpsp_rv32_flow_dynamic_thumb_exit(gpsp_rv32_code *code,
    uint16_t opcode, bool target_ready)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  bool bx = (opcode & 0xff00) == 0x4700;
  bool bl_second = (opcode & 0xf800) == 0xf800;
  if (!target_ready && (bx || bl_second))
    return emit_shared_exit(code,
        (uintptr_t)(bx ? gpsp_rv32_thumb_bx_exit_shared :
                        gpsp_rv32_thumb_bl_exit_shared));
  if ((!target_ready && !gpsp_rv32_addi(code, RV_PC, A0, 0)) ||
      (bx && !emit_thumb_state(code)))
    goto failed;

  uint8_t *arm_bx = NULL;
  if (bx)
  {
    arm_bx = code->ptr;
    if (!gpsp_rv32_beq(code, A1, X0, 0))
      goto failed;
  }
  if (!emit_dynamic_wait(code, true, bl_second, true) ||
      !emit_dynamic_idle(code) ||
      (arm_bx && !gpsp_rv32_patch_branch(code, arm_bx, code->ptr)) ||
      !gpsp_rv32_jalr(code, X0, RA, 0))
    goto failed;
  return true;

failed:
  gpsp_rv32_code_restore(code, &begin);
  return false;
}

bool gpsp_rv32_flow_dynamic_arm_bx(gpsp_rv32_code *code)
{
  if (!code)
    return false;
  return emit_shared_exit(code, (uintptr_t)gpsp_rv32_arm_bx_exit_shared);
}

bool gpsp_rv32_flow_dynamic_arm_pc_exit(gpsp_rv32_code *code,
    uint32_t pc, bool fall_through)
{
  if (!code)
    return false;
  gpsp_rv32_code_mark begin = gpsp_rv32_code_save(code);
  uint8_t *not_taken = code->ptr;
  if (!gpsp_rv32_beq(code, A1, X0, 0) ||
      !emit_shared_exit(code, (uintptr_t)gpsp_rv32_arm_pc_exit_shared) ||
      !gpsp_rv32_patch_branch(code, not_taken, code->ptr) ||
      !gpsp_rv32_flow_advance(code, pc, false) ||
      (!fall_through && !gpsp_rv32_jalr(code, X0, RA, 0)))
  {
    gpsp_rv32_code_restore(code, &begin);
    return false;
  }
  return true;
}

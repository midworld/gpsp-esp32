/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Run with make -C tests check-cpu from the core root.
 */
#include <assert.h>
#include "../cpu.cc"

static const u32 code_base = 0x06000000;
static u32 code_words[8192];
static unsigned update_count;

extern "C" {
u8 ws_cyc_seq[16][2];
u8 ws_cyc_nseq[16][2];
u32 gamepak_sticky_bit[1024/32];
u32 cheat_master_hook = 0xFFFFFFFF;
u32 idle_loop_target_pc = 0xFFFFFFFF;

u8 *load_gamepak_page(u32) { assert(false); return NULL; }
void process_cheats(void) { assert(false); }
u32 update_gba(int remaining_cycles)
{
  assert(remaining_cycles == 0);
  update_count++;
  return 0x80000000;
}

u32 read_memory8(u32) { assert(false); return 0; }
u32 read_memory16(u32) { assert(false); return 0; }
u16 read_memory16_signed(u32) { assert(false); return 0; }
u32 read_memory32(u32) { assert(false); return 0; }
cpu_alert_type write_memory8(u32, u8) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory16(u32, u16) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory32(u32, u32) { assert(false); return CPU_ALERT_NONE; }
}

static void reset_cpu(u32 opcode, bool thumb, u32 flags)
{
  memset(reg, 0, sizeof(reg));
  memset(reg_mode, 0, sizeof(reg_mode));
  memset(spsr, 0, sizeof(spsr));
  memset(code_words, 0, sizeof(code_words));
  memset(memory_map_read, 0, sizeof(memory_map_read));
  memset(ws_cyc_seq, 0, sizeof(ws_cyc_seq));
  memset(ws_cyc_nseq, 0, sizeof(ws_cyc_nseq));
  memset(gamepak_sticky_bit, 0, sizeof(gamepak_sticky_bit));
  code_words[0] = opcode;
  memory_map_read[code_base >> 15] = (u8 *)code_words;
  ws_cyc_seq[6][thumb ? 0 : 1] = 1;
  reg[REG_PC] = code_base;
  reg[REG_CPSR] = flags | 0x1F | (thumb ? 0x20 : 0);
  reg[CPU_MODE] = MODE_SYSTEM;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  update_count = 0;
}

static void run_one(u32 value, u32 expected, u32 flags, bool thumb)
{
  reg[0] = value;
  assert(execute_arm_internal<false>(1, true) == 0x80000000);
  assert(update_count == 1);
  assert(reg[0] == expected);
  assert((reg[REG_CPSR] & 0xF0000000) == flags);
  assert(reg[REG_PC] == code_base + (thumb ? 2 : 4));
}

static u32 arm_ror(bool set_flags)
{
  /* MOV{S} r0,r1,ROR r2 */
  return 0xE1A00071 | ((u32)set_flags << 20) | (2U << 8);
}

static u32 thumb_shift_opcode(u32 operation)
{
  /* Shift r0 by r1 in the Thumb ALU group. */
  return 0x4000 | (operation << 6) | (1U << 3);
}

int main()
{
  reset_cpu(arm_ror(false), false, 0x10000000);
  reg[1] = 0x81234567;
  reg[2] = 0x120;
  run_one(0, 0x81234567, 0x10000000, false);

  reset_cpu(arm_ror(true), false, 0);
  reg[1] = 0x81234567;
  reg[2] = 0x120;
  run_one(0, 0x81234567, 0xA0000000, false);

  reset_cpu(arm_ror(true), false, 0);
  reg[1] = 0x81234567;
  reg[2] = 0x121;
  run_one(0, 0xC091A2B3, 0xA0000000, false);

  reset_cpu(arm_ror(true), false, 0x10000000);
  reg[1] = 0x81234567;
  reg[2] = 0x100;
  run_one(0, 0x81234567, 0x90000000, false);

  reset_cpu(thumb_shift_opcode(2), true, 0x30000000);
  reg[1] = 0x100;
  run_one(1, 1, 0x30000000, true);

  reset_cpu(thumb_shift_opcode(3), true, 0);
  reg[1] = 0x120;
  run_one(0x80000000, 0, 0x60000000, true);

  reset_cpu(thumb_shift_opcode(4), true, 0);
  reg[1] = 0x120;
  run_one(0x80000000, 0xFFFFFFFF, 0xA0000000, true);

  reset_cpu(thumb_shift_opcode(7), true, 0);
  reg[1] = 0x120;
  run_one(0x81234567, 0x81234567, 0xA0000000, true);

  reset_cpu(thumb_shift_opcode(7), true, 0);
  reg[1] = 0x121;
  run_one(0x81234567, 0xC091A2B3, 0xA0000000, true);

  reset_cpu(thumb_shift_opcode(7), true, 0x10000000);
  reg[1] = 0x100;
  run_one(0x81234567, 0x81234567, 0x90000000, true);
  return 0;
}

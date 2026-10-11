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

u8 *load_gamepak_page(u32)
{
  assert(false);
  return NULL;
}

void process_cheats(void)
{
  assert(false);
}

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

static u32 data_proc_imm(u32 condition, u32 operation, bool set_flags,
                         u32 rn, u32 rd, u32 imm8, u32 rotate = 0)
{
  return (condition << 28) | (1U << 25) | (operation << 21) |
         ((u32)set_flags << 20) | (rn << 16) | (rd << 12) |
         (rotate << 8) | imm8;
}

static u32 data_proc_reg(u32 condition, u32 operation, bool set_flags,
                         u32 rn, u32 rd, u32 rm)
{
  return (condition << 28) | (operation << 21) |
         ((u32)set_flags << 20) | (rn << 16) | (rd << 12) | rm;
}

static void reset_cpu(u32 opcode, u32 initial_flags)
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
  ws_cyc_seq[6][1] = 1;
  reg[REG_PC] = code_base;
  reg[REG_CPSR] = initial_flags | 0x1F;
  reg[CPU_MODE] = MODE_SYSTEM;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  update_count = 0;
}

static void run_one()
{
  u32 result = execute_arm_internal<false>(1, true);
  assert(result == 0x80000000);
  assert(update_count == 1);
}

static void expect_flags(u32 expected)
{
  assert((reg[REG_CPSR] & 0xF0000000) == expected);
  assert((reg[REG_CPSR] & 0xFF) == 0x1F);
}

static void test_mov_and_pc()
{
  reset_cpu(data_proc_imm(14, 13, false, 0, 0, 7), 0xB0000000);
  run_one();
  assert(reg[0] == 7);
  assert(reg[REG_PC] == code_base + 4);
  expect_flags(0xB0000000);

  reset_cpu(data_proc_imm(14, 13, true, 0, 1, 0), 0xB0000000);
  run_one();
  assert(reg[1] == 0);
  expect_flags(0x70000000); // Z set, N clear, C and V preserved.

  reset_cpu(data_proc_reg(14, 13, false, 0, 0, REG_PC), 0);
  run_one();
  assert(reg[0] == code_base + 8);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(data_proc_imm(14, 4, false, REG_PC, 0, 4), 0);
  run_one();
  assert(reg[0] == code_base + 12);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(data_proc_reg(14, 13, false, 0, REG_PC, 0), 0);
  reg[0] = code_base + 0x100;
  run_one();
  assert(reg[REG_PC] == code_base + 0x100);
  expect_flags(0);
}

static void test_rotated_immediate_and_skipped_condition()
{
  // Rotate field 4 means ROR 8: 0x80 becomes 0x80000000.
  reset_cpu(data_proc_imm(14, 13, true, 0, 4, 0x80, 4), 0x10000000);
  run_one();
  assert(reg[4] == 0x80000000);
  assert(reg[REG_PC] == code_base + 4);
  expect_flags(0xB0000000); // Nonzero rotate copies result bit 31 into C.

  reset_cpu(data_proc_imm(0, 13, true, 0, 4, 0x80, 4), 0x10000000);
  reg[4] = 0xDEADBEEF;
  run_one();
  assert(reg[4] == 0xDEADBEEF);
  assert(reg[REG_PC] == code_base + 4);
  expect_flags(0x10000000); // Failed EQ preserves N, Z, C, V.
}

static void test_add_flags()
{
  reset_cpu(data_proc_imm(14, 4, false, 3, 2, 1), 0xB0000000);
  reg[3] = 0x7FFFFFFF;
  run_one();
  assert(reg[2] == 0x80000000);
  expect_flags(0xB0000000);

  reset_cpu(data_proc_imm(14, 4, true, 3, 2, 1), 0);
  reg[3] = 0x7FFFFFFF;
  run_one();
  assert(reg[2] == 0x80000000);
  expect_flags(0x90000000); // Signed overflow without unsigned carry.

  reset_cpu(data_proc_imm(14, 4, true, 3, 2, 1), 0);
  reg[3] = 0xFFFFFFFF;
  run_one();
  assert(reg[2] == 0);
  expect_flags(0x60000000); // Zero with unsigned carry.
}

static void test_sub_flags()
{
  reset_cpu(data_proc_imm(14, 2, false, 3, 2, 1), 0xB0000000);
  reg[3] = 0;
  run_one();
  assert(reg[2] == 0xFFFFFFFF);
  expect_flags(0xB0000000);

  reset_cpu(data_proc_imm(14, 2, true, 3, 2, 1), 0);
  reg[3] = 0;
  run_one();
  assert(reg[2] == 0xFFFFFFFF);
  expect_flags(0x80000000); // Borrow clears C.

  reset_cpu(data_proc_imm(14, 2, true, 3, 2, 1), 0);
  reg[3] = 0x80000000;
  run_one();
  assert(reg[2] == 0x7FFFFFFF);
  expect_flags(0x30000000); // No borrow, signed overflow.
}

static void test_cmp_and_conditions()
{
  reset_cpu(data_proc_imm(14, 10, true, 3, 0, 2), 0);
  reg[3] = 2;
  reg[0] = 0xDEADBEEF;
  run_one();
  assert(reg[3] == 2);
  assert(reg[0] == 0xDEADBEEF);
  expect_flags(0x60000000);

  reset_cpu(data_proc_imm(14, 10, true, 3, 0, 2), 0);
  reg[3] = 0;
  run_one();
  expect_flags(0x80000000);

  for (u32 condition = 0; condition <= 1; condition++) {
    for (u32 initial_z = 0; initial_z <= 1; initial_z++) {
      reset_cpu(data_proc_imm(condition, 13, true, 0, 0, 5),
                0x30000000 | (initial_z << 30));
      reg[0] = 0xDEADBEEF;
      run_one();
      bool executes = (condition == 0) ? initial_z : !initial_z;
      assert(reg[0] == (executes ? 5U : 0xDEADBEEFU));
      assert(reg[REG_PC] == code_base + 4);
      expect_flags(0x30000000 | ((!executes && initial_z) << 30));
    }
  }
}

int main()
{
  test_mov_and_pc();
  test_rotated_immediate_and_skipped_condition();
  test_add_flags();
  test_sub_flags();
  test_cmp_and_conditions();
  return 0;
}

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Run with make -C tests check-cpu from the core root.
 */
#include <assert.h>
#include "../cpu.cc"

static const u32 test_base = 0x03000000;
static u32 test_ram[8192];

extern "C" {
u8 ws_cyc_seq[16][2];

u32 read_memory32(u32 address)
{
  assert(address >= test_base && address < test_base + sizeof(test_ram));
  return test_ram[(address - test_base) / 4];
}

cpu_alert_type write_memory32(u32 address, u32 value)
{
  assert(address >= test_base && address < test_base + sizeof(test_ram));
  test_ram[(address - test_base) / 4] = value;
  return CPU_ALERT_NONE;
}
}

static void reset_cpu(cpu_mode_type mode)
{
  memset(reg, 0, sizeof(reg));
  memset(reg_mode, 0, sizeof(reg_mode));
  memset(spsr, 0, sizeof(spsr));
  memset(test_ram, 0, sizeof(test_ram));
  memset(memory_map_read, 0, sizeof(memory_map_read));
  memory_map_read[test_base >> 15] = (u8 *)test_ram;
  reg[CPU_MODE] = mode;
  reg[REG_CPSR] = (mode == MODE_IRQ) ? 0x12 : 0x11;
  reg[REG_PC] = 0x08000000;
}

static void test_irq_store_user_bank()
{
  reset_cpu(MODE_IRQ);
  reg[REG_SP] = test_base;
  reg[REG_LR] = 0x11111111;
  reg[0] = 0x22222222;
  REG_MODE(MODE_USER)[5] = 0x33333333;
  REG_MODE(MODE_USER)[6] = 0x44444444;

  // User-bank LDM/STM^ must leave W clear.
  s32 cycles = 100;
  exec_arm_block_mem<AccStore, false, true, AddrPostInc>(
    REG_SP, (1U << 0) | (1U << REG_SP) | (1U << REG_LR), cycles);

  assert(test_ram[0] == 0x22222222);
  assert(test_ram[1] == 0x33333333);
  assert(test_ram[2] == 0x44444444);
  assert(reg[REG_SP] == test_base);
  assert(reg[REG_LR] == 0x11111111);
  assert(REG_MODE(MODE_USER)[5] == 0x33333333);
  assert(reg[CPU_MODE] == MODE_IRQ);
}

static void test_irq_load_user_bank_without_pc()
{
  reset_cpu(MODE_IRQ);
  reg[REG_SP] = test_base;
  reg[REG_LR] = 0x11111111;
  REG_MODE(MODE_USER)[5] = 0x22222222;
  REG_MODE(MODE_USER)[6] = 0x33333333;
  test_ram[0] = 0x44444444;
  test_ram[1] = 0x55555555;
  test_ram[2] = 0x66666666;

  s32 cycles = 100;
  exec_arm_block_mem<AccLoad, false, true, AddrPostInc>(
    REG_SP, (1U << 0) | (1U << REG_SP) | (1U << REG_LR), cycles);

  assert(reg[0] == 0x44444444);
  assert(REG_MODE(MODE_USER)[5] == 0x55555555);
  assert(REG_MODE(MODE_USER)[6] == 0x66666666);
  assert(reg[REG_SP] == test_base);
  assert(reg[REG_LR] == 0x11111111);
  assert(reg[CPU_MODE] == MODE_IRQ);
}

static void test_irq_exception_return()
{
  reset_cpu(MODE_IRQ);
  reg[REG_SP] = test_base;
  reg[REG_LR] = 0x11111111;
  REG_MODE(MODE_USER)[5] = 0x22222222;
  REG_MODE(MODE_USER)[6] = 0x33333333;
  REG_SPSR(MODE_IRQ) = 0x10;
  test_ram[0] = 0x44444444;
  test_ram[1] = 0x55555554;

  s32 cycles = 100;
  exec_arm_block_mem<AccLoad, true, true, AddrPostInc>(
    REG_SP, (1U << REG_LR) | (1U << REG_PC), cycles);

  assert(reg[REG_SP] == test_base + 8);
  assert(reg[REG_LR] == 0x44444444);
  assert(reg[REG_PC] == 0x55555554);
  assert(REG_MODE(MODE_USER)[5] == 0x22222222);
  assert(REG_MODE(MODE_USER)[6] == 0x33333333);
  assert(reg[CPU_MODE] == MODE_IRQ);

  // The interpreter's caller restores SPSR after this transfer.
  reg[REG_CPSR] = REG_SPSR(MODE_IRQ);
  set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
  assert(reg[CPU_MODE] == MODE_USER);
  assert(reg[REG_SP] == 0x22222222);
  assert(reg[REG_LR] == 0x33333333);
  assert(REG_MODE(MODE_IRQ)[5] == test_base + 8);
  assert(REG_MODE(MODE_IRQ)[6] == 0x44444444);
}

static void test_fiq_user_bank_and_shared_registers()
{
  reset_cpu(MODE_IRQ);
  reg[8] = 0x11111111;
  reg[REG_SP] = 0x22222222;
  reg[REG_LR] = 0x33333333;
  REG_MODE(MODE_USER)[5] = 0x44444444;
  REG_MODE(MODE_USER)[6] = 0x55555555;
  REG_MODE(MODE_FIQ)[0] = 0x66666666;
  REG_MODE(MODE_FIQ)[5] = 0x77777777;
  REG_MODE(MODE_FIQ)[6] = 0x88888888;

  set_cpu_mode(MODE_FIQ);
  reg[REG_CPSR] = 0x11;
  assert(reg[8] == 0x66666666);
  assert(REG_MODE(MODE_USER)[0] == 0x11111111);
  reg[0] = test_base;

  s32 cycles = 100;
  exec_arm_block_mem<AccStore, false, true, AddrPostInc>(
    0, (1U << 8) | (1U << REG_SP) | (1U << REG_LR), cycles);
  assert(test_ram[0] == 0x11111111);
  assert(test_ram[1] == 0x44444444);
  assert(test_ram[2] == 0x55555555);
  assert(reg[8] == 0x66666666);
  assert(reg[REG_SP] == 0x77777777);
  assert(reg[0] == test_base);

  reg[0] = test_base;
  test_ram[0] = 0x99999999;
  test_ram[1] = 0xAAAAAAAA;
  test_ram[2] = 0xBBBBBBBB;
  exec_arm_block_mem<AccLoad, false, true, AddrPostInc>(
    0, (1U << 8) | (1U << REG_SP) | (1U << REG_LR), cycles);
  assert(REG_MODE(MODE_USER)[0] == 0x99999999);
  assert(REG_MODE(MODE_USER)[5] == 0xAAAAAAAA);
  assert(REG_MODE(MODE_USER)[6] == 0xBBBBBBBB);
  assert(reg[8] == 0x66666666);
  assert(reg[REG_SP] == 0x77777777);

  set_cpu_mode(MODE_USER);
  assert(reg[8] == 0x99999999);
  assert(reg[REG_SP] == 0xAAAAAAAA);
  assert(reg[REG_LR] == 0xBBBBBBBB);
  set_cpu_mode(MODE_IRQ);
  assert(reg[8] == 0x99999999);
  assert(reg[REG_SP] == 0x22222222);
}

static void test_fiq_exception_return_uses_current_bank()
{
  reset_cpu(MODE_FIQ);
  reg[0] = test_base;
  reg[8] = 0x11111111;
  REG_MODE(MODE_USER)[0] = 0x22222222;
  REG_SPSR(MODE_FIQ) = 0x10;
  test_ram[0] = 0x33333333;
  test_ram[1] = 0x08000100;

  s32 cycles = 100;
  exec_arm_block_mem<AccLoad, true, true, AddrPostInc>(
    0, (1U << 8) | (1U << REG_PC), cycles);
  assert(reg[8] == 0x33333333);
  assert(REG_MODE(MODE_USER)[0] == 0x22222222);
  assert(reg[0] == test_base + 8);

  reg[REG_CPSR] = REG_SPSR(MODE_FIQ);
  set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
  assert(reg[8] == 0x22222222);
  assert(REG_MODE(MODE_FIQ)[0] == 0x33333333);
}

static void test_base_first_store()
{
  reset_cpu(MODE_IRQ);
  reg[0] = test_base;
  reg[1] = 0x12345678;

  s32 cycles = 100;
  exec_arm_block_mem<AccStore, true, false, AddrPostInc>(
    0, (1U << 0) | (1U << 1), cycles);
  assert(test_ram[0] == test_base);
  assert(test_ram[1] == 0x12345678);
  assert(reg[0] == test_base + 8);
}

static void test_plain_load_uses_current_bank()
{
  reset_cpu(MODE_IRQ);
  reg[0] = test_base;
  reg[REG_SP] = 0x11111111;
  REG_MODE(MODE_USER)[5] = 0x22222222;
  test_ram[0] = 0x33333333;

  s32 cycles = 100;
  exec_arm_block_mem<AccLoad, true, false, AddrPostInc>(
    0, 1U << REG_SP, cycles);
  assert(reg[REG_SP] == 0x33333333);
  assert(REG_MODE(MODE_USER)[5] == 0x22222222);
  assert(reg[0] == test_base + 4);
}

static void test_stm_stores_pc_plus_12()
{
  reset_cpu(MODE_USER);
  reg[0] = test_base;
  s32 cycles = 100;
  exec_arm_block_mem<AccStore, false, false, AddrPostInc>(
    0, 1U << REG_PC, cycles);
  assert(test_ram[0] == 0x0800000c);
  assert(reg[REG_PC] == 0x08000004);
}

static void test_mode_banks()
{
  const cpu_mode_type modes[] = {
    MODE_USER, MODE_SYSTEM, MODE_IRQ, MODE_FIQ,
    MODE_SUPERVISOR, MODE_ABORT, MODE_UNDEFINED
  };
  const u32 cpsr_modes[] = {0x10, 0x1F, 0x12, 0x11, 0x13, 0x17, 0x1B};
  const u32 mode_count = sizeof(modes) / sizeof(modes[0]);
  for (u32 from = 0; from < mode_count; from++)
  {
    for (u32 to = 0; to < mode_count; to++)
    {
      if (from == to)
        continue;
      reset_cpu(modes[from]);
      reg[REG_CPSR] = cpsr_modes[from];
      for (u32 r = 8; r < 13; r++)
      {
        REG_MODE(MODE_USER)[r - 8] = 0x100 + r;
        REG_MODE(MODE_FIQ)[r - 8] = 0x200 + r;
        reg[r] = modes[from] == MODE_FIQ ? 0x200 + r : 0x100 + r;
      }
      for (u32 i = 0; i < mode_count; i++)
      {
        REG_MODE(modes[i])[5] = 0x1000 + (modes[i] & 0xF);
        REG_MODE(modes[i])[6] = 0x2000 + (modes[i] & 0xF);
      }
      reg[REG_SP] = REG_MODE(modes[from])[5];
      reg[REG_LR] = REG_MODE(modes[from])[6];
      reg[0] = 0x12345678;
      u32 cpsr = reg[REG_CPSR];

      set_cpu_mode(modes[to]);
      assert(reg[CPU_MODE] == modes[to]);
      for (u32 r = 8; r < 13; r++)
        assert(reg[r] == (modes[to] == MODE_FIQ ? 0x200 + r : 0x100 + r));
      assert(reg[REG_SP] == 0x1000 + (modes[to] & 0xF));
      assert(reg[REG_LR] == 0x2000 + (modes[to] & 0xF));
      for (u32 r = 8; r < 13; r++)
        reg[r] = 0x300 + 16 * to + r;
      reg[REG_SP] = 0x3000 + to;
      reg[REG_LR] = 0x4000 + to;

      set_cpu_mode(modes[from]);
      assert(reg[CPU_MODE] == modes[from]);
      for (u32 r = 8; r < 13; r++)
        assert(reg[r] == ((modes[from] == MODE_FIQ || modes[to] == MODE_FIQ) ?
            (modes[from] == MODE_FIQ ? 0x200 + r : 0x100 + r) :
            0x300 + 16 * to + r));
      // USER and SYSTEM share one SP/LR bank.
      bool same_bank = (modes[from] & 0xF) == (modes[to] & 0xF);
      assert(reg[REG_SP] == (same_bank ? 0x3000 + to :
          0x1000 + (modes[from] & 0xF)));
      assert(reg[REG_LR] == (same_bank ? 0x4000 + to :
          0x2000 + (modes[from] & 0xF)));

      set_cpu_mode(modes[to]);
      assert(reg[CPU_MODE] == modes[to]);
      for (u32 r = 8; r < 13; r++)
        assert(reg[r] == 0x300 + 16 * to + r);
      assert(reg[REG_SP] == 0x3000 + to);
      assert(reg[REG_LR] == 0x4000 + to);
      assert(reg[0] == 0x12345678);
      assert(reg[REG_PC] == 0x08000000);
      assert(reg[REG_CPSR] == cpsr);
    }
  }
}

int main()
{
  test_irq_store_user_bank();
  test_irq_load_user_bank_without_pc();
  test_irq_exception_return();
  test_fiq_user_bank_and_shared_registers();
  test_fiq_exception_return_uses_current_bank();
  test_base_first_store();
  test_plain_load_uses_current_bank();
  test_stm_stores_pc_plus_12();
  test_mode_banks();
  return 0;
}

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Run with make -C tests check-cpu from the core root.
 */
#include <assert.h>
#include "../cpu.cc"

static const u32 code_base = 0x06000000;
static const u32 data_base = 0x03000000;
static u32 code_words[8192];
static u32 data_words[8192];
static unsigned scheduler_calls;

extern "C" {
u8 ws_cyc_seq[16][2];
u8 ws_cyc_nseq[16][2];
u32 gamepak_sticky_bit[1024/32];
u32 cheat_master_hook = 0xFFFFFFFF;
u32 idle_loop_target_pc = 0xFFFFFFFF;

u8 *load_gamepak_page(u32) { assert(false); return NULL; }
void process_cheats(void) { assert(false); }
u32 update_gba(int) { scheduler_calls++; assert(false); return 0; }

u32 read_memory8(u32) { assert(false); return 0; }
u32 read_memory16(u32) { assert(false); return 0; }
u16 read_memory16_signed(u32) { assert(false); return 0; }
u32 read_memory32(u32 address)
{
  assert(address >= data_base && address < data_base + sizeof(data_words));
  return data_words[(address - data_base) / 4];
}
cpu_alert_type write_memory8(u32, u8) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory16(u32, u16) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory32(u32 address, u32 value)
{
  assert(address >= data_base && address < data_base + sizeof(data_words));
  data_words[(address - data_base) / 4] = value;
  return CPU_ALERT_NONE;
}
}

static void reset_cpu(u32 opcode, bool thumb)
{
  memset(reg, 0, sizeof(reg));
  memset(reg_mode, 0, sizeof(reg_mode));
  memset(spsr, 0, sizeof(spsr));
  memset(code_words, 0, sizeof(code_words));
  memset(data_words, 0, sizeof(data_words));
  memset(memory_map_read, 0, sizeof(memory_map_read));
  memset(ws_cyc_seq, 0, sizeof(ws_cyc_seq));
  memset(ws_cyc_nseq, 0, sizeof(ws_cyc_nseq));
  memset(gamepak_sticky_bit, 0, sizeof(gamepak_sticky_bit));
  code_words[0] = opcode;
  memory_map_read[code_base >> 15] = (u8 *)code_words;
  memory_map_read[data_base >> 15] = (u8 *)data_words;
  ws_cyc_seq[6][thumb ? 0 : 1] = 1;
  ws_cyc_nseq[6][thumb ? 0 : 1] = 1;
  ws_cyc_nseq[3][1] = 1;
  reg[REG_PC] = code_base;
  reg[REG_CPSR] = 0x1f | (thumb ? 0x20 : 0);
  reg[CPU_MODE] = MODE_SYSTEM;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  scheduler_calls = 0;
}

static void test_arm_alu_and_condition()
{
  reset_cpu(0xE2932001, false); /* ADDS r2,r3,#1 */
  reg[3] = 0x7fffffff;
  assert(execute_arm_one(100) == 99);
  assert(reg[2] == 0x80000000);
  assert(reg[REG_CPSR] == 0x9000001f);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0x03A04055, false); /* MOVEQ r4,#0x55, Z initially clear */
  reg[4] = 0xaa;
  assert(execute_arm_one(100) == 99);
  assert(reg[4] == 0xaa);
  assert(reg[REG_PC] == code_base + 4);
}

static void test_arm_memory_and_branch()
{
  reset_cpu(0xE5901000, false); /* LDR r1,[r0] */
  reg[0] = data_base;
  data_words[0] = 0x11223344;
  assert(execute_arm_one(100) == 98);
  assert(reg[1] == 0x11223344);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xE5A01004, false); /* STR r1,[r0,#4]! */
  reg[0] = data_base;
  reg[1] = 0xaabbccdd;
  assert(execute_arm_one(100) == 98);
  assert(data_words[1] == 0xaabbccdd);
  assert(reg[0] == data_base + 4);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xEA000002, false); /* B to instruction address +16 */
  assert(execute_arm_one(100) == 98);
  assert(reg[REG_PC] == code_base + 16);
}

static void test_mode_switch_and_exception()
{
  reset_cpu(0xE12FFF10, false); /* BX r0, into Thumb */
  reg[0] = code_base + 0x21;
  execute_arm_one(100);
  assert(reg[REG_PC] == code_base + 0x20);
  assert((reg[REG_CPSR] & 0x20) != 0);

  reset_cpu(0x4700, true); /* Thumb BX r0, into ARM */
  reg[0] = code_base + 0x20;
  execute_arm_one(100);
  assert(reg[REG_PC] == code_base + 0x20);
  assert((reg[REG_CPSR] & 0x20) == 0);

  reset_cpu(0x2001, true); /* MOVS r0,#1 */
  assert(execute_arm_one(100) == 99);
  assert(reg[0] == 1);
  assert(reg[REG_PC] == code_base + 2);

  reset_cpu(0xEF000000, false); /* SWI */
  execute_arm_one(100);
  assert(reg[REG_PC] == 0x8);
  assert(reg[CPU_MODE] == MODE_SUPERVISOR);
  assert(REG_SPSR(MODE_SUPERVISOR) == 0x1f);
}

static void test_jit_slow_helper()
{
  reset_cpu(0xE3A00007, false); /* MOV r0,#7 */
  uint32_t cycles = 100;
  uint64_t result = gpsp_rv32_interpret_one_helper(reg, &cycles, 0, 0);
  assert(result == GPSP_RV32_HELPER_EXIT);
  assert(cycles == 99);
  assert(reg[0] == 7);
  assert(reg[REG_PC] == code_base + 4);
}

/* Leave an observable instruction after the one under test.  A bridge must
 * return to its caller even when the first instruction falls through. */
static u32 run_slow_one(u32 starting_cycles = 100)
{
  const bool thumb = (reg[REG_CPSR] & 0x20) != 0;
  reg[7] = 0x12345678;
  if (thumb)
    code_words[0] = (code_words[0] & 0xffff) | (0x27a5u << 16);
  else
    code_words[1] = 0xe3a070a5; /* MOV r7,#0xa5 */
  u32 cycles = starting_cycles;
  const u64 result = gpsp_rv32_interpret_one_helper(reg, &cycles, 0, 0);
  assert(result == GPSP_RV32_HELPER_EXIT);
  assert(reg[7] == 0x12345678);
  assert(scheduler_calls == 0);
  return cycles;
}

static void test_slow_arm_memory()
{
  reset_cpu(0xe5901000, false); /* LDR r1,[r0] */
  reg[0] = data_base;
  data_words[0] = 0x11223344;
  assert(run_slow_one() == 98);
  assert(reg[1] == 0x11223344);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xe5a01004, false); /* STR r1,[r0,#4]! */
  reg[0] = data_base;
  reg[1] = 0xaabbccdd;
  assert(run_slow_one() == 98);
  assert(data_words[1] == 0xaabbccdd);
  assert(reg[0] == data_base + 4);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xe8a00006, false); /* STMIA r0!,{r1,r2} */
  ws_cyc_seq[3][1] = 1;
  reg[0] = data_base;
  reg[1] = 0x0badcafe;
  reg[2] = 0xfacefeed;
  assert(run_slow_one() == 97);
  assert(data_words[0] == 0x0badcafe && data_words[1] == 0xfacefeed);
  assert(reg[0] == data_base + 8 && reg[REG_PC] == code_base + 4);

  reset_cpu(0xe8b00006, false); /* LDMIA r0!,{r1,r2} */
  ws_cyc_seq[3][1] = 1;
  reg[0] = data_base;
  data_words[0] = 0x87654321;
  data_words[1] = 0x13579bdf;
  assert(run_slow_one() == 97);
  assert(reg[1] == 0x87654321 && reg[2] == 0x13579bdf);
  assert(reg[0] == data_base + 8 && reg[REG_PC] == code_base + 4);

  reset_cpu(0xe1002091, false); /* SWP r2,r1,[r0] */
  reg[0] = data_base;
  reg[1] = 0x7f00ff11;
  data_words[0] = 0x33aa55cc;
  assert(run_slow_one() == 96); /* fetch, two data waits and internal cycle */
  assert(reg[2] == 0x33aa55cc && data_words[0] == 0x7f00ff11);
  assert(reg[REG_PC] == code_base + 4);
}

static void test_slow_arm_multiply_psr_and_branch()
{
  reset_cpu(0xe0140190, false); /* MULS r4,r0,r1 */
  reg[0] = 0xffffffff;
  reg[1] = 2;
  reg[REG_CPSR] = 0x3000001f; /* C/V must survive */
  assert(run_slow_one() == 99);
  assert(reg[4] == 0xfffffffe);
  assert(reg[REG_CPSR] == 0xb000001f);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xe0b54190, false); /* UMLALS r4,r5,r0,r1 */
  reg[0] = 1;
  reg[1] = 1;
  reg[4] = 0xffffffff;
  reg[5] = 0;
  reg[REG_CPSR] = 0x3000001f;
  assert(run_slow_one() == 99);
  assert(reg[4] == 0 && reg[5] == 1);
  assert(reg[REG_CPSR] == 0x3000001f);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xe10f2000, false); /* MRS r2,CPSR */
  reg[REG_CPSR] = 0xa000001f;
  assert(run_slow_one() == 99);
  assert(reg[2] == 0xa000001f && reg[REG_PC] == code_base + 4);

  reset_cpu(0xe128f000, false); /* MSR CPSR_f,r0 */
  reg[0] = 0x50000000;
  reg[REG_CPSR] = 0xa000001f;
  assert(run_slow_one() == 99);
  assert(reg[REG_CPSR] == 0x5000001f);
  assert(reg[REG_PC] == code_base + 4);

  reset_cpu(0xeb000002, false); /* BL to instruction address +16 */
  assert(run_slow_one() == 98);
  assert(reg[REG_LR] == code_base + 4);
  assert(reg[REG_PC] == code_base + 16);

  reset_cpu(0xef000000, false); /* ARM SWI */
  assert(run_slow_one() == 100); /* BIOS waitstate is zero in fixture */
  assert(reg[REG_PC] == 8);
  assert(reg[CPU_MODE] == MODE_SUPERVISOR);
  assert(REG_SPSR(MODE_SUPERVISOR) == 0x1f);
  assert(REG_MODE(MODE_SUPERVISOR)[6] == code_base + 4);
}

static void test_slow_thumb_memory_branch_and_exception()
{
  reset_cpu(0x6008, true); /* STR r0,[r1,#0] */
  reg[0] = 0xc001cafe;
  reg[1] = data_base;
  assert(run_slow_one() == 98);
  assert(data_words[0] == 0xc001cafe && reg[REG_PC] == code_base + 2);

  reset_cpu(0x6808, true); /* LDR r0,[r1,#0] */
  reg[1] = data_base;
  data_words[0] = 0xa1b2c3d4;
  assert(run_slow_one() == 98);
  assert(reg[0] == 0xa1b2c3d4 && reg[REG_PC] == code_base + 2);

  reset_cpu(0xb403, true); /* PUSH {r0,r1} */
  ws_cyc_seq[3][1] = 1;
  reg[REG_SP] = data_base + 8;
  reg[0] = 0x11111111;
  reg[1] = 0x22222222;
  assert(run_slow_one() == 97);
  assert(data_words[0] == 0x11111111 && data_words[1] == 0x22222222);
  assert(reg[REG_SP] == data_base && reg[REG_PC] == code_base + 2);

  reset_cpu(0xd001, true); /* BEQ +2, taken */
  reg[REG_CPSR] |= 0x40000000;
  assert(run_slow_one() == 98);
  assert(reg[REG_PC] == code_base + 6);

  reset_cpu(0xd001, true); /* BEQ +2, not taken */
  assert(run_slow_one() == 98);
  assert(reg[REG_PC] == code_base + 2);

  reset_cpu(0xdf00, true); /* Thumb SWI */
  assert(run_slow_one() == 100);
  assert(reg[REG_PC] == 8);
  assert(reg[CPU_MODE] == MODE_SUPERVISOR);
  assert(REG_SPSR(MODE_SUPERVISOR) == 0x3f);
  assert(REG_MODE(MODE_SUPERVISOR)[6] == code_base + 2);
}

static void test_slow_ldm_exception_return()
{
  reset_cpu(0xe8fd8001, false); /* LDMIA sp!,{r0,pc}^ */
  reg[CPU_MODE] = MODE_IRQ;
  reg[REG_CPSR] = 0x92;
  reg[REG_SP] = data_base;
  REG_MODE(MODE_USER)[5] = data_base + 0x40;
  REG_SPSR(MODE_IRQ) = 0x30; /* return to user Thumb mode */
  data_words[0] = 0xfeedface;
  data_words[1] = code_base + 0x20;
  ws_cyc_seq[3][1] = 1;
  assert(run_slow_one() == 98); /* mode switch exits before next fetch */
  assert(reg[0] == 0xfeedface);
  assert(reg[REG_PC] == code_base + 0x20);
  assert(reg[REG_CPSR] == 0x30 && reg[CPU_MODE] == MODE_USER);
  assert(reg[REG_SP] == data_base + 0x40);
  assert(REG_MODE(MODE_IRQ)[5] == data_base + 8);
}

static void test_slow_user_bank_transfer()
{
  reset_cpu(0xe8c06000, false); /* STMIA r0,{sp,lr}^ */
  reg[CPU_MODE] = MODE_IRQ;
  reg[REG_CPSR] = 0x92;
  reg[0] = data_base;
  reg[REG_SP] = 0x11111111;
  reg[REG_LR] = 0x22222222;
  REG_MODE(MODE_USER)[5] = 0x33333333;
  REG_MODE(MODE_USER)[6] = 0x44444444;
  ws_cyc_seq[3][1] = 1;
  assert(run_slow_one() == 97);
  assert(data_words[0] == 0x33333333 && data_words[1] == 0x44444444);
  assert(reg[0] == data_base && reg[REG_SP] == 0x11111111);
  assert(reg[REG_LR] == 0x22222222 && reg[CPU_MODE] == MODE_IRQ);

  reset_cpu(0xe8d06000, false); /* LDMIA r0,{sp,lr}^ */
  reg[CPU_MODE] = MODE_IRQ;
  reg[REG_CPSR] = 0x92;
  reg[0] = data_base;
  reg[REG_SP] = 0x11111111;
  reg[REG_LR] = 0x22222222;
  data_words[0] = 0x55555555;
  data_words[1] = 0x66666666;
  ws_cyc_seq[3][1] = 1;
  assert(run_slow_one() == 97);
  assert(REG_MODE(MODE_USER)[5] == 0x55555555);
  assert(REG_MODE(MODE_USER)[6] == 0x66666666);
  assert(reg[0] == data_base && reg[REG_SP] == 0x11111111);
  assert(reg[REG_LR] == 0x22222222 && reg[CPU_MODE] == MODE_IRQ);
}

int main()
{
  test_arm_alu_and_condition();
  test_arm_memory_and_branch();
  test_mode_switch_and_exception();
  test_jit_slow_helper();
  test_slow_arm_memory();
  test_slow_arm_multiply_psr_and_branch();
  test_slow_thumb_memory_branch_and_exception();
  test_slow_ldm_exception_return();
  test_slow_user_bank_transfer();
  assert(scheduler_calls == 0);
  return 0;
}

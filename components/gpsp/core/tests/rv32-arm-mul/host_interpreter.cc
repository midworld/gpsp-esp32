/* Run with make -C tests check-cpu from the core root.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cases.h"
#include "../../cpu.cc"

static uint32_t code_words[8192];

extern "C" {
u8 ws_cyc_seq[16][2];
u8 ws_cyc_nseq[16][2];
u32 gamepak_sticky_bit[1024/32];
u32 cheat_master_hook = 0xFFFFFFFF;
u32 idle_loop_target_pc = 0xFFFFFFFF;

u8 *load_gamepak_page(u32) { assert(false); return NULL; }
void process_cheats(void) { assert(false); }
u32 update_gba(int) { assert(false); return 0; }
u32 read_memory8(u32) { assert(false); return 0; }
u32 read_memory16(u32) { assert(false); return 0; }
u16 read_memory16_signed(u32) { assert(false); return 0; }
u32 read_memory32(u32) { assert(false); return 0; }
cpu_alert_type write_memory8(u32, u8) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory16(u32, u16) { assert(false); return CPU_ALERT_NONE; }
cpu_alert_type write_memory32(u32, u32) { assert(false); return CPU_ALERT_NONE; }
}

static bool check_case(const mul_case *test)
{
  uint32_t expected[64];
  mul_case_init_regs(test, expected);
  memset(reg, 0, sizeof(reg));
  memcpy(reg, expected, sizeof(expected));
  memset(reg_mode, 0, sizeof(reg_mode));
  memset(spsr, 0, sizeof(spsr));
  memset(code_words, 0, sizeof(code_words));
  memset(memory_map_read, 0, sizeof(memory_map_read));
  memset(ws_cyc_seq, 0, sizeof(ws_cyc_seq));
  memset(ws_cyc_nseq, 0, sizeof(ws_cyc_nseq));
  memset(gamepak_sticky_bit, 0, sizeof(gamepak_sticky_bit));
  code_words[0] = test->opcode;
  memory_map_read[MUL_PC >> 15] = (u8 *)code_words;
  ws_cyc_seq[6][1] = 1;
  ws_cyc_nseq[6][1] = 1;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;

  mul_case_reference(test, expected);
  (void)execute_arm_one(100);
  for (unsigned r = 0; r <= 17; ++r)
  {
    if (reg[r] != expected[r])
    {
      fprintf(stderr, "interpreter mismatch: %s opcode=%08x r%u got=%08x expected=%08x\n",
          test->name, test->opcode, r, reg[r], expected[r]);
      return false;
    }
  }
  return true;
}

int main()
{
  for (unsigned i = 0; i < MUL_CASE_COUNT; ++i)
    if (!check_case(&mul_cases[i]))
      return 1;

  for (unsigned cond = 0; cond < 16; ++cond)
  {
    for (unsigned flags = 0; flags < 16; ++flags)
    {
      mul_case test = {"condition matrix",
          ARM_MUL_SHORT(cond, 0, 1, 4, 0, 1, 0), flags << 28, 2,
          {{0, 3}, {1, 5}}};
      if (!check_case(&test))
        return 1;
    }
  }
  printf("ARM multiply interpreter fixtures: %u directed + 256 conditions PASS\n",
      (unsigned)MUL_CASE_COUNT);
  return 0;
}

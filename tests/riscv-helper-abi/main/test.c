/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv32_stub.h"

extern void gpsp_rv32_test_continue_block(void);
extern void gpsp_rv32_test_mode_block(void);
extern void gpsp_rv32_test_force_block(void);

/* The stub links the Thumb slow bridge; these blocks never reach it. */
uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
        uint32_t opcode, uint32_t unused)
{
    (void)regs;
    (void)cycles;
    (void)opcode;
    (void)unused;
    abort();
}

static unsigned helper_error;
static unsigned helper_calls;

static void check_call(uint32_t *regs, uint32_t *cycles,
        uint32_t arg0, uint32_t arg1, uint32_t want0, uint32_t want1)
{
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    if ((sp & 15) || arg0 != want0 || arg1 != want1 ||
        regs[0] != 1 || regs[4] != 4 || regs[8] != 8 ||
            regs[9] != 9 || regs[13] != 13 || regs[14] != 14 ||
            regs[15] != 0x08000000 || regs[16] != 0xA000001F ||
            *cycles != 100)
        helper_error++;
    helper_calls++;
}

uint64_t gpsp_rv32_test_helper_continue(uint32_t *regs, uint32_t *cycles,
        uint32_t arg0, uint32_t arg1)
{
    check_call(regs, cycles, arg0, arg1, 0x123, 0x456);
    regs[0] = 40;
    regs[4] = 44;
    regs[8] = 80;
    regs[9] = 90;
    regs[13] = 0x100;
    regs[14] = 0x200;
    regs[16] = 0x5000001F;      /* NZCV change without mode change */
    *cycles -= 5;
    return UINT64_C(0x12345678);
}

uint64_t gpsp_rv32_test_helper_mode(uint32_t *regs, uint32_t *cycles,
        uint32_t arg0, uint32_t arg1)
{
    check_call(regs, cycles, arg0, arg1, 0x789, 0xABC);
    regs[0] = 50;
    regs[4] = 54;
    regs[15] = 0x08000100;
    regs[16] = 0xF0000033;      /* Thumb + supervisor mode */
    regs[17] = 0x13;            /* gpSP CPU_MODE */
    *cycles -= 7;
    return UINT64_C(0xDEADBEEF);
}

uint64_t gpsp_rv32_test_helper_force(uint32_t *regs, uint32_t *cycles,
        uint32_t arg0, uint32_t arg1)
{
    check_call(regs, cycles, arg0, arg1, 0x135, 0x246);
    regs[0] = 60;
    *cycles -= 9;
    return GPSP_RV32_HELPER_EXIT | UINT64_C(0xCAFEBABE);
}

static void init_regs(uint32_t *regs)
{
    memset(regs, 0, 64 * sizeof(*regs));
    for (unsigned i = 0; i < 15; ++i)
        regs[i] = i;
    regs[0] = 1;
    regs[15] = 0x08000000;
    regs[16] = 0xA000001F;
    regs[17] = 0x1F;
}

void app_main(void)
{
    uint32_t regs[64];
    int pass = 1;
    init_regs(regs);
    if (gpsp_rv32_run_block(regs, 100, gpsp_rv32_test_continue_block) != 93 ||
        regs[0] != 42 || regs[2] != 0x12345678 || regs[4] != 44 ||
            regs[8] != 80 || regs[9] != 91 || regs[13] != 0x100 ||
            regs[14] != 0x200 || regs[15] != 0x08000004 ||
            regs[16] != 0x5000001F || helper_calls != 1 || helper_error)
        pass = 0;

    init_regs(regs);
    if (gpsp_rv32_run_block(regs, 100, gpsp_rv32_test_mode_block) != 93 ||
        regs[0] != 50 || regs[4] != 54 || regs[15] != 0x08000100 ||
            regs[16] != 0xF0000033 || regs[17] != 0x13 ||
            helper_calls != 2 || helper_error)
        pass = 0;

    init_regs(regs);
    if (gpsp_rv32_run_block(regs, 100, gpsp_rv32_test_force_block) != 91 ||
        regs[0] != 60 || regs[15] != 0x08000000 ||
            regs[16] != 0xA000001F || helper_calls != 3 || helper_error)
        pass = 0;

    printf("RV32 HELPER ABI RESULT: %s (calls=%u errors=%u)\n",
            pass ? "PASS" : "FAIL", helper_calls, helper_error);
    printf("RV32 HELPER ABI COMPLETE\n");
}

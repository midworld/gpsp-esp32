/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Budget guards in the block-JIT stub: the lean update_gba path and the C
 * update path, each resuming in place or reloading changed guest state. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rv32_stub.h"

extern void gpsp_rv32_abi_flags_guard_block(void);
extern void gpsp_rv32_abi_lazy_guard_block(void);

uint32_t gpsp_rv32_pending_alert;
uint32_t cheat_master_hook = 0xffffffff;
uint32_t gpsp_rv32_native_budget_updates;

static uint32_t *budget_regs;
static uint32_t update_next;
static int update_resume;
static int change_state;
static unsigned update_calls, finish_calls, c_calls;
static int32_t seen_remaining;
static uint32_t seen_cpsr, seen_r0;

static void observe(uint32_t *regs, int32_t remaining)
{
    seen_remaining = remaining;
    seen_cpsr = regs[16];
    seen_r0 = regs[0];
    if (change_state)
    {
        regs[0] = 0x55;
        regs[15] = 0x08000100;
        regs[16] = change_state == 1 ? 0x6000001f : 0xf000001f;
    }
}

uint32_t update_gba(int remaining)
{
    update_calls++;
    observe(budget_regs, remaining);
    return update_next;
}

uint32_t gpsp_rv32_native_budget_finish(uint32_t next)
{
    finish_calls++;
    return next;
}

uint64_t gpsp_rv32_native_budget_update(uint32_t *regs, int32_t remaining)
{
    c_calls++;
    observe(regs, remaining);
    return (uint64_t)update_next | ((uint64_t)update_resume << 32);
}

static const struct
{
    const char *name;
    int32_t cycles;
    uint32_t next;
    int resume, change;
    uint32_t cpsr;
    /* Expected calls and final state. */
    unsigned lean, finish, c;
    uint64_t result;
    uint32_t r0, pc, final_cpsr;
    int lazy;
} cases[] = {
    /* update_gba returns no status: the guard body resumes. */
    {"lean resume", 0, 5, 0, 0, 0x9000001f, 1, 0, 0, 4,
            11, 0x08000002, 0x9000001f, 0},
    /* A status bit reloads registers and CPSR written by the update. */
    {"lean reload", 0, 0x40000007, 0, 1, 0x9000001f, 1, 1, 0,
            ((uint64_t)2 << 32) | 7, 0x55, 0x08000100, 0x6000001f, 0},
    /* Large debt takes the C update, which may resume. */
    {"C resume", -100, 10, 1, 0, 0x3000001f, 0, 0, 1, 9,
            11, 0x08000002, 0x3000001f, 0},
    /* A frame end from the C update reloads and returns the raw status. */
    {"C reload", -100, 0x80000003, 0, 2, 0x5000001f, 0, 0, 1,
            ((uint64_t)UINT32_MAX << 32) | 0x80000003, 0x55, 0x08000100,
            0xf000001f, 0},
    {"lean lazy ADD", 0, 5, 0, 0, 0x9000001f, 1, 0, 0, 4,
            11, 0x08000002, 0x9000001f, 1},
    {"C lazy ADD", -100, 10, 1, 0, 0x9000001f, 0, 0, 1, 9,
            11, 0x08000002, 0x9000001f, 1},
};

int test_budget_paths(void)
{
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t regs[64];
        memset(regs, 0, sizeof(regs));
        regs[0] = 10;
        regs[15] = 0x08000000;
        /* The guard must replace the stale NZCV before calling the update. */
        regs[16] = cases[i].cpsr ^ 0xf0000000u;
        if (cases[i].lazy)
        {
            regs[4] = 0x7fffffff;
            regs[5] = 1;
        }
        budget_regs = regs;
        update_next = cases[i].next;
        update_resume = cases[i].resume;
        change_state = cases[i].change;
        update_calls = finish_calls = c_calls = 0;
        seen_cpsr = seen_r0 = 0;
        uint64_t result = gpsp_rv32_run_block(regs, (uint32_t)cases[i].cycles,
                cases[i].lazy ? gpsp_rv32_abi_lazy_guard_block :
                gpsp_rv32_abi_flags_guard_block);
        /* The update must see stored registers and packed NZCV. */
        if (update_calls != cases[i].lean || finish_calls != cases[i].finish ||
            c_calls != cases[i].c || seen_remaining != cases[i].cycles ||
                seen_cpsr != cases[i].cpsr || seen_r0 != 10 ||
                result != cases[i].result || regs[0] != cases[i].r0 ||
                regs[15] != cases[i].pc || regs[16] != cases[i].final_cpsr)
        {
            printf("budget %s: calls=%u/%u/%u result=%08lx:%08lx r0=%08lx "
                    "pc=%08lx cpsr=%08lx seen=%08lx\n", cases[i].name,
                    update_calls, finish_calls, c_calls,
                    (unsigned long)(result >> 32), (unsigned long)result,
                    (unsigned long)regs[0], (unsigned long)regs[15],
                    (unsigned long)regs[16], (unsigned long)seen_cpsr);
            return 0;
        }
    }
    return 1;
}

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv32_stub.h"

extern uint32_t gpsp_rv32_abi_check_saved(uint32_t *regs, uint32_t cycles,
        void (*block)(void));
extern void gpsp_rv32_abi_test_block(void);
extern void gpsp_rv32_abi_guard_block(void);
extern void gpsp_rv32_abi_guard_body(void);
extern void gpsp_rv32_abi_bl_cache_block(void);
extern void gpsp_rv32_abi_arm_cache_block(void);
extern void gpsp_rv32_abi_cached_block(void);
extern void gpsp_rv32_abi_arm_pc_exit_block(void);
extern void gpsp_rv32_abi_system_exit_block(void);
extern void gpsp_rv32_abi_flags_block(void);
extern void gpsp_rv32_abi_lazy_add_block(void);
extern int test_cached_bl_pair(void);
#if defined(TEST_STUB_BUDGET)
extern int test_budget_paths(void);
#endif

uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];
uint32_t idle_loop_target_pc = UINT32_MAX;
uint32_t gpsp_rv32_branch_cache[1024 * 2];

uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
        uint32_t opcode, uint32_t unused)
{
    (void)regs;
    (void)cycles;
    (void)opcode;
    (void)unused;
    abort();
}

static int test_cached_fetch(void)
{
    for (unsigned thumb = 0; thumb < 2; thumb++)
        for (unsigned offset = 0; offset < (thumb ? 4 : 1); offset++)
            for (unsigned hit = 0; hit < 2; hit++)
                for (unsigned budget = 0; budget < 2; budget++)
                {
                    uint32_t regs[64] = {0};
                    uint32_t target = 0x03000100 + offset;
                    unsigned slot = ((target >> 1) & 1023) * 2;
                    uint32_t key = target | thumb;
                    memset(gpsp_rv32_branch_cache, 0,
                            sizeof(gpsp_rv32_branch_cache));
                    gpsp_rv32_branch_cache[slot] = hit ? key : key ^ 0x800;
                    gpsp_rv32_branch_cache[slot + 1] =
                            (uint32_t)(uintptr_t)gpsp_rv32_abi_cached_block;
                    regs[14] = target;
                    regs[15] = 0x08000000;
                    regs[16] = 0xa000001f | (thumb << 5);
                    uint32_t cycles = budget ? 100 : 0;
                    uint64_t result = gpsp_rv32_run_block(regs, cycles,
                            thumb ? gpsp_rv32_abi_bl_cache_block :
                            gpsp_rv32_abi_arm_cache_block);
                    int entered = hit && budget;
                    /* Idle and cheat hooks use the raw target before fetch alignment. */
                    uint32_t fetch_pc = entered && thumb ? target & ~1u : target;
                    if ((uint32_t)result != cycles || (uint32_t)(result >> 32) ||
                        regs[0] != (entered ? fetch_pc : 0) ||
                            regs[14] != target || regs[15] != fetch_pc ||
                            regs[16] != (0xa000001f | (thumb << 5)))
                    {
                        printf("cache thumb=%u offset=%u hit=%u budget=%u: "
                                "r0=%08lx pc=%08lx\n", thumb, offset, hit, budget,
                                (unsigned long)regs[0], (unsigned long)regs[15]);
                        return 0;
                    }
                }
    return 1;
}

/* ARM PC and system exits read CPSR.T; the cache key must carry it. */
static int test_exit_thumb_state(void)
{
    for (unsigned system = 0; system < 2; system++)
        for (unsigned thumb = 0; thumb < 2; thumb++)
            for (unsigned match = 0; match < 2; match++)
            {
                uint32_t regs[64] = {0};
                const uint32_t target = 0x03000100;
                unsigned slot = ((target >> 1) & 1023) * 2;
                memset(gpsp_rv32_branch_cache, 0, sizeof(gpsp_rv32_branch_cache));
                gpsp_rv32_branch_cache[slot] = target | (match ? thumb : !thumb);
                gpsp_rv32_branch_cache[slot + 1] =
                        (uint32_t)(uintptr_t)gpsp_rv32_abi_cached_block;
                regs[14] = target;
                regs[15] = 0x08000000;
                regs[16] = 0x6000001f | (thumb << 5);
                uint64_t result = gpsp_rv32_run_block(regs, 100,
                        system ? gpsp_rv32_abi_system_exit_block :
                        gpsp_rv32_abi_arm_pc_exit_block);
                if ((uint32_t)result != 100 || (uint32_t)(result >> 32) ||
                    regs[0] != (match ? target : 0) || regs[15] != target ||
                        regs[16] != (0x6000001f | (thumb << 5)))
                {
                    printf("exit system=%u thumb=%u match=%u: r0=%08lx pc=%08lx\n",
                            system, thumb, match, (unsigned long)regs[0],
                            (unsigned long)regs[15]);
                    return 0;
                }
            }
    return 1;
}

/* Entry unpacks and exit packs every NZCV pair; CPSR bits 0-27 stay. */
static int test_nzcv(void)
{
    for (unsigned in = 0; in < 16; in++)
        for (unsigned out = 0; out < 16; out++)
        {
            uint32_t regs[64] = {0};
            regs[15] = 0x08000000;
            regs[16] = (in << 28) | 0x0abcdef3;
            for (unsigned bit = 0; bit < 4; bit++)
                regs[4 + bit] = (out >> (3 - bit)) & 1;
            gpsp_rv32_run_block(regs, 1, gpsp_rv32_abi_flags_block);
            for (unsigned bit = 0; bit < 4; bit++)
                if (regs[bit] != ((in >> (3 - bit)) & 1))
                    goto fail;
            if (regs[16] != ((out << 28) | 0x0abcdef3))
                goto fail;
            continue;
fail:
            printf("nzcv in=%x out=%x: r0-r3=%lu%lu%lu%lu cpsr=%08lx\n", in, out,
                    (unsigned long)regs[0], (unsigned long)regs[1],
                    (unsigned long)regs[2], (unsigned long)regs[3],
                    (unsigned long)regs[16]);
            return 0;
        }
    return 1;
}

/* A pending ADD resolves to NZCV before the exit packs CPSR. */
static int test_lazy_add(void)
{
    static const struct { uint32_t a, b, nzcv; } cases[] = {
        {1, 2, 0x0}, {0x7fffffff, 1, 0x9}, {0xffffffff, 1, 0x6},
        {0x80000000, 0x80000000, 0x7}, {0xffffffff, 0xffffffff, 0xa},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t regs[64] = {0};
        regs[4] = cases[i].a;
        regs[5] = cases[i].b;
        regs[15] = 0x08000000;
        regs[16] = 0x0000001f;
        gpsp_rv32_run_block(regs, 1, gpsp_rv32_abi_lazy_add_block);
        if (regs[16] != ((cases[i].nzcv << 28) | 0x1f))
        {
            printf("lazy add %u: cpsr=%08lx\n", i, (unsigned long)regs[16]);
            return 0;
        }
    }
    return 1;
}

static int test_raw_idle(void)
{
    const uint32_t target = 0x03000103;
    for (unsigned raw = 0; raw < 2; raw++)
    {
        uint32_t regs[64] = {0};
        unsigned slot = ((target >> 1) & 1023) * 2;
        memset(gpsp_rv32_branch_cache, 0, sizeof(gpsp_rv32_branch_cache));
        gpsp_rv32_branch_cache[slot] = target;
        gpsp_rv32_branch_cache[slot + 1] =
                (uint32_t)(uintptr_t)gpsp_rv32_abi_cached_block;
        idle_loop_target_pc = raw ? target : target & ~1u;
        regs[14] = target;
        regs[15] = 0x08000000;
        regs[16] = 0xa000003f;
        uint64_t result = gpsp_rv32_run_block(regs, 100,
                gpsp_rv32_abi_bl_cache_block);
        if ((uint32_t)(result >> 32) || (uint32_t)result != (raw ? 0 : 100) ||
            regs[0] != (raw ? 0 : target & ~1u) ||
                regs[14] != target || regs[15] != (raw ? target : target & ~1u) ||
                regs[16] != 0xa000003f)
        {
            idle_loop_target_pc = UINT32_MAX;
            printf("idle raw=%u: r0=%08lx pc=%08lx cycles=%ld\n", raw,
                    (unsigned long)regs[0], (unsigned long)regs[15],
                    (long)(int32_t)result);
            return 0;
        }
    }
    idle_loop_target_pc = UINT32_MAX;
    return 1;
}

void app_main(void)
{
    uint32_t regs[64];
    for (unsigned i = 0; i < 64; ++i)
        regs[i] = 0x1000 + i;

    regs[0] = 3;
    regs[4] = 10;
    regs[8] = 100;
    regs[9] = 200;
    regs[13] = 0x1234;
    regs[14] = 0x8000;
    regs[15] = 0x08000000;
    regs[16] = 0xA000001F;

    uint32_t cycles = gpsp_rv32_abi_check_saved(regs, 100,
            gpsp_rv32_abi_test_block);
    int pass = cycles == 97 && regs[0] == 8 && regs[4] == 18 &&
            regs[8] == 107 && regs[9] == 209 && regs[12] == 0x1017 &&
            regs[13] == 0x1238 &&
            regs[14] == 0x7FFE && regs[15] == 0x08000004 &&
            regs[16] == 0x5000001F;
    for (unsigned i = 0; i < 15; ++i)
    {
        if (i == 0 || i == 4 || i == 8 || i == 9 || i == 12 ||
            i == 13 || i == 14)
            continue;
        if (regs[i] != 0x1000 + i)
            pass = 0;
    }
    if (regs[17] != 0x1011 || regs[63] != 0x103F)
        pass = 0;

#if !defined(TEST_STUB_BUDGET)
    regs[0] = 0;
    uint64_t guard_result = gpsp_rv32_run_block(regs, 0,
            gpsp_rv32_abi_guard_block);
    if ((uint32_t)guard_result != 0 ||
        (uint32_t)(guard_result >> 32) !=
            (uint32_t)(uintptr_t)gpsp_rv32_abi_guard_body ||
            regs[0] != 0)
        pass = 0;

    uint64_t resumed_result = gpsp_rv32_run_block(regs, 5,
            gpsp_rv32_abi_guard_body);
    if ((uint32_t)resumed_result != 4 ||
        (uint32_t)(resumed_result >> 32) != 0 ||
            regs[0] != 1 || regs[15] != 0x08000006)
        pass = 0;
#else
    if (!test_budget_paths())
        pass = 0;
#endif
    if (!test_nzcv())
        pass = 0;
    if (!test_lazy_add())
        pass = 0;

    if (!test_cached_fetch())
        pass = 0;
    if (!test_raw_idle())
        pass = 0;
    if (!test_exit_thumb_state())
        pass = 0;
    if (!test_cached_bl_pair())
        pass = 0;

    printf("RV32 STUB ABI RESULT: %s\n", pass ? "PASS" : "FAIL");
    printf("RV32 STUB ABI COMPLETE\n");
}

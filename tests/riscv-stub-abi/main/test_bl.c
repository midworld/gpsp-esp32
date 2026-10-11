/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "rv32_stub.h"

/* Exercise the production BL-pair emitter without the block scanner. */
#include "rv32_thumb_flow_full.c"

#define CODE_SIZE 256

extern uint8_t ws_cyc_seq[16][2];
extern uint32_t gpsp_rv32_branch_cache[1024 * 2];
const uint32_t def_seq_cycles[16][2] = {{0}};

static int sync_code(void *memory, size_t size)
{
    esp_err_t err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    __asm__ volatile("fence.i" ::: "memory");
    return 1;
}

static int run_bl_cases(uint8_t *memory)
{
    const uint32_t source_pc = 0x08000040;
    ws_cyc_seq[8][0] = 1;
    ws_cyc_nseq[8][0] = 1;
    for (unsigned offset = 0; offset < 4; offset += 2)
        for (unsigned far = 0; far < 2; far++)
            for (unsigned suffix = 0; suffix < 2; suffix++)
            {
                uint32_t pc = 0x08000100 + offset;
                uint32_t target = bl_pair_target(pc, far ? 0xf001 : 0xf000,
                        far ? 0xf800 : 0xf81e);
                uint16_t opcode = suffix ? 0xf803 : 0xf800;
                uint8_t *pair = memory + CODE_SIZE;
                gpsp_rv32_code code;
                gpsp_rv32_code_init(&code, memory, CODE_SIZE);
                if (gpsp_rv32_emit_thumb(&code, opcode, source_pc, 15) !=
                        GPSP_RV32_THUMB_EXIT ||
                        !gpsp_rv32_flow_dynamic_thumb_exit(&code, opcode, false))
                    return 0;
                gpsp_rv32_thumb_flow_full_layout layout = {0};
                gpsp_rv32_code_init(&code, pair, CODE_SIZE);
                if (!emit_bl_pair(&code, pair, pc, target, 0, &layout) ||
                    !sync_code(memory, CODE_SIZE * 2))
                    return 0;

                for (unsigned hit = 0; hit < 2; hit++)
                    for (unsigned budget = 0; budget < 2; budget++)
                    {
                        uint32_t regs[64] = {0};
                        unsigned slot = ((pc >> 1) & 1023) * 2;
                        memset(gpsp_rv32_branch_cache, 0,
                                sizeof(gpsp_rv32_branch_cache));
                        gpsp_rv32_branch_cache[slot] = hit ? pc | 1 : pc ^ 0x800;
                        gpsp_rv32_branch_cache[slot + 1] = (uint32_t)(uintptr_t)pair;
                        regs[14] = (pc | 1) - ((opcode & 0x7ff) << 1);
                        regs[15] = source_pc;
                        regs[16] = 0xb000003f;
                        uint32_t cycles = budget ? 100 : 2;
                        uint64_t result = gpsp_rv32_run_block(regs, cycles,
                                (void (*)(void))memory);
                        int entered = hit && budget;
                        if ((uint32_t)(result >> 32) ||
                            (uint32_t)result != cycles - (entered ? 5 : 2) ||
                                regs[14] != (entered ? pc + 5 : source_pc + 3) ||
                                regs[15] != (entered ? target : pc | 1) ||
                                regs[16] != 0xb000003f)
                        {
                            printf("BL offset=%u far=%u suffix=%u hit=%u budget=%u: "
                                    "lr=%08lx pc=%08lx cycles=%ld\n", offset, far,
                                    suffix, hit, budget, (unsigned long)regs[14],
                                    (unsigned long)regs[15], (long)(int32_t)result);
                            return 0;
                        }
                    }
            }
    return 1;
}

int test_cached_bl_pair(void)
{
    uint8_t *memory = heap_caps_aligned_alloc(64, CODE_SIZE * 2,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
        return 0;
    int pass = run_bl_cases(memory);
    ws_cyc_seq[8][0] = 0;
    ws_cyc_nseq[8][0] = 0;
    heap_caps_free(memory);
    printf("RV32 CACHED BL RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

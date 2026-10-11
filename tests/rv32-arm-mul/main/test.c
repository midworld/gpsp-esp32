/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cases.h"
#include "rv32_arm_mul.h"
#include "rv32_register_map.h"
#include "rv32_stub.h"

#define CODE_SIZE 4096

static const char *TAG = "rv32_arm_mul";

/* The shared entry stub references this unused Thumb bridge. */
uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
        uint32_t opcode, uint32_t unused)
{
    (void)regs;
    (void)cycles;
    (void)opcode;
    (void)unused;
    return 0;
}

static bool sync_code(void *memory)
{
    esp_err_t err = esp_cache_msync(memory, CODE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return false;
    err = esp_cache_msync(memory, CODE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return false;
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

static bool run_case(uint8_t *memory, const mul_case *test)
{
    uint32_t regs[64];
    uint32_t expected[64];
    mul_case_init_regs(test, regs);
    memcpy(expected, regs, sizeof(expected));
    mul_case_reference(test, expected);

    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, memory, CODE_SIZE);
    if (gpsp_rv32_emit_arm_mul(&code, test->opcode, MUL_PC) !=
            GPSP_RV32_ARM_MUL_EMITTED ||
            !gpsp_rv32_addi(&code, RV_PC, RV_PC, 4) ||
            !gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, -1) ||
            !gpsp_rv32_jalr(&code, X0, RA, 0) ||
            !sync_code(memory))
        return false;

    if (gpsp_rv32_run_block(regs, 100, (void (*)(void))memory) != 99)
        return false;
    for (unsigned r = 0; r <= 17; ++r)
    {
        if (regs[r] != expected[r])
        {
            ESP_LOGE(TAG, "%s opcode=%08lx r%u got=%08lx expected=%08lx",
                    test->name, (unsigned long)test->opcode, r,
                    (unsigned long)regs[r], (unsigned long)expected[r]);
            return false;
        }
    }
    return true;
}

static bool check_failure(uint32_t opcode, uint32_t pc,
        gpsp_rv32_arm_mul_result expected_result, size_t capacity)
{
    uint32_t words[32];
    memset(words, 0xA5, sizeof(words));
    uint32_t original[32];
    memcpy(original, words, sizeof(words));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, capacity);
    uint8_t *before = code.ptr;
    return gpsp_rv32_emit_arm_mul(&code, opcode, pc) == expected_result &&
            code.ptr == before && memcmp(words, original, sizeof(words)) == 0;
}

static bool check_failures(void)
{
    static const uint32_t bad_opcodes[] = {
        ARM_MUL_SHORT(14, 0, 0, 4, 1, 2, 0), /* MUL reserved Rn */
        ARM_MUL_SHORT(14, 0, 0, 15, 0, 2, 0),
        ARM_MUL_SHORT(14, 0, 0, 4, 0, 15, 0),
        ARM_MUL_SHORT(14, 0, 0, 4, 0, 2, 15),
        ARM_MUL_SHORT(14, 1, 0, 4, 15, 2, 0),
        ARM_MUL_LONG(14, 0, 0, 0, 4, 4, 2, 0),
        ARM_MUL_LONG(14, 0, 0, 0, 15, 4, 2, 0),
        ARM_MUL_LONG(14, 0, 0, 0, 5, 15, 2, 0),
        ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 15, 0),
        ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 2, 15),
        ARM_MUL_SHORT(14, 0, 0, 4, 0, 2, 0) ^ 0x80u,
    };
    for (unsigned i = 0; i < sizeof(bad_opcodes) / sizeof(bad_opcodes[0]); ++i)
        if (!check_failure(bad_opcodes[i], MUL_PC,
                GPSP_RV32_ARM_MUL_UNSUPPORTED, sizeof(uint32_t) * 32))
            return false;
    return check_failure(ARM_MUL_SHORT(14, 0, 0, 4, 0, 2, 0), MUL_PC + 2,
            GPSP_RV32_ARM_MUL_UNSUPPORTED, sizeof(uint32_t) * 32) &&
            check_failure(ARM_MUL_LONG(14, 1, 1, 1, 5, 4, 2, 0), MUL_PC,
                    GPSP_RV32_ARM_MUL_BUFFER_FULL, sizeof(uint32_t));
}

static bool check_sizes(void)
{
    static const struct
    {
        uint32_t opcode;
        size_t bytes;
    } cases[] = {
        {ARM_MUL_SHORT(14, 0, 0, 4, 0, 1, 0), 4},
        {ARM_MUL_SHORT(14, 0, 1, 4, 0, 1, 0), 12},
        {ARM_MUL_SHORT(14, 1, 0, 4, 2, 1, 0), 8},
        {ARM_MUL_SHORT(14, 1, 1, 4, 2, 1, 0), 16},
        {ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 1, 0), 8},
        {ARM_MUL_LONG(14, 0, 0, 1, 5, 4, 1, 0), 20},
        {ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 1, 4), 12},
        {ARM_MUL_LONG(14, 0, 0, 0, 4, 5, 1, 4), 8},
        {ARM_MUL_LONG(14, 0, 1, 0, 5, 4, 1, 0), 32},
        {ARM_MUL_LONG(14, 0, 1, 1, 5, 4, 1, 0), 44},
    };
    uint32_t words[32];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        code.compress = false;  /* sizes of 32-bit instructions */
        if (gpsp_rv32_emit_arm_mul(&code, cases[i].opcode, MUL_PC) !=
                GPSP_RV32_ARM_MUL_EMITTED ||
                (size_t)(code.ptr - code.start) != cases[i].bytes)
        {
            ESP_LOGE(TAG, "size case %u got=%u expected=%u", i,
                    (unsigned)(code.ptr - code.start), (unsigned)cases[i].bytes);
            return false;
        }
    }
    return true;
}

void app_main(void)
{
#ifdef MALLOC_CAP_EXEC
    uint8_t *memory = heap_caps_aligned_alloc(64, CODE_SIZE,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
    {
        ESP_LOGE(TAG, "executable allocation failed");
        ESP_LOGI(TAG, "RV32 ARM MUL RESULT: FAIL");
        ESP_LOGI(TAG, "RV32 ARM MUL COMPLETE");
        return;
    }

    bool pass = check_failures() && check_sizes();
    for (unsigned i = 0; pass && i < MUL_CASE_COUNT; ++i)
        if (!run_case(memory, &mul_cases[i]))
        {
            ESP_LOGE(TAG, "case %u (%s) failed", i, mul_cases[i].name);
            pass = false;
        }

    for (unsigned cond = 0; pass && cond < 16; ++cond)
    {
        for (unsigned flags = 0; pass && flags < 16; ++flags)
        {
            mul_case test = {"condition matrix",
                    ARM_MUL_SHORT(cond, 0, 1, 4, 0, 1, 0), flags << 28, 2,
                    {{0, 3}, {1, 5}}};
            pass = run_case(memory, &test);
        }
    }

    ESP_LOGI(TAG, "directed=%u condition_matrix=256 failures_atomic=%u sizes=10",
            (unsigned)MUL_CASE_COUNT, 13u);
    ESP_LOGI(TAG, "RV32 ARM MUL RESULT: %s", pass ? "PASS" : "FAIL");
    heap_caps_free(memory);
#else
    ESP_LOGI(TAG, "RV32 ARM MUL RESULT: UNAVAILABLE");
#endif
    ESP_LOGI(TAG, "RV32 ARM MUL COMPLETE");
}

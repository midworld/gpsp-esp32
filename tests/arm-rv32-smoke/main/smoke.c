/* ARM ALU emitter execution test, independent of gpSP's dispatcher.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "rv32_codegen.h"
#include "rv32_arm_alu.h"
#include "rv32_arm_flow_alu.h"
#include "rv32_register_map.h"
#include "rv32_stub.h"
#include "../../rv32-golden.h"

#define CODE_SIZE 4096


static const char *TAG = "arm_rv32_smoke";

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

static bool sync_code(void *ptr, size_t size)
{
    esp_err_t err = esp_cache_msync(ptr, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return false;
    err = esp_cache_msync(ptr, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return false;
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

typedef struct {
    uint32_t opcode;
    uint32_t initial_flags;
    unsigned source_reg;
    uint32_t source_value;
    unsigned changed_reg;
    uint32_t changed_value;
    uint32_t final_flags;
} alu_vector;

#define NO_REG 16u
#define ARM_PC 0x06000000u

/* Expected values are shared with the one-instruction interpreter fixtures
 * in core/tests/arm_alu_interpreter.cc. The wrapper owns PC/cycle updates. */
static const alu_vector alu_vectors[] = {
    {0xE3A00007, 0xB0000000, NO_REG, 0, 0, 7, 0xB0000000},
    {0xE3B01000, 0xB0000000, NO_REG, 0, 1, 0, 0x70000000},
    {0xE2832001, 0xB0000000, 3, 0x7FFFFFFF, 2, 0x80000000, 0xB0000000},
    {0xE2932001, 0, 3, 0x7FFFFFFF, 2, 0x80000000, 0x90000000},
    {0xE2932001, 0, 3, 0xFFFFFFFF, 2, 0, 0x60000000},
    {0xE2532001, 0, 3, 0, 2, 0xFFFFFFFF, 0x80000000},
    {0xE2532001, 0, 3, 0x80000000, 2, 0x7FFFFFFF, 0x30000000},
    {0xE3530002, 0, 3, 2, NO_REG, 0, 0x60000000},
    {0x03B00005, 0x70000000, NO_REG, 0, 0, 5, 0x30000000},
    {0x03B00005, 0x10000000, NO_REG, 0, NO_REG, 0, 0x10000000},
    {0xE3B04480, 0x10000000, NO_REG, 0, 4, 0x80000000, 0xB0000000},
    {0x03B04480, 0x10000000, NO_REG, 0, NO_REG, 0, 0x10000000},
    {0xE28F0004, 0, NO_REG, 0, 0, ARM_PC + 12, 0},
    {0xE1A0000F, 0, NO_REG, 0, 0, ARM_PC + 8, 0},
    {0xE1B00001, 0x30000000, NO_REG, 0, 0, 0x1001, 0x30000000},

    /* Arithmetic with carry and borrow, including overflow. */
    {0xE2B32000, 0x20000000, 3, 0x7FFFFFFF, 2, 0x80000000, 0x90000000}, /* ADCS */
    {0xE2D32000, 0, 3, 0, 2, 0xFFFFFFFF, 0x80000000}, /* SBCS C=0 */
    {0xE2D32000, 0x20000000, 3, 0, 2, 0, 0x60000000}, /* SBCS C=1 */
    {0xE2F32000, 0x20000000, 3, 0x80000000, 2, 0x80000000, 0x90000000}, /* RSCS */
    {0xE2F32000, 0, 3, 0, 2, 0xFFFFFFFF, 0x80000000}, /* RSCS C=0 */
    {0xE2732001, 0, 3, 2, 2, 0xFFFFFFFF, 0x80000000}, /* RSBS */
    {0xE3730001, 0, 3, 0xFFFFFFFF, NO_REG, 0, 0x60000000}, /* CMN */

    /* ADD-family V: negative overflow, aliases and a failed condition. */
    {0xE0932003, 0, 3, 0x80000000, 2, 0, 0x70000000}, /* ADDS r2,r3,r3 */
    {0xE2933001, 0, 3, 0x7FFFFFFF, 3, 0x80000000, 0x90000000}, /* ADDS r3,r3,#1 */
    {0xE3730001, 0, 3, 0x7FFFFFFF, NO_REG, 0, 0x90000000}, /* CMN V */
    {0xE0B32007, 0x20000000, 3, 0x80000000, 2, 2, 0x30000000}, /* ADCS r2,r3,r7 */
    {0x00932003, 0, 3, 0x80000000, NO_REG, 0, 0}, /* ADDSEQ, Z clear */

    /* Logical S uses shifter carry while preserving V; tests write no Rd. */
    {0xE0132081, 0x30000000, 3, 0xFFFFFFFF, 2, 0x2002, 0x10000000}, /* ANDS LSL #1 */
    {0xE0332021, 0x30000000, 3, 0x80000000, 2, 0x80000000, 0x90000000}, /* EORS LSR #32 */
    {0xE1932061, 0x30000000, 3, 0, 2, 0x80000800, 0xB0000000}, /* ORRS RRX */
    {0xE1D32041, 0x30000000, 3, 0xFFFFFFFF, 2, 0xFFFFFFFF, 0x90000000}, /* BICS ASR #32 */
    {0xE1F02021, 0x30000000, NO_REG, 0, 2, 0xFFFFFFFF, 0x90000000}, /* MVNS LSR #32 */
    {0xE1130081, 0x30000000, 3, 0xFFFFFFFF, NO_REG, 0, 0x10000000}, /* TST */
    {0xE1330027, 0x30000000, 3, 0x80000000, NO_REG, 0, 0xB0000000}, /* TEQ LSR #32 */

    /* Immediate rotates check C updates and V preservation. */
    {0xE1B020E7, 0x10000000, NO_REG, 0, 2, 0xC0000000, 0xB0000000}, /* ROR #1 */
    {0xE1B02FE7, 0x30000000, NO_REG, 0, 2, 3, 0x10000000}, /* ROR #31 */
    {0xE1A020E7, 0x70000000, NO_REG, 0, 2, 0xC0000000, 0x70000000}, /* ROR #1, no S */

    /* The register shifter uses Rs[7:0]. r7 starts at 0x80000001. */
    {0xE1B02311, 0x30000000, 3, 0, 2, 0x1001, 0x30000000}, /* LSL 0 */
    {0xE1B02311, 0x30000000, 3, 32, 2, 0, 0x70000000}, /* LSL 32 */
    {0xE1B02311, 0x30000000, 3, 33, 2, 0, 0x50000000}, /* LSL >32 */
    {0xE1B02337, 0x10000000, 3, 0, 2, 0x80000001, 0x90000000}, /* LSR 0 */
    {0xE1B02337, 0x10000000, 3, 32, 2, 0, 0x70000000}, /* LSR 32 */
    {0xE1B02337, 0x10000000, 3, 33, 2, 0, 0x50000000}, /* LSR >32 */
    {0xE1B02357, 0x10000000, 3, 0, 2, 0x80000001, 0x90000000}, /* ASR 0 */
    {0xE1B02357, 0x10000000, 3, 32, 2, 0xFFFFFFFF, 0xB0000000}, /* ASR 32 */
    {0xE1B02357, 0x10000000, 3, 33, 2, 0xFFFFFFFF, 0xB0000000}, /* ASR >32 */
    {0xE1B02377, 0x10000000, 3, 0, 2, 0x80000001, 0x90000000}, /* ROR 0 */
    {0xE1B02377, 0x10000000, 3, 32, 2, 0x80000001, 0xB0000000}, /* ROR 32 */
    {0xE1B02377, 0x10000000, 3, 33, 2, 0xC0000000, 0xB0000000}, /* ROR >32 */
    {0xE1B02377, 0x10000000, 3, 64, 2, 0x80000001, 0xB0000000}, /* ROR 64 */
    {0xE1A02317, 0x10000000, 3, 32, 2, 0, 0x10000000}, /* MOV, no S */
    {0xE1B02317, 0x10000000, 3, 1, 2, 2, 0x30000000}, /* LSL 1 */
    {0xE1B02311, 0x30000000, 3, 19, 2, 0x80080000, 0x90000000}, /* LSL 19 */
    {0xE1B02337, 0x10000000, 3, 1, 2, 0x40000000, 0x30000000}, /* LSR 1 */
    {0xE1B02337, 0x30000000, 3, 31, 2, 1, 0x10000000}, /* LSR 31 */
    {0xE1B02357, 0x30000000, 3, 4, 2, 0xF8000000, 0x90000000}, /* ASR 4 */

    /* Immediate shifts check C updates. */
    {0xE1B020A7, 0x10000000, NO_REG, 0, 2, 0x40000000, 0x30000000}, /* LSR #1 */
    {0xE1B02107, 0x30000000, NO_REG, 0, 2, 4, 0x10000000}, /* LSL #2 */

    /* ADD with Rm, LSL #1-3 and no S. */
    {0xE0832087, 0x30000000, 3, 5, 2, 7, 0x30000000}, /* ADD LSL #1 */
    {0xE0832187, 0x30000000, 3, 0xFFFFFFF8, 2, 0, 0x30000000}, /* ADD LSL #3 */
    {0xE0833103, 0x30000000, 3, 3, 3, 15, 0x30000000}, /* ADD r3,r3,r3 */
    {0xE1B02067, 0x30000000, NO_REG, 0, 2, 0xC0000000, 0xB0000000}, /* RRX */

    /* Reading r15 depends on whether the shift amount is an immediate or Rs. */
    {0xE1A0208F, 0xB0000000, NO_REG, 0, 2, 0x0C000010, 0xB0000000}, /* PC+8 */
    {0xE1A0231F, 0xB0000000, 3, 1, 2, 0x0C000018, 0xB0000000}, /* PC+12 */
    {0xE08F2311, 0xB0000000, 3, 1, 2, 0x0600200A, 0xB0000000}, /* Rn PC+8 */

    /* Failed conditions cannot change the destination or shifter flags. */
    {0x01B02067, 0x10000000, NO_REG, 0, NO_REG, 0, 0x10000000}, /* MOVEQS RRX */
    {0x12B32000, 0x60000000, 3, 0x7FFFFFFF, NO_REG, 0, 0x60000000}, /* ADCSNE */

    /* Rd equals an operand: C and V need the operands kept. */
    {0xE2533001, 0, 3, 0, 3, 0xFFFFFFFF, 0x80000000}, /* SUBS r3,r3,#1 */
    {0xE0933003, 0, 3, 0x80000000, 3, 0, 0x70000000}, /* ADDS r3,r3,r3 */
};

/* direct uses the flow emitter. */
static bool check_alu_vector_with(uint8_t *memory, const alu_vector *vector,
        bool direct)
{
    uint32_t regs[64] = {0};
    uint32_t before[15];
    for (unsigned i = 0; i < 15; ++i)
        regs[i] = 0x1000 + i;
    regs[7] = 0x80000001; /* Signed shift source in the fixed register map. */
    if (vector->source_reg < 15)
        regs[vector->source_reg] = vector->source_value;
    memcpy(before, regs, sizeof(before));
    regs[15] = ARM_PC;
    regs[16] = vector->initial_flags | 0x1f;

    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, memory, CODE_SIZE);
    if (!(direct ? gpsp_rv32_arm_flow_emit_direct(&code, vector->opcode) :
            gpsp_rv32_emit_arm_alu(&code, vector->opcode, ARM_PC) ==
                    GPSP_RV32_ARM_ALU_EMITTED) ||
            !gpsp_rv32_addi(&code, RV_PC, RV_PC, 4) ||
            !gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, -1) ||
            !gpsp_rv32_jalr(&code, X0, RA, 0) ||
            (rv32_golden_dump(direct ? "smoke_flow" : "smoke_alu", memory,
                    CODE_SIZE, memory, code.ptr), !sync_code(memory, CODE_SIZE)))
        return false;

    uint32_t cycles = gpsp_rv32_run_block(regs, 100, (void (*)(void))memory);
    if (cycles != 99 || regs[15] != ARM_PC + 4 ||
        regs[16] != (vector->final_flags | 0x1f))
        return false;
    for (unsigned i = 0; i < 15; ++i)
    {
        uint32_t expected = i == vector->changed_reg ? vector->changed_value :
                before[i];
        if (regs[i] != expected)
            return false;
    }
    return true;
}

static bool check_alu_vector(uint8_t *memory, const alu_vector *vector)
{
    return check_alu_vector_with(memory, vector, false);
}

static bool check_alu_vectors(uint8_t *memory)
{
    for (unsigned i = 0; i < sizeof(alu_vectors) / sizeof(alu_vectors[0]); ++i)
    {
        if (!check_alu_vector(memory, &alu_vectors[i]))
        {
            ESP_LOGE(TAG, "ALU vector %u failed (opcode=%08lx)", i,
                    (unsigned long)alu_vectors[i].opcode);
            return false;
        }
    }
    /* The flow emitter takes the unconditional forms it supports. */
    unsigned direct_vectors = 0;
    for (unsigned i = 0; i < sizeof(alu_vectors) / sizeof(alu_vectors[0]); ++i)
    {
        if ((alu_vectors[i].opcode >> 28) != 14 ||
            !gpsp_rv32_arm_flow_direct_supported(alu_vectors[i].opcode))
            continue;
        direct_vectors++;
        if (!check_alu_vector_with(memory, &alu_vectors[i], true))
        {
            ESP_LOGE(TAG, "flow ALU vector %u failed (opcode=%08lx)", i,
                    (unsigned long)alu_vectors[i].opcode);
            return false;
        }
    }
    ESP_LOGI(TAG, "flow emitter vectors=%u", direct_vectors);
    ESP_LOGI(TAG, "fixed-map ARM ALU vectors=%u", (unsigned)
            (sizeof(alu_vectors) / sizeof(alu_vectors[0])));
    return true;
}

static bool check_alu_block(uint8_t *memory, uint32_t r1,
            uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t flags)
{
    static const uint32_t program[] = {
        0xE3A00001, /* MOV r0,#1 */
        0xE0902001, /* ADDS r2,r0,r1 */
        0xE2523001, /* SUBS r3,r2,#1 */
        0xE3530000, /* CMP r3,#0 */
        0x03A04055, /* MOVEQ r4,#0x55 */
        0x13A05066, /* MOVNE r5,#0x66 */
    };
    uint32_t regs[64] = {0};
    regs[1] = r1;
    regs[5] = 0xA5;
    regs[15] = ARM_PC;
    regs[16] = 0x1f;

    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, memory, CODE_SIZE);
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
    {
        if (gpsp_rv32_emit_arm_alu(&code, program[i], ARM_PC + i * 4) !=
                GPSP_RV32_ARM_ALU_EMITTED ||
                !gpsp_rv32_addi(&code, RV_PC, RV_PC, 4) ||
                !gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, -1))
            return false;
    }
    if (!gpsp_rv32_jalr(&code, X0, RA, 0) ||
        (rv32_golden_dump("smoke_block", memory, CODE_SIZE, memory, code.ptr),
                !sync_code(memory, CODE_SIZE)))
        return false;

    uint32_t cycles = gpsp_rv32_run_block(regs, 100, (void (*)(void))memory);
    return cycles == 94 && regs[0] == 1 && regs[1] == r1 &&
            regs[2] == r2 && regs[3] == r3 && regs[4] == r4 &&
            regs[5] == r5 && regs[15] == ARM_PC + sizeof(program) &&
            regs[16] == (flags | 0x1f);
}

void app_main(void)
{
#ifdef MALLOC_CAP_EXEC
    uint8_t *memory = heap_caps_aligned_alloc(64, CODE_SIZE,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
    {
        ESP_LOGE(TAG, "executable allocation failed");
        ESP_LOGI(TAG, "ARM RV32 SMOKE RESULT: FAIL");
        ESP_LOGI(TAG, "ARM RV32 SMOKE COMPLETE");
        return;
    }

    bool passed = check_alu_vectors(memory) &&
            check_alu_block(memory, 0, 1, 0, 0x55, 0xA5, 0x60000000) &&
            check_alu_block(memory, 1, 2, 1, 0, 0x66, 0x20000000);

    ESP_LOGI(TAG, "ARM RV32 SMOKE RESULT: %s", passed ? "PASS" : "FAIL");
    heap_caps_free(memory);
#else
    ESP_LOGW(TAG, "MALLOC_CAP_EXEC unavailable");
    ESP_LOGI(TAG, "ARM RV32 SMOKE RESULT: UNAVAILABLE");
#endif
    ESP_LOGI(TAG, "ARM RV32 SMOKE COMPLETE");
}

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdio.h>

#include "rv32_fallback.h"
#include "rv32_register_map.h"

static int check_target(uint32_t target)
{
    uint32_t words[3] = { 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA };
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    if (gpsp_rv32_emit_interpreter_step_stub(&code, target) !=
            GPSP_RV32_FALLBACK_EMITTED || code.ptr != (uint8_t *)&words[2] ||
            words[2] != 0xAAAAAAAA)
        return 0;

    uint32_t lui = words[0], jalr = words[1];
    if ((lui & 0xfff) != ((RV_TMP1 << 7) | 0x37) ||
        (jalr & 0xfffff) != ((RV_TMP1 << 15) | 0x67))
        return 0;
    uint32_t upper = lui & 0xfffff000;
    int32_t lower = (int32_t)jalr >> 20;
    return upper + (uint32_t)lower == target;
}

static int check_atomicity(void)
{
    uint32_t words[3] = { 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA };
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(uint32_t));
    if (gpsp_rv32_emit_interpreter_step_stub(&code, 0x40001000) !=
            GPSP_RV32_FALLBACK_BUFFER_FULL || code.ptr != (uint8_t *)words ||
            words[0] != 0xAAAAAAAA || words[1] != 0xAAAAAAAA)
        return 0;

    gpsp_rv32_code_init(&code, words, sizeof(words));
    if (gpsp_rv32_emit_interpreter_step_stub(&code, 0) !=
            GPSP_RV32_FALLBACK_INVALID ||
            gpsp_rv32_emit_interpreter_step_stub(&code, 0x40001002) !=
                    GPSP_RV32_FALLBACK_INVALID || code.ptr != (uint8_t *)words ||
            words[0] != 0xAAAAAAAA)
        return 0;
#if UINTPTR_MAX > UINT32_MAX
    if (gpsp_rv32_emit_interpreter_step_stub(&code,
            UINT64_C(0x100000000)) != GPSP_RV32_FALLBACK_INVALID ||
            code.ptr != (uint8_t *)words)
        return 0;
#endif
    return 1;
}

int main(void)
{
    const uint32_t targets[] = {
        0x00000004, 0x000007fc, 0x00000800, 0x40001000,
        0x400017fc, 0x40001800, 0x80000800, 0xfffff800,
    };
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (!check_target(targets[i]))
        {
            fprintf(stderr, "fallback target failed: %08x\n", targets[i]);
            return 1;
        }
    if (!check_atomicity())
    {
        fprintf(stderr, "fallback atomicity failed\n");
        return 1;
    }
    puts("RV32 fallback stub encoding PASS");
    return 0;
}

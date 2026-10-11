/* Thumb flow retry spans, using synthetic addresses from retry_symbols.S.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv32_block_emit.c"

uint32_t idle_loop_target_pc = UINT32_MAX;
const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {0};

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static unsigned fixed_words, span_begins, span_ends;
static const uint8_t *span_start, *span_end;

static void record_span(const gpsp_rv32_code *code, bool begin)
{
    if (begin)
    {
        span_begins++;
        span_start = code->ptr;
    }
    else
    {
        span_ends++;
        span_end = code->ptr;
    }
}

static void record_word(const gpsp_rv32_code *code)
{
    fixed_words += code->fixed_depth != 0;
}

static void check_span(uint32_t pc, uint16_t opcode, bool resume,
        unsigned retry_bytes)
{
    uint32_t words[64];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    code.compress = true;
    bool advanced = false;
    fixed_words = span_begins = span_ends = 0;
    span_start = span_end = NULL;

    /* Skip the fetch charge: its wait-table address is a synthetic symbol. */
    CHECK(emit_thumb_memory(&code, pc, opcode, resume, false, &advanced));
    CHECK(code.compress && !code.fixed_depth);
    CHECK(fixed_words * 4 == retry_bytes);
    CHECK(span_begins == (retry_bytes != 0) && span_ends == span_begins);
    if (retry_bytes)
    {
        CHECK(span_start == code.start);
        CHECK(span_end - span_start == (ptrdiff_t)retry_bytes);
        uint32_t call;
        memcpy(&call, span_end - 4, sizeof(call));
        CHECK((call & 0x000fffffu) == ((RV_TMP1 << 15) | (RV_TMP0 << 7) | 0x67));
    }
}

static void check_failure(uint16_t opcode, size_t capacity)
{
    uint32_t words[64];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, capacity);
    code.compress = true;
    bool advanced = false;
    CHECK(!emit_thumb_memory(&code, 0x03000100, opcode, true, false,
            &advanced));
    CHECK(code.ptr == code.start && code.compress && !code.fixed_depth);
}

int main(void)
{
    static const struct { uint16_t opcode; unsigned retry_bytes; } cases[] = {
        {0x4801, 12},  /* LDR literal */
        {0x6808, 12},  /* LDR immediate */
        {0x6001, 16},  /* STR immediate */
        {0x5888, 12},  /* LDR register */
        {0x5088, 16},  /* STR register */
        {0x8808, 12},  /* LDRH */
        {0x8001, 16},  /* STRH */
        {0x9800, 12},  /* LDR SP-relative */
        {0x9000, 16},  /* STR SP-relative */
    };
    gpsp_rv32_test_emit_hook = record_word;
    gpsp_rv32_test_fixed_hook = record_span;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        check_span(0x03000100, cases[i].opcode, true, cases[i].retry_bytes);
        check_span(0x03000100, cases[i].opcode, false, 0);
        if (cases[i].opcode != 0x4801)
            check_span(0x08000100, cases[i].opcode, true, cases[i].retry_bytes);
    }
    check_span(0x08000100, 0x4801, false, 0);

    idle_loop_target_pc = 0x03000102;
    check_span(0x03000100, 0x6808, false, 0);
    check_span(0x03000100, 0x6001, false, 0);
    check_failure(0x6808, 256);
    idle_loop_target_pc = UINT32_MAX;

    /* Fail inside the span and after the load span, at its result move. */
    for (size_t capacity = 0; capacity < 16; capacity += 4)
    {
        check_failure(0x6808, capacity);
        check_failure(0x6001, capacity);
    }
    check_failure(0xffff, 256);
    puts("Thumb flow retry spans: ok");
    return 0;
}

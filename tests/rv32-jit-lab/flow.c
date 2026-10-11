/* Branch-block Thumb IR code size and optional host translation time.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "rv32_thumb_flow_emit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
    START_PC = 0x08000100,
    SEQ_WAIT_ADDRESS = 0x20001230,
    NSEQ_WAIT_ADDRESS = 0x20001330,
    STEP_STUB_ADDRESS = 0x30000000,
};

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

typedef struct
{
    gpsp_rv32_thumb_flow_layout layout;
    size_t side_bytes;
    unsigned side_count;
} flow_result;

__attribute__((noinline)) static flow_result translate_flow(
        const uint16_t *opcodes, size_t count,
        const gpsp_rv32_thumb_flow_environment *environment,
        bool all_entries, bool reverse_entries, bool dump,
        uint32_t main_words[512])
{
    gpsp_rv32_thumb_ir ir;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, count, &ir))
        fail("IR decode failed");
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, main_words, sizeof(uint32_t[512]));
    flow_result result = {0};
    if (!gpsp_rv32_thumb_flow_emit(&ir, environment, &code, &result.layout))
        fail("flow emission failed");

    uint64_t published = UINT64_C(1) << count;
    for (unsigned i = 0; i < count; i++)
        if (result.layout.entry_offset[i] != UINT16_MAX)
            published |= UINT64_C(1) << i;
    if (all_entries)
        for (unsigned order = 1; order < count; order++)
        {
            unsigned i = reverse_entries ? (unsigned)count - order : order;
            if (published & (UINT64_C(1) << i))
                continue;
            unsigned join = i + 1;
            while (!(published & (UINT64_C(1) << join)))
                join++;
            uint32_t side_words[512];
            gpsp_rv32_code side_code;
            gpsp_rv32_code_init(&side_code, side_words, sizeof(side_words));
            gpsp_rv32_thumb_flow_side_layout side;
            if (!gpsp_rv32_thumb_flow_emit_side(&ir, environment, i, join,
                    published,
                    &result.layout, &side_code, &side))
                fail("side emission failed");
            result.side_bytes += side.code_bytes;
            result.side_count++;
            published |= UINT64_C(1) << i;
            if (dump)
                printf("side[%u] guest_ops=%u bytes=%u links=%u\n", i,
                        side.link_target[side.link_count - 1] - i,
                        side.code_bytes, side.link_count);
        }
    return result;
}

static unsigned parse_iterations(const char *value)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || end == value || *end || !parsed || parsed > 100000)
        fail("iterations must be in 1..100000");
    return (unsigned)parsed;
}

static uint64_t process_cpu_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now))
        fail("clock_gettime failed");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

int main(int argc, char **argv)
{
    bool cap32 = false, all_entries = false, reverse_entries = false;
    bool dump = false;
    unsigned iterations = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--cap32"))
            cap32 = true;
        else if (!strcmp(argv[i], "--all-entries"))
            all_entries = true;
        else if (!strcmp(argv[i], "--reverse-entries"))
            reverse_entries = true;
        else if (!strcmp(argv[i], "--dump"))
            dump = true;
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            iterations = parse_iterations(argv[++i]);
        else
            fail("usage: build/flow [--cap32] [--all-entries] "
                    "[--reverse-entries] [--dump] [--iterations N]");
    }

    uint16_t opcodes[GPSP_RV32_THUMB_IR_MAX_OPS] = {
        0x4200, 0xd000, 0x2102, 0x2203
    };
    size_t count = cap32 ? GPSP_RV32_THUMB_IR_MAX_OPS : 4;
    if (cap32)
        for (unsigned i = 0; i < count; i++)
            opcodes[i] = 0x2000u | ((i & 7u) << 8) | (i + 1u);
    gpsp_rv32_thumb_flow_environment environment = {
        .sequential_wait_address = SEQ_WAIT_ADDRESS,
        .nonsequential_wait_address = NSEQ_WAIT_ADDRESS,
        .interpreter_stub_address = STEP_STUB_ADDRESS,
        .idle_loop_target_pc = UINT32_MAX,
    };
    uint32_t main_words[512];
    flow_result result = translate_flow(opcodes, count, &environment,
            all_entries, reverse_entries, dump, main_words);

    size_t total = result.layout.code_bytes + result.side_bytes;
    printf("translator=thumb_flow fixture=%s guest_ops=%zu "
            "main_bytes=%u main_words=%u blocks=%u side_entries=%u "
            "side_bytes=%zu code_bytes=%zu rv32_words=%zu "
            "cache_header_bytes=8 cache_total_bytes=%zu side_order=%s\n",
            cap32 ? "cap32" : "branch4", count, result.layout.code_bytes,
            result.layout.code_bytes / 4, result.layout.block_count,
            result.side_count, result.side_bytes, total, total / 4, total + 8,
            reverse_entries ? "descending" : "ascending");
    printf("ir_bytes=%zu layout_bytes=%zu side_layout_bytes=%zu "
            "lab_output_capacity_bytes=%zu\n",
            sizeof(gpsp_rv32_thumb_ir), sizeof(gpsp_rv32_thumb_flow_layout),
            sizeof(gpsp_rv32_thumb_flow_side_layout),
            sizeof(main_words) + sizeof(uint32_t[512]));
    printf("memory_note=output_capacity_is_not_peak_stack;"
            "emitter_frames_excluded\n");
    if (dump)
    {
        for (unsigned i = 0; i < count; i++)
            if (result.layout.entry_offset[i] != UINT16_MAX)
                printf("entry[%u]=%u\n", i, result.layout.entry_offset[i]);
        printf("exit_link_offset=%u\n", result.layout.exit_link_offset);
        for (unsigned i = 0; i < result.layout.code_bytes / 4; i++)
            printf("%04x: %08x\n", i * 4, main_words[i]);
    }
    if (iterations)
    {
        for (unsigned i = 0; i < 32; i++)
            result = translate_flow(opcodes, count, &environment, all_entries,
                    reverse_entries, false, main_words);
        uint64_t start = process_cpu_ns();
        size_t emitted = 0;
        for (unsigned i = 0; i < iterations; i++)
        {
            result = translate_flow(opcodes, count, &environment, all_entries,
                    reverse_entries, false, main_words);
            emitted += result.layout.code_bytes + result.side_bytes;
        }
        uint64_t cpu_ns = process_cpu_ns() - start;
        printf("translation_scope=decode_main_emit%s "
                "iterations=%u cpu_ns_total=%" PRIu64
                " cpu_ns_per_translation=%.1f emitted_bytes=%zu\n",
                all_entries ? "_side_emit" : "", iterations, cpu_ns,
                (double)cpu_ns / iterations, emitted);
    }
    return 0;
}

/* Four-instruction Thumb IR translation budget.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "lab_report.h"
#include "rv32_thumb_ir_emit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
    START_PC = 0x08000100,
    SEQ_WAIT_ADDRESS = 0x20001240,
    NSEQ_WAIT_ADDRESS = 0x20001340,
    STEP_STUB_ADDRESS = 0x30000000,
    TRIALS = 5,
};

static const uint16_t thumb_opcodes[LAB_THUMB_COUNT] = {
    0x4200, 0xd000, 0x2102, 0x2203
};

typedef struct
{
    uint32_t code[64];
    gpsp_rv32_thumb_ir ir;
    gpsp_rv32_thumb_ir_layout layout;
} ir_workspace;

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static uint64_t clock_ns(clockid_t kind)
{
    struct timespec now;
    if (clock_gettime(kind, &now))
        fail("clock_gettime failed");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static int compare_u64(const void *lhs, const void *rhs)
{
    uint64_t a = *(const uint64_t *)lhs;
    uint64_t b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}

__attribute__((noinline)) static void translate_ir(ir_workspace *work,
        uint8_t entry_mask, bool decode, bool append)
{
    if (decode && !gpsp_rv32_thumb_ir_decode(START_PC, thumb_opcodes,
            LAB_THUMB_COUNT, &work->ir))
        fail("IR decode failed");
    gpsp_rv32_thumb_ir_environment environment = {
        .sequential_wait_address = SEQ_WAIT_ADDRESS,
        .nonsequential_wait_address = NSEQ_WAIT_ADDRESS,
        .interpreter_stub_address = STEP_STUB_ADDRESS,
        .idle_loop_target_pc = UINT32_MAX,
        .entry_mask = append ? 1 : entry_mask,
    };
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, work->code, sizeof(work->code));
    if (!gpsp_rv32_thumb_ir_emit(&work->ir, &environment, &code,
            &work->layout))
        fail("IR emission failed");
    if (append)
        for (unsigned i = 1; i < LAB_THUMB_COUNT; i++)
            if ((entry_mask & (1U << i)) &&
                !gpsp_rv32_thumb_ir_append_entry(&work->ir, &environment, i,
                            &code, &work->layout))
                fail("IR side entry append failed");
}

static void time_translations(ir_workspace *work, uint8_t entry_mask,
        unsigned iterations, bool decode, bool append, uint64_t *cpu_median,
        uint64_t *wall_median)
{
    uint64_t cpu[TRIALS], wall[TRIALS];
    for (unsigned trial = 0; trial < TRIALS; trial++)
    {
        for (unsigned i = 0; i < 256; i++)
            translate_ir(work, entry_mask, decode, append);
        uint64_t wall_start = clock_ns(CLOCK_MONOTONIC);
        uint64_t cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        for (unsigned i = 0; i < iterations; i++)
            translate_ir(work, entry_mask, decode, append);
        cpu[trial] = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
        wall[trial] = clock_ns(CLOCK_MONOTONIC) - wall_start;
    }
    qsort(cpu, TRIALS, sizeof(cpu[0]), compare_u64);
    qsort(wall, TRIALS, sizeof(wall[0]), compare_u64);
    *cpu_median = cpu[TRIALS / 2];
    *wall_median = wall[TRIALS / 2];
}

static unsigned parse_iterations(const char *value)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || end == value || *end || !parsed || parsed > 1000000)
        fail("iterations must be in 1..1000000");
    return (unsigned)parsed;
}

int main(int argc, char **argv)
{
    unsigned iterations = 100000;
    uint8_t entry_mask = 15;
    bool dump = false;
    bool append = false;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--dump"))
            dump = true;
        else if (!strcmp(argv[i], "--append"))
            append = true;
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            iterations = parse_iterations(argv[++i]);
        else if (!strcmp(argv[i], "--entries") && i + 1 < argc)
        {
            const char *mode = argv[++i];
            if (!strcmp(mode, "head"))
                entry_mask = 1;
            else if (!strcmp(mode, "head-join"))
                entry_mask = 9;
            else if (!strcmp(mode, "all"))
                entry_mask = 15;
            else
                fail("entries must be head, head-join, or all");
        }
        else
            fail("usage: build/ir [--entries head|head-join|all] "
                    "[--append] [--dump] [--iterations N]");
    }

    ir_workspace work;
    translate_ir(&work, entry_mask, true, append);
    uint64_t cpu_ns, wall_ns, emit_cpu_ns, emit_wall_ns;
    time_translations(&work, entry_mask, iterations, true, append,
            &cpu_ns, &wall_ns);
    time_translations(&work, entry_mask, iterations, false, append,
            &emit_cpu_ns, &emit_wall_ns);

    lab_report_data report = {
        .name = append ? "thumb_ir_append" : "thumb_ir",
        .segment_label = "entry_region",
        .code = (uint8_t *)work.code,
        .code_bytes = work.layout.code_bytes,
        .cache_header_bytes = 8,
        .link_slots = 1,
        .cold_bytes = 8,
        .workspace_capacity_bytes = sizeof(work),
        .emitter_stage_capacity_bytes = work.layout.staging_bytes,
        .metadata_bytes = sizeof(work.ir) + sizeof(work.layout),
        .wall_ns = wall_ns,
        .cpu_ns = cpu_ns,
        .iterations = iterations,
        .trials = TRIALS,
    };
    for (unsigned i = 0; i < LAB_THUMB_COUNT; i++)
    {
        uint16_t offset = work.layout.entry_offset[i];
        report.entry_offsets[i] = offset == UINT16_MAX ? SIZE_MAX : offset;
        if (offset == UINT16_MAX)
            continue;
        size_t next = work.layout.code_bytes;
        for (unsigned j = i + 1; j < LAB_THUMB_COUNT; j++)
            if (work.layout.entry_offset[j] != UINT16_MAX)
            {
                next = work.layout.entry_offset[j];
                break;
            }
        report.instruction_bytes[i] = next - offset;
    }
    lab_print_report(&report, dump);
    printf("entry_mask=0x%x append=%u ir_bytes=%zu "
            "emit_only_cpu_ns_per_translation=%.1f "
            "emit_only_wall_ns_per_translation=%.1f\n", entry_mask,
            (unsigned)append, sizeof(work.ir), (double)emit_cpu_ns / iterations,
            (double)emit_wall_ns / iterations);
    return 0;
}

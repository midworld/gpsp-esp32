/* Current RV32 block emitter on a four-instruction Thumb sample.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "lab_report.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Include the emitter just as the existing host emitter test does. */
#include "rv32_block_emit.c"

/* Only their addresses are encoded. No host code reads these symbols. */
#if defined(__APPLE__)
__asm__(".globl _ws_cyc_seq\n_ws_cyc_seq = 0x20001230\n"
        ".globl _ws_cyc_nseq\n_ws_cyc_nseq = 0x20001330\n");
#else
__asm__(".globl ws_cyc_seq\n.set ws_cyc_seq, 0x20001230\n"
        ".globl ws_cyc_nseq\n.set ws_cyc_nseq, 0x20001330\n");
#endif

uint32_t idle_loop_target_pc;

/* ARM and memory paths are referenced by the common block emitter. */
gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu_block(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_ALU_UNSUPPORTED;
}

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;
}

gpsp_rv32_arm_mul_result gpsp_rv32_emit_arm_mul(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MUL_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_thumb_swi(
        gpsp_rv32_code *code, uint16_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {0};

#define UNUSED_RV32_ENTRY(name) void name(void) { abort(); }
UNUSED_RV32_ENTRY(gpsp_rv32_call_helper)
UNUSED_RV32_ENTRY(gpsp_rv32_interpreter_step_stub)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram_store)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_slow_bridge)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_pop_exit_shared)
#undef UNUSED_RV32_ENTRY

enum { START_PC = 0x08000100, VIRTUAL_CODE_BASE = 0x30000000 };
static const uint16_t thumb_opcodes[LAB_THUMB_COUNT] = {
    0x4200, 0xd000, 0x2102, 0x2203
};

typedef struct
{
    uint32_t code[64];
    size_t entry_offsets[LAB_THUMB_COUNT];
    size_t instruction_bytes[LAB_THUMB_COUNT];
    gpsp_rv32_block_link link;
    size_t code_bytes;
} current_workspace;

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

/* One translation includes the internal branch patch and fallthrough slot. */
__attribute__((noinline)) static void translate_current(current_workspace *work)
{
    uint8_t *start = (uint8_t *)work->code;
    uint8_t *cursor = start;
    uint8_t *limit = start + sizeof(work->code);
    work->link.site = NULL;

    for (unsigned i = 0; i < LAB_THUMB_COUNT; i++)
    {
        bool ends_block = false;
        gpsp_rv32_block_link link;
        uint32_t pc = START_PC + i * 2;
        work->entry_offsets[i] = (size_t)(cursor - start);
        if (!gpsp_rv32_emit_block_instruction(&cursor, limit, pc,
                thumb_opcodes[i], true, true, 0xf, false, &ends_block, &link))
            fail("instruction emission failed");
        if (ends_block || (link.site && work->link.site))
            fail("unexpected block exit or second link");
        if (link.site)
            work->link = link;
        work->instruction_bytes[i] = (size_t)(cursor - start) -
                work->entry_offsets[i];
    }

    gpsp_rv32_code gate;
    uint8_t *fallthrough_site = NULL;
    gpsp_rv32_code_init(&gate, cursor, (size_t)(limit - cursor));
    if (!gpsp_rv32_emit_link_slot(&gate, &fallthrough_site) ||
        !fallthrough_site || !work->link.site ||
            work->link.target != START_PC + 6 ||
            !work->link.thumb)
        fail("fallthrough slot or internal branch missing");
    cursor = gate.ptr;

    work->code_bytes = (size_t)(cursor - start);
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, start, sizeof(work->code));
    code.ptr = cursor;
    if (!gpsp_rv32_patch_link_slot(&code, work->link.site,
            VIRTUAL_CODE_BASE + work->entry_offsets[3]))
        fail("internal branch patch failed");
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

static uint64_t median(uint64_t *samples, unsigned count)
{
    for (unsigned i = 1; i < count; i++)
    {
        uint64_t value = samples[i];
        unsigned j = i;
        while (j && samples[j - 1] > value)
        {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    return samples[count / 2];
}

int main(int argc, char **argv)
{
    unsigned iterations = 100000;
    unsigned trials = 5;
    bool dump = false;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--dump"))
            dump = true;
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            iterations = parse_iterations(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc)
        {
            trials = parse_iterations(argv[++i]);
            if (trials > 9)
                fail("trials must be in 1..9");
        }
        else
            fail("usage: build/current [--dump] [--iterations N] [--trials N]");
    }

    current_workspace work;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    struct gpsp_rv32_emit_byte_stats before, after;
    gpsp_rv32_emit_get_byte_stats(&before);
#endif
    translate_current(&work);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    gpsp_rv32_emit_get_byte_stats(&after);
#endif
    for (unsigned i = 0; i < 256; i++)
        translate_current(&work);

    uint64_t cpu_samples[9], wall_samples[9];
    for (unsigned trial = 0; trial < trials; trial++)
    {
        uint64_t wall_start = clock_ns(CLOCK_MONOTONIC);
        uint64_t cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        for (unsigned i = 0; i < iterations; i++)
            translate_current(&work);
        cpu_samples[trial] = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
        wall_samples[trial] = clock_ns(CLOCK_MONOTONIC) - wall_start;
    }

    lab_report_data report = {
        .name = "current",
        .code = (uint8_t *)work.code,
        .code_bytes = work.code_bytes,
        .cache_header_bytes = 8,
        .link_slots = 2,
        .cold_bytes = 0,
        .workspace_capacity_bytes = sizeof(work),
        .emitter_stage_capacity_bytes = sizeof(uint32_t[256]),
        .metadata_bytes = sizeof(work.entry_offsets) +
                sizeof(work.instruction_bytes) + sizeof(work.link),
        .wall_ns = median(wall_samples, trials),
        .cpu_ns = median(cpu_samples, trials),
        .iterations = iterations,
        .trials = trials,
    };
    memcpy(report.entry_offsets, work.entry_offsets,
            sizeof(report.entry_offsets));
    memcpy(report.instruction_bytes, work.instruction_bytes,
            sizeof(report.instruction_bytes));
    lab_print_report(&report, dump);
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    uint64_t emitted = after.guard - before.guard +
            after.body - before.body + after.timing - before.timing +
            after.link_exit - before.link_exit;
    if (emitted > work.code_bytes)
        fail("byte accounting exceeded block size");
    printf("emitter_bytes guard=%" PRIu64 " body=%" PRIu64
            " timing=%" PRIu64 " link_exit=%" PRIu64 " tail=%zu\n",
            after.guard - before.guard, after.body - before.body,
            after.timing - before.timing,
            after.link_exit - before.link_exit,
            work.code_bytes - (size_t)emitted);
#endif
    printf("host_wait_table_virtual_addresses=0x20001230,0x20001330\n");
    return 0;
}

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ARM_MEMORY_H
#define GPSP_RV32_ARM_MEMORY_H

#include "rv32_codegen.h"

typedef enum
{
  GPSP_RV32_ARM_MEMORY_EMITTED,
  GPSP_RV32_ARM_MEMORY_PC_EXIT,
  GPSP_RV32_ARM_MEMORY_UNSUPPORTED,
  GPSP_RV32_ARM_MEMORY_BUFFER_FULL,
} gpsp_rv32_arm_memory_result;

/* Translate ordinary ARM single and block transfers. The caller charges
 * instruction fetch time; PC_EXIT supplies a0=target and a1=taken. */
gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc);

/* Same, for code whose RV_PC lags the instruction PC by pc_delta. Helper
   paths sync it and restore it when the block continues. A helper exit
   first adds exit_refund cycles back to the budget. */
gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory_lazy(
    gpsp_rv32_code *code, uint32_t opcode, uint32_t instruction_pc,
    int32_t pc_delta, int32_t exit_refund);

#if defined(GPSP_RV32_TEST_HOOKS)
/* Tests: false emits transfers without short branches; a nonzero size
   shrinks the staging buffer. */
extern bool gpsp_rv32_test_arm_short_branch;
extern size_t gpsp_rv32_test_arm_staging_bytes;
#endif

#if defined(GPSP_BENCHMARK) || defined(GPSP_RV32_TEST_HOOKS)
/* Transfers emitted with short branches first, those emitted again with
   full-length branches, and retried ones that still failed, including a
   full parent buffer. */
typedef struct
{
  uint32_t attempts, retries, retry_failures;
} gpsp_rv32_arm_short_branch_stats;

extern gpsp_rv32_arm_short_branch_stats gpsp_rv32_arm_short_branch;
#endif
#if defined(GPSP_BENCHMARK)
void gpsp_rv32_arm_short_branch_report(void);
#endif

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
/* Indices are palette/VRAM/OAM, read/write, then 8/16/32-bit width. */
struct gpsp_rv32_arm_video_memory_stats
{
  uint64_t calls[3][2][3];
  uint64_t cycles[3][2][3];
};

/* Indices are palette/VRAM/OAM. PC 0x7B4 is the built-in BIOS CpuFastSet store. */
struct gpsp_rv32_arm_video_source_stats
{
  uint64_t bios_store_calls[3];
  uint64_t bios_store_cycles[3];
  uint64_t pc_7b4_store_calls[3];
  uint64_t pc_7b4_store_cycles[3];
};

#ifdef __cplusplus
extern "C" {
#endif
void gpsp_rv32_arm_video_memory_get_stats(
    struct gpsp_rv32_arm_video_memory_stats *stats);
void gpsp_rv32_arm_video_source_get_stats(
    struct gpsp_rv32_arm_video_source_stats *stats);
#ifdef __cplusplus
}
#endif
#endif

#endif

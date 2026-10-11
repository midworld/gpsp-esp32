/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_STUB_H
#define GPSP_RV32_STUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A helper sees the precise guest state in regs[0..16]. It may update regs,
 * CPSR, PC, banked registers and *cycles. Low 32 return bits are a value for
 * the generated block; any nonzero high 32 bits request redispatch. */
typedef uint64_t (*gpsp_rv32_helper_fn)(uint32_t *regs, uint32_t *cycles,
    uint32_t arg0, uint32_t arg1);

#define GPSP_RV32_HELPER_EXIT (UINT64_C(1) << 32)

#if defined(GPSP_RV32_JIT)
/* High return words reserved for an already completed scheduler update. */
#define GPSP_RV32_BUDGET_DISPATCH_RETURN 2u
#define GPSP_RV32_BUDGET_FRAME_RETURN UINT32_MAX
#endif

/* Enter a block or saved guard continuation. Return remaining cycles and the
 * next continuation, and store guest state in regs. */
uint64_t gpsp_rv32_run_block(uint32_t *regs, uint32_t cycles,
    void (*block)(void));

/* Non-C calling convention for emitted code only:
 *   a0 = gpsp_rv32_helper_fn, a1 = arg0, a2 = arg1
 *   call gpsp_rv32_call_helper
 *   a0 = low 32 result bits, a1 != 0 means exit the current block now.
 * The call preserves the JIT register map and ra's eventual block-exit
 * target. a0-a2 and RV_TMP0/RV_TMP1 are scratch.
 * Blocks must test a1 before executing another guest instruction. */
void gpsp_rv32_call_helper(void);
void gpsp_rv32_thumb_multi_slow_bridge(void);
void gpsp_rv32_thumb_side_preflight_stub(void);

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
/* Tag a generated memory read for ROM-specific bridge timing. */
void gpsp_rv32_call_memory_read_helper(void);
#endif

/* Correctness-first slow path for any guest opcode. Execute the current
 * ARM/Thumb instruction through gpSP's interpreter and always redispatch. */
uint64_t gpsp_rv32_interpret_one_helper(uint32_t *regs, uint32_t *cycles,
    uint32_t arg0, uint32_t arg1);

#ifdef __cplusplus
}
#endif

#endif

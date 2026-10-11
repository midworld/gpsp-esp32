/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_INTERPRETER_STUB_H
#define GPSP_RV32_INTERPRETER_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Tail-call target for a generated block's interpreter fallback. */
void gpsp_rv32_interpreter_step_stub(void);

#ifdef __cplusplus
}
#endif

#endif

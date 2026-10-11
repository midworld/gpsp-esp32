/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_MULTI_RAM_H
#define GPSP_RV32_THUMB_MULTI_RAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Generated-code ABI: A1 is the aligned first guest address; A2 is the
 * word count (1..9). Call with RV_TMP1 as the link register. A0 returns the
 * contiguous host pointer or zero; A1 returns total memory cycles on hit. */
void gpsp_rv32_thumb_multi_ram(void);
/* Store variant rejects any range containing translated RAM code. */
void gpsp_rv32_thumb_multi_ram_store(void);

#ifdef __cplusplus
}
#endif

#endif

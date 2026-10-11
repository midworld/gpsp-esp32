/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_BIOS_PROBE_H
#define GPSP_RV32_BIOS_PROBE_H

#include <stdint.h>

#if defined(GPSP_BENCHMARK_JIT_DETAIL)
#ifdef __cplusplus
extern "C" {
#endif

struct gpsp_rv32_bios_probe_site
{
  uint32_t pc;
  uint32_t calls;
};

struct gpsp_rv32_bios_probe_stats
{
  uint64_t block_calls;
  uint64_t block_to_bios;
  uint64_t block_bios_to_bios;
  uint64_t block_bios_to_bios_ready;
  uint64_t block_bios_to_other;
  uint64_t block_bios_to_other_ready;
  uint64_t update_to_bios;
  uint64_t update_from_bios;
  uint64_t irq_to_bios;
  uint64_t irq_from_bios;
  uint64_t interpreter_to_bios;
  uint64_t interpreter_from_bios;
  struct gpsp_rv32_bios_probe_site top_blocks[8];
  uint64_t fastset_outer_from_7a8_ready;
  uint64_t fastset_outer_from_7b8_ready;
  uint64_t fastset_inner_from_7b8_ready;
  uint64_t swi_entry_to_64_ready;
};

void gpsp_rv32_bios_probe_reset(void);
void gpsp_rv32_bios_probe_get_stats(
    struct gpsp_rv32_bios_probe_stats *stats);

#ifdef __cplusplus
}
#endif
#endif

#endif

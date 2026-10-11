/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_CYCLE_BUDGET_H
#define GPSP_RV32_CYCLE_BUDGET_H

/* Frame state, independent of translation cache lifetime. */
extern unsigned gpsp_rv32_frame_cycle_debt;

static inline int gpsp_rv32_take_cycles(unsigned cycles, unsigned *frame_debt)
{
  int remaining = (int)cycles - (int)*frame_debt;
  *frame_debt = 0;
  return remaining;
}

/* Stop at the first frame boundary and carry instruction overshoot.
   frame_start_cycles() separately charges pending DMA and IRQ stalls. */
static inline unsigned gpsp_rv32_update_cycles(int remaining,
    unsigned (*update)(int), const unsigned *execute_cycles,
    unsigned *frame_debt)
{
  unsigned debt = remaining < -64 ? (unsigned)-(remaining + 64) : 0;
  unsigned next = update(debt ? -64 : remaining);
  unsigned status = next & ~0x7fffu;
  unsigned budget = next & 0x7fffu;
  while (!(status & 0x80000000u) && debt > budget)
  {
    unsigned slice = debt - budget > 64 ? budget + 64 : debt;
    next = update((int)budget - (int)slice);
    debt -= slice;
    status |= next & ~0x7fffu;
    budget = next & 0x7fffu;
  }
  if (status & 0x80000000u)
  {
    *frame_debt = debt + *execute_cycles - budget;
    return status | budget;
  }
  return status | (budget - debt);
}

#endif

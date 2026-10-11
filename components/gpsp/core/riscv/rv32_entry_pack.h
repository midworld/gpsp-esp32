/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ENTRY_PACK_H
#define GPSP_RV32_ENTRY_PACK_H

#include <stdbool.h>
#include <stdint.h>

/* Lookup entries store code-cache offsets and sizes in four-byte units.
 * Check a value before anything that depends on it is published. */

/* An aligned offset whose four-byte unit count fits in 1..32 bits. */
static inline bool gpsp_rv32_packed_offset_valid(uint32_t offset,
    unsigned bits)
{
  return bits >= 1 && bits <= 32 &&
      offset != UINT32_MAX && !(offset & 3) &&
      (offset >> 2) <= (UINT32_MAX >> (32 - bits));
}

/* Slow entry: offset in the high 22 bits, size in the low 10. */
#define GPSP_RV32_SLOW_SIZE_BITS 10
#define GPSP_RV32_SLOW_SIZE_MASK ((1U << GPSP_RV32_SLOW_SIZE_BITS) - 1)

/* False leaves value unchanged. */
static inline bool gpsp_rv32_slow_pack(uint32_t offset, uint32_t size,
    uint32_t *value)
{
  if (!gpsp_rv32_packed_offset_valid(offset, 32 - GPSP_RV32_SLOW_SIZE_BITS) ||
      (size & 3) || size / 4 > GPSP_RV32_SLOW_SIZE_MASK)
    return false;
  *value = ((offset / 4) << GPSP_RV32_SLOW_SIZE_BITS) | size / 4;
  return true;
}

static inline uint32_t gpsp_rv32_slow_offset(uint32_t value)
{
  return (value >> GPSP_RV32_SLOW_SIZE_BITS) * 4;
}

static inline uint32_t gpsp_rv32_slow_size(uint32_t value)
{
  return (value & GPSP_RV32_SLOW_SIZE_MASK) * 4;
}

#endif

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_CODE_ARENA_H
#define GPSP_RV32_CODE_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../gpsp_config.h"

/* Offsets count through equal chunks in order. Blocks stay in one chunk;
 * link slots reach code in any chunk. */

#define GPSP_RV32_CHUNK_BYTES (1024u * GPSP_RV32_CODE_CHUNK_KIB)
#define GPSP_RV32_ARENA_MAX 64

typedef struct
{
  uint8_t *base[GPSP_RV32_ARENA_MAX];
  unsigned count;    /* allocated chunks */
  unsigned max;
  unsigned current;  /* chunk being filled */
} gpsp_rv32_arena;

#ifdef __cplusplus
extern "C" {
#endif

extern gpsp_rv32_arena gpsp_rv32_rom_arena;
extern gpsp_rv32_arena gpsp_rv32_ram_arena;

/* Adds a chunk; false at the limit or when allocation fails. */
bool gpsp_rv32_arena_grow(gpsp_rv32_arena *arena);

#ifdef __cplusplus
}
#endif

static inline uint8_t *gpsp_rv32_arena_at(const gpsp_rv32_arena *arena,
    uint32_t offset)
{
  return arena->base[offset / GPSP_RV32_CHUNK_BYTES] +
      offset % GPSP_RV32_CHUNK_BYTES;
}

/* UINT32_MAX when the address is in no chunk. */
static inline uint32_t gpsp_rv32_arena_offset(const gpsp_rv32_arena *arena,
    const void *code)
{
  for (unsigned i = 0; i < arena->count; i++)
  {
    uintptr_t delta = (uintptr_t)code - (uintptr_t)arena->base[i];
    if (delta < GPSP_RV32_CHUNK_BYTES)
      return i * GPSP_RV32_CHUNK_BYTES + (uint32_t)delta;
  }
  return UINT32_MAX;
}

static inline bool gpsp_rv32_arena_contains(const gpsp_rv32_arena *arena,
    const void *code)
{
  return gpsp_rv32_arena_offset(arena, code) != UINT32_MAX;
}

/* True when [begin, end) lies in one chunk. */
static inline bool gpsp_rv32_arena_holds(const gpsp_rv32_arena *arena,
    const void *begin, const void *end)
{
  uint32_t first = gpsp_rv32_arena_offset(arena, begin);
  return first != UINT32_MAX &&
      (uintptr_t)end - (uintptr_t)begin <=
          GPSP_RV32_CHUNK_BYTES - first % GPSP_RV32_CHUNK_BYTES;
}

/* End of the chunk being filled. */
static inline uint8_t *gpsp_rv32_arena_end(const gpsp_rv32_arena *arena)
{
  return arena->base[arena->current] + GPSP_RV32_CHUNK_BYTES;
}

static inline uint32_t gpsp_rv32_arena_capacity(const gpsp_rv32_arena *arena)
{
  return arena->count * GPSP_RV32_CHUNK_BYTES;
}

#endif

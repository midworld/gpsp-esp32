/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_RAM_UNITS_H
#define GPSP_RV32_RAM_UNITS_H

#include <stdbool.h>
#include <stdint.h>

/* RAM translation units: the guest range and native code of each RAM block,
 * so a code write can find the blocks it affects. A dropped unit stays
 * dormant with a copy of its guest bytes; when the same bytes come back,
 * its native code is used again instead of a new translation. Guest
 * addresses are physical: 0x02000000 | (pc & 0x3ffff) or
 * 0x03000000 | (pc & 0x7fff). */

#define GPSP_RV32_RAM_UNITS 4096
#define GPSP_RV32_RAM_LINKS 16384
#define GPSP_RV32_RAM_DIRTY 16

/* A guest key (pc | thumb) and a RAM cache offset. */
typedef struct
{
  uint32_t key, offset;
} gpsp_rv32_ram_ref;

/* Native code of one ARM op whose immediate can change in place. */
typedef struct
{
  uint32_t guest;                     /* physical address */
  uint32_t native;                    /* RAM cache offset */
  uint8_t bytes, mode;                /* mode: GPSP_RV32_ARM_FLOW_OP_* */
  uint8_t live;                       /* branch: flags live at its target */
} gpsp_rv32_ram_patch;

typedef struct
{
  uint32_t guest_start, guest_end;    /* scanned range */
  uint32_t native_start, native_end;  /* RAM cache offsets */
  uint32_t head;                      /* head entry offset, or 0 */
  uint32_t hash, copy;                /* guest bytes and their copy */
  uint32_t first_ref, first_patch;
  uint16_t first_node;                /* page nodes, one per page */
  uint16_t entries, exits;            /* refs: entries, then exit sites */
  uint16_t patches;
  bool thumb, live;
  uint8_t switches;                   /* slot-list length at translation */
} gpsp_rv32_ram_unit;

/* Forget all units; called with a full RAM cache flush. */
void gpsp_rv32_ram_units_reset(void);

/* Entry keys and exit link sites of the block being translated, kept
 * until gpsp_rv32_ram_unit_add stores them with the unit. */
void gpsp_rv32_ram_stage_begin(void);
void gpsp_rv32_ram_stage_entry(uint32_t key, uint32_t offset);
void gpsp_rv32_ram_stage_exit(uint32_t key, uint32_t site);
/* Patch points are optional; ones that do not fit are left out. */
void gpsp_rv32_ram_stage_patch(uint32_t pc, uint32_t native,
    unsigned bytes, unsigned mode, unsigned live);
/* Mode of the staged patch whose code ends at site, or 0. */
unsigned gpsp_rv32_ram_stage_mode_before(uint32_t site);

/* guest points at the block's bytes. False when a table is full; the
   caller then needs a full flush. */
bool gpsp_rv32_ram_unit_add(uint32_t start_pc, uint32_t end_pc,
    uint32_t native_start, uint32_t native_end, uint32_t head, bool thumb,
    unsigned switches, const uint8_t *guest);

/* Live unit ids whose guest range overlaps [address, address + bytes). */
unsigned gpsp_rv32_ram_units_find(uint32_t address, unsigned bytes,
    uint16_t *ids, unsigned max_ids);

/* Dormant unit ids of one mode whose guest range holds pc. */
unsigned gpsp_rv32_ram_units_dormant(uint32_t pc, bool thumb,
    uint16_t *ids, unsigned max_ids);

/* True when guest holds the bytes the unit was translated from. */
bool gpsp_rv32_ram_unit_matches(unsigned id, const uint8_t *guest);

/* Native code stays in the cache. A dropped unit is no longer entered
 * until it is revived. */
void gpsp_rv32_ram_unit_kill(unsigned id);
void gpsp_rv32_ram_unit_revive(unsigned id);

/* The guest bytes a unit was translated from. */
const uint8_t *gpsp_rv32_ram_unit_copy(unsigned id);

/* Takes guest as the unit's bytes after its code was patched to match. */
void gpsp_rv32_ram_unit_update(unsigned id, const uint8_t *guest);

/* Points the unit's exit at site to a new target key; false when the
   unit has no such exit. */
bool gpsp_rv32_ram_unit_retarget(unsigned id, uint32_t site, uint32_t key);

/* The patch point at a physical guest address, or NULL. */
const gpsp_rv32_ram_patch *gpsp_rv32_ram_unit_patch(unsigned id,
    uint32_t guest);

const gpsp_rv32_ram_unit *gpsp_rv32_ram_unit_get(unsigned id);
const gpsp_rv32_ram_ref *gpsp_rv32_ram_unit_refs(unsigned id);
const gpsp_rv32_ram_patch *gpsp_rv32_ram_unit_patches(unsigned id);
unsigned gpsp_rv32_ram_unit_count(void);
bool gpsp_rv32_ram_units_full(void);

#if defined(GPSP_BENCHMARK_RAM_CODE)
/* Peak table use since boot and why units or links were not recorded. */
typedef struct
{
  unsigned units, nodes, refs, patches, copy_bytes, links;
  unsigned add_while_full, add_stage_full, add_bad_range, add_units_full;
  unsigned add_nodes_full, add_refs_full, add_copy_full;
  unsigned patch_table_full, links_full;
  bool full;
} gpsp_rv32_ram_units_bench;

void gpsp_rv32_ram_units_bench_get(gpsp_rv32_ram_units_bench *out);
#endif

/* True when the RAM cache offset lies in one of the units' native code. */
bool gpsp_rv32_ram_units_contain(uint32_t offset, const uint16_t *ids,
    unsigned count);

/* A patched jump from one RAM unit into another, as RAM cache offsets. */
void gpsp_rv32_ram_link_add(uint32_t site, uint32_t target, uint32_t key);

/* Forgets the link from site. */
void gpsp_rv32_ram_link_drop(uint32_t site);

/* Forgets links from or into the given dropped units. Links from live
   units into them are passed to unlink first. */
void gpsp_rv32_ram_links_kill(const uint16_t *ids, unsigned count,
    void (*unlink)(uint32_t site, uint32_t key));

/* Code writes that changed bytes, kept until the RAM cache is updated.
 * A full queue sets the overflow flag instead. */
void gpsp_rv32_ram_dirty_add(uint32_t address, unsigned bytes);
unsigned gpsp_rv32_ram_dirty_count(void);
bool gpsp_rv32_ram_dirty_overflow(void);
uint32_t gpsp_rv32_ram_dirty_get(unsigned index, unsigned *bytes);
void gpsp_rv32_ram_dirty_clear(void);

#endif

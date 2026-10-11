/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_ram_units.h"

#include <string.h>

/* 256-byte pages: IWRAM pages first, then EWRAM. */
#define PAGE_SHIFT 8
#define IWRAM_PAGES (0x8000 >> PAGE_SHIFT)
#define EWRAM_PAGES (0x40000 >> PAGE_SHIFT)
#define PAGE_COUNT (IWRAM_PAGES + EWRAM_PAGES)
#define PAGE_NODES 16384
#define REFS 65536
#define STAGE_REFS 2048
#define PATCHES 8192
#define STAGE_PATCHES 256
#define COPY_BYTES (512 * 1024)
#define NONE 0xffffu

/* Links hold a node index plus one, so zeroed memory is an empty list.
   Live and dormant units are kept on separate page lists. */
typedef struct
{
  uint16_t unit, next, prev;
} page_node;

static gpsp_rv32_ram_unit units[GPSP_RV32_RAM_UNITS];
static unsigned unit_count;
static uint16_t live_head[PAGE_COUNT], dormant_head[PAGE_COUNT];
static page_node nodes[PAGE_NODES];
static unsigned node_count;
static gpsp_rv32_ram_ref refs[REFS];
static unsigned ref_count;
static uint8_t copies[COPY_BYTES];
static gpsp_rv32_ram_patch patches[PATCHES];
static unsigned patch_count;
static unsigned copy_bytes;
static bool full;

static gpsp_rv32_ram_ref stage_entries[STAGE_REFS];
static gpsp_rv32_ram_ref stage_exits[STAGE_REFS];
static gpsp_rv32_ram_patch stage_patches[STAGE_PATCHES];
static unsigned stage_entry_count, stage_exit_count, stage_patch_count;
static bool stage_full;

static struct
{
  uint32_t site, target, key;
} links[GPSP_RV32_RAM_LINKS];
static unsigned link_count;

#if defined(GPSP_BENCHMARK_RAM_CODE)
static gpsp_rv32_ram_units_bench bench;

static void bench_peak(void)
{
  if (unit_count > bench.units) bench.units = unit_count;
  if (node_count > bench.nodes) bench.nodes = node_count;
  if (ref_count > bench.refs) bench.refs = ref_count;
  if (patch_count > bench.patches) bench.patches = patch_count;
  if (copy_bytes > bench.copy_bytes) bench.copy_bytes = copy_bytes;
  if (link_count > bench.links) bench.links = link_count;
}

void gpsp_rv32_ram_units_bench_get(gpsp_rv32_ram_units_bench *out)
{
  *out = bench;
  out->full = full;
}
#define bench_count(field) (bench.field++)
#else
#define bench_peak() ((void)0)
#define bench_count(field) ((void)0)
#endif

static struct
{
  uint32_t address;
  uint16_t bytes;
} dirty[GPSP_RV32_RAM_DIRTY];
static unsigned dirty_count;
static bool dirty_overflow;

static uint32_t physical(uint32_t pc)
{
  return (pc >> 24) == 2 ? 0x02000000u | (pc & 0x3ffffu) :
      0x03000000u | (pc & 0x7fffu);
}

/* Page index, or NONE outside IWRAM and EWRAM. */
static unsigned page_of(uint32_t address)
{
  if ((address >> 24) == 3)
    return (address & 0x7fffu) >> PAGE_SHIFT;
  if ((address >> 24) == 2)
    return IWRAM_PAGES + ((address & 0x3ffffu) >> PAGE_SHIFT);
  return NONE;
}

static void list_insert(uint16_t *head, unsigned node)
{
  nodes[node - 1].prev = 0;
  nodes[node - 1].next = *head;
  if (*head)
    nodes[*head - 1].prev = (uint16_t)node;
  *head = (uint16_t)node;
}

static void list_remove(uint16_t *head, unsigned node)
{
  page_node *n = &nodes[node - 1];
  if (n->prev)
    nodes[n->prev - 1].next = n->next;
  else
    *head = n->next;
  if (n->next)
    nodes[n->next - 1].prev = n->prev;
}

/* Moves a unit's page nodes between the live and dormant lists. */
static void move_unit(const gpsp_rv32_ram_unit *unit, uint16_t *from,
    uint16_t *to)
{
  unsigned first = page_of(unit->guest_start);
  unsigned last = page_of(unit->guest_end - 1);
  for (unsigned page = first; page <= last; page++)
  {
    unsigned node = unit->first_node + (page - first) + 1;
    list_remove(&from[page], node);
    list_insert(&to[page], node);
  }
}

/* Word-wise FNV-1a; matches are confirmed against the stored copy. */
static uint32_t hash_bytes(const uint8_t *data, unsigned bytes)
{
  uint32_t hash = 2166136261u;
  unsigned i = 0;
  for (; i + 4 <= bytes; i += 4)
  {
    uint32_t word;
    memcpy(&word, data + i, sizeof(word));
    hash = (hash ^ word) * 16777619u;
  }
  for (; i < bytes; i++)
    hash = (hash ^ data[i]) * 16777619u;
  return hash;
}

void gpsp_rv32_ram_units_reset(void)
{
  unit_count = 0;
  node_count = 0;
  ref_count = 0;
  patch_count = 0;
  copy_bytes = 0;
  link_count = 0;
  full = false;
  memset(live_head, 0, sizeof(live_head));
  memset(dormant_head, 0, sizeof(dormant_head));
}

void gpsp_rv32_ram_stage_begin(void)
{
  stage_entry_count = 0;
  stage_exit_count = 0;
  stage_patch_count = 0;
  stage_full = false;
}

void gpsp_rv32_ram_stage_entry(uint32_t key, uint32_t offset)
{
  if (stage_entry_count == STAGE_REFS)
  {
    stage_full = true;
    return;
  }
  stage_entries[stage_entry_count].key = key;
  stage_entries[stage_entry_count].offset = offset;
  stage_entry_count++;
}

void gpsp_rv32_ram_stage_exit(uint32_t key, uint32_t site)
{
  if (stage_exit_count == STAGE_REFS)
  {
    stage_full = true;
    return;
  }
  stage_exits[stage_exit_count].key = key;
  stage_exits[stage_exit_count].offset = site;
  stage_exit_count++;
}

void gpsp_rv32_ram_stage_patch(uint32_t pc, uint32_t native,
    unsigned bytes, unsigned mode, unsigned live)
{
  if (stage_patch_count == STAGE_PATCHES)
    return;
  gpsp_rv32_ram_patch *patch = &stage_patches[stage_patch_count++];
  patch->guest = physical(pc);
  patch->native = native;
  patch->bytes = (uint8_t)bytes;
  patch->mode = (uint8_t)mode;
  patch->live = (uint8_t)live;
}

unsigned gpsp_rv32_ram_stage_mode_before(uint32_t site)
{
  for (unsigned i = 0; i < stage_patch_count; i++)
  {
    if (stage_patches[i].native + stage_patches[i].bytes == site)
      return stage_patches[i].mode;
  }
  return 0;
}

bool gpsp_rv32_ram_unit_add(uint32_t start_pc, uint32_t end_pc,
    uint32_t native_start, uint32_t native_end, uint32_t head, bool thumb,
    unsigned switches, const uint8_t *guest)
{
  if (full)
  {
    bench_count(add_while_full);
    return false;
  }

  uint32_t start = physical(start_pc);
  uint32_t end = start + (end_pc - start_pc);
  unsigned first = page_of(start);
  unsigned last = page_of(end - 1);
  unsigned ref_total = stage_entry_count + stage_exit_count;
  if (stage_full || first == NONE || last == NONE || last < first ||
      unit_count == GPSP_RV32_RAM_UNITS ||
      node_count + (last - first + 1) > PAGE_NODES ||
      ref_count + ref_total > REFS ||
      copy_bytes + (end - start) > COPY_BYTES)
  {
#if defined(GPSP_BENCHMARK_RAM_CODE)
    if (stage_full)
      bench.add_stage_full++;
    else if (first == NONE || last == NONE || last < first)
      bench.add_bad_range++;
    else if (unit_count == GPSP_RV32_RAM_UNITS)
      bench.add_units_full++;
    else if (node_count + (last - first + 1) > PAGE_NODES)
      bench.add_nodes_full++;
    else if (ref_count + ref_total > REFS)
      bench.add_refs_full++;
    else
      bench.add_copy_full++;
#endif
    full = true;
    return false;
  }

  unsigned id = unit_count++;
  gpsp_rv32_ram_unit *unit = &units[id];
  unit->guest_start = start;
  unit->guest_end = end;
  unit->native_start = native_start;
  unit->native_end = native_end;
  unit->head = head;
  unit->thumb = thumb;
  unit->live = true;
  unit->switches = (uint8_t)switches;
  unit->hash = hash_bytes(guest, end - start);
  unit->copy = copy_bytes;
  memcpy(copies + copy_bytes, guest, end - start);
  copy_bytes += (end - start + 3) & ~3u;

  unit->first_ref = ref_count;
  unit->entries = (uint16_t)stage_entry_count;
  unit->exits = (uint16_t)stage_exit_count;
  memcpy(refs + ref_count, stage_entries,
      stage_entry_count * sizeof(*refs));
  ref_count += stage_entry_count;
  memcpy(refs + ref_count, stage_exits, stage_exit_count * sizeof(*refs));
  ref_count += stage_exit_count;

  unit->first_patch = patch_count;
  unit->patches = 0;
  if (patch_count + stage_patch_count <= PATCHES)
  {
    memcpy(patches + patch_count, stage_patches,
        stage_patch_count * sizeof(*patches));
    patch_count += stage_patch_count;
    unit->patches = (uint16_t)stage_patch_count;
  }
#if defined(GPSP_BENCHMARK_RAM_CODE)
  else if (stage_patch_count)
    bench_count(patch_table_full);
#endif

  unit->first_node = (uint16_t)node_count;
  for (unsigned page = first; page <= last; page++)
  {
    nodes[node_count].unit = (uint16_t)id;
    list_insert(&live_head[page], ++node_count);
  }
  bench_peak();
  return true;
}

/* Units on the pages of [start, end) that pass the filter. */
static unsigned collect(uint32_t start, uint32_t end, bool live,
    int thumb, uint16_t *ids, unsigned max_ids)
{
  unsigned first = page_of(start);
  unsigned last = page_of(end - 1);
  unsigned found = 0;
  if (first == NONE || last == NONE)
    return 0;

  const uint16_t *head = live ? live_head : dormant_head;
  for (unsigned page = first; page <= last; page++)
  {
    for (unsigned n = head[page]; n; n = nodes[n - 1].next)
    {
      uint16_t id = nodes[n - 1].unit;
      const gpsp_rv32_ram_unit *unit = &units[id];
      if (unit->live != live ||
          (thumb >= 0 && unit->thumb != thumb) ||
          unit->guest_start >= end || unit->guest_end <= start)
        continue;
      unsigned i = 0;
      while (i < found && ids[i] != id)
        i++;
      if (i == found && found < max_ids)
        ids[found++] = id;
    }
  }
  return found;
}

unsigned gpsp_rv32_ram_units_find(uint32_t address, unsigned bytes,
    uint16_t *ids, unsigned max_ids)
{
  uint32_t start = physical(address);
  return collect(start, start + bytes, true, -1, ids, max_ids);
}

unsigned gpsp_rv32_ram_units_dormant(uint32_t pc, bool thumb,
    uint16_t *ids, unsigned max_ids)
{
  uint32_t start = physical(pc);
  return collect(start, start + 2, false, thumb, ids, max_ids);
}

bool gpsp_rv32_ram_unit_matches(unsigned id, const uint8_t *guest)
{
  const gpsp_rv32_ram_unit *unit = &units[id];
  unsigned bytes = unit->guest_end - unit->guest_start;
  return hash_bytes(guest, bytes) == unit->hash &&
      !memcmp(guest, copies + unit->copy, bytes);
}

void gpsp_rv32_ram_unit_kill(unsigned id)
{
  if (id < unit_count && units[id].live)
  {
    move_unit(&units[id], live_head, dormant_head);
    units[id].live = false;
  }
}

void gpsp_rv32_ram_unit_revive(unsigned id)
{
  if (id < unit_count && !units[id].live)
  {
    move_unit(&units[id], dormant_head, live_head);
    units[id].live = true;
  }
}

const uint8_t *gpsp_rv32_ram_unit_copy(unsigned id)
{
  return id < unit_count ? copies + units[id].copy : NULL;
}

void gpsp_rv32_ram_unit_update(unsigned id, const uint8_t *guest)
{
  gpsp_rv32_ram_unit *unit = &units[id];
  unsigned bytes = unit->guest_end - unit->guest_start;
  memcpy(copies + unit->copy, guest, bytes);
  unit->hash = hash_bytes(guest, bytes);
}

bool gpsp_rv32_ram_unit_retarget(unsigned id, uint32_t site, uint32_t key)
{
  const gpsp_rv32_ram_unit *unit = &units[id];
  gpsp_rv32_ram_ref *exits = &refs[unit->first_ref + unit->entries];
  for (unsigned i = 0; i < unit->exits; i++)
  {
    if (exits[i].offset == site)
    {
      exits[i].key = key;
      return true;
    }
  }
  return false;
}

const gpsp_rv32_ram_patch *gpsp_rv32_ram_unit_patch(unsigned id,
    uint32_t guest)
{
  const gpsp_rv32_ram_unit *unit = &units[id];
  for (unsigned i = 0; i < unit->patches; i++)
  {
    if (patches[unit->first_patch + i].guest == guest)
      return &patches[unit->first_patch + i];
  }
  return NULL;
}

const gpsp_rv32_ram_unit *gpsp_rv32_ram_unit_get(unsigned id)
{
  return id < unit_count ? &units[id] : NULL;
}

const gpsp_rv32_ram_ref *gpsp_rv32_ram_unit_refs(unsigned id)
{
  return id < unit_count ? &refs[units[id].first_ref] : NULL;
}

const gpsp_rv32_ram_patch *gpsp_rv32_ram_unit_patches(unsigned id)
{
  return id < unit_count ? &patches[units[id].first_patch] : NULL;
}

unsigned gpsp_rv32_ram_unit_count(void)
{
  return unit_count;
}

bool gpsp_rv32_ram_units_full(void)
{
  return full;
}

bool gpsp_rv32_ram_units_contain(uint32_t offset, const uint16_t *ids,
    unsigned count)
{
  for (unsigned i = 0; i < count; i++)
  {
    const gpsp_rv32_ram_unit *unit = &units[ids[i]];
    if (offset >= unit->native_start && offset < unit->native_end)
      return true;
  }
  return false;
}

void gpsp_rv32_ram_link_add(uint32_t site, uint32_t target, uint32_t key)
{
  if (link_count == GPSP_RV32_RAM_LINKS)
  {
    bench_count(links_full);
    full = true;
    return;
  }
  links[link_count].site = site;
  links[link_count].target = target;
  links[link_count].key = key;
  link_count++;
  bench_peak();
}

void gpsp_rv32_ram_link_drop(uint32_t site)
{
  for (unsigned i = 0; i < link_count;)
  {
    if (links[i].site == site)
      links[i] = links[--link_count];
    else
      i++;
  }
}

void gpsp_rv32_ram_links_kill(const uint16_t *ids, unsigned count,
    void (*unlink)(uint32_t site, uint32_t key))
{
  /* Most links are outside the dropped units' native span. */
  uint32_t low = UINT32_MAX, high = 0;
  for (unsigned k = 0; k < count; k++)
  {
    if (units[ids[k]].native_start < low)
      low = units[ids[k]].native_start;
    if (units[ids[k]].native_end > high)
      high = units[ids[k]].native_end;
  }
  unsigned i = 0;
  while (i < link_count)
  {
    if ((links[i].site < low || links[i].site >= high) &&
        (links[i].target < low || links[i].target >= high))
    {
      i++;
      continue;
    }
    bool dead_site = gpsp_rv32_ram_units_contain(links[i].site, ids, count);
    if (!dead_site &&
        !gpsp_rv32_ram_units_contain(links[i].target, ids, count))
    {
      i++;
      continue;
    }
    if (!dead_site)
      unlink(links[i].site, links[i].key);
    links[i] = links[--link_count];
  }
}

/* Merges touching ranges into dirty[index], so a copy over code stays
   one range. Physical addresses keep a range inside one region. */
static bool dirty_merge(unsigned index, uint32_t address, unsigned bytes)
{
  uint32_t start = dirty[index].address;
  uint32_t end = start + dirty[index].bytes;
  if (address + bytes < start || address > end)
    return false;
  uint32_t low = address < start ? address : start;
  uint32_t high = address + bytes > end ? address + bytes : end;
  if (high - low > UINT16_MAX)
    return false;
  dirty[index].address = low;
  dirty[index].bytes = (uint16_t)(high - low);
  return true;
}

void gpsp_rv32_ram_dirty_add(uint32_t address, unsigned bytes)
{
  address = physical(address);
  unsigned i = 0;
  while (i < dirty_count && !dirty_merge(i, address, bytes))
    i++;
  if (i < dirty_count)
  {
    /* The grown range may now touch others. */
    for (unsigned j = 0; j < dirty_count;)
    {
      if (j != i && dirty_merge(i, dirty[j].address, dirty[j].bytes))
      {
        dirty[j] = dirty[--dirty_count];
        if (i == dirty_count)
          i = j;
        j = 0;
      }
      else
        j++;
    }
    return;
  }
  if (dirty_count == GPSP_RV32_RAM_DIRTY)
  {
    dirty_overflow = true;
    return;
  }
  dirty[dirty_count].address = address;
  dirty[dirty_count].bytes = (uint16_t)bytes;
  dirty_count++;
}

unsigned gpsp_rv32_ram_dirty_count(void)
{
  return dirty_count;
}

bool gpsp_rv32_ram_dirty_overflow(void)
{
  return dirty_overflow;
}

uint32_t gpsp_rv32_ram_dirty_get(unsigned index, unsigned *bytes)
{
  if (bytes)
    *bytes = dirty[index].bytes;
  return dirty[index].address;
}

void gpsp_rv32_ram_dirty_clear(void)
{
  dirty_count = 0;
  dirty_overflow = false;
}

/* ROM backup signature scanning.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include "gba_rom_scan.h"

#define ROM_SIG_ALL \
  (ROM_SIG_EEPROM | ROM_SIG_SRAM | ROM_SIG_FLASH1M | ROM_SIG_FLASH5)

struct rom_scan_state
{
  uint32_t found;
  uint32_t offset;
  uint8_t tail[16];
  uint32_t tail_size;
};

static uint32_t scan_range(const uint8_t *data, uint32_t size,
    uint32_t offset)
{
  uint32_t found = 0;
  uint32_t i;

  /* Preserve the whole-ROM scan's word alignment, including short reads. */
  for (i = (0U - offset) & 3; i < size; i += 4)
  {
    uint32_t left = size - i;
    if (data[i] == 'E' && left >= 8 && !memcmp(data + i, "EEPROM_V", 8))
      found |= ROM_SIG_EEPROM;
    else if (data[i] == 'S' && left >= 6 && !memcmp(data + i, "SRAM_V", 6))
      found |= ROM_SIG_SRAM;
    else if (data[i] == 'F')
    {
      if (left >= 9 && !memcmp(data + i, "FLASH1M_V", 9))
        found |= ROM_SIG_FLASH1M;
      else if ((left >= 10 && !memcmp(data + i, "FLASH512_V", 10)) ||
               (left >= 7 && !memcmp(data + i, "FLASH_V", 7)))
        found |= ROM_SIG_FLASH5;
    }
  }
  return found;
}

static void scan_chunk(struct rom_scan_state *scan, const uint8_t *data,
    uint32_t size)
{
  uint8_t edge[32];
  uint32_t head = size < 16 ? size : 16;

  /* Carry bytes across both resident block and file read boundaries. */
  memcpy(edge, scan->tail, scan->tail_size);
  memcpy(edge + scan->tail_size, data, head);
  scan->found |= scan_range(edge, scan->tail_size + head,
      scan->offset - scan->tail_size);
  scan->found |= scan_range(data, size, scan->offset);

  if (size >= 16)
  {
    memcpy(scan->tail, data + size - 16, 16);
    scan->tail_size = 16;
  }
  else
  {
    uint32_t keep = scan->tail_size;
    if (keep > 16 - size)
      keep = 16 - size;
    memmove(scan->tail, scan->tail + scan->tail_size - keep, keep);
    memcpy(scan->tail + keep, data, size);
    scan->tail_size = keep + size;
  }
  scan->offset += size;
}

struct rom_scan_result rom_scan_signatures(RFILE *file,
    uint8_t *const *blocks, uint32_t block_count, uint32_t block_size)
{
  struct rom_scan_state scan = {0};
  struct rom_scan_result result = {0, ROM_SCAN_OK};
  int64_t file_size = filestream_get_size(file);
  uint32_t i, size, chunk_size;
  uint8_t *chunk = NULL;

  if (file_size <= 0 || file_size > UINT32_MAX || !block_size)
  {
    result.status = ROM_SCAN_SIZE_ERROR;
    return result;
  }
  size = (uint32_t)file_size;
  for (i = 0; i < block_count && scan.offset < size; i++)
  {
    uint32_t count = size - scan.offset;
    if (count > block_size)
      count = block_size;
    scan_chunk(&scan, blocks[i], count);
    if (scan.found == ROM_SIG_ALL)
      break;
  }

  if (scan.offset < size && scan.found != ROM_SIG_ALL)
  {
    /* A small buffer still scans the full file when memory is scarce. */
    for (chunk_size = 32 * 1024; chunk_size >= 512; chunk_size /= 2)
    {
      chunk = (uint8_t *)malloc(chunk_size);
      if (chunk)
        break;
    }
    if (!chunk)
      result.status = ROM_SCAN_NO_MEMORY;
    else if (filestream_seek(file, scan.offset, SEEK_SET) != scan.offset)
      result.status = ROM_SCAN_SEEK_ERROR;
    else
    {
      while (scan.offset < size && scan.found != ROM_SIG_ALL)
      {
        uint32_t count = size - scan.offset;
        int64_t got;
        if (count > chunk_size)
          count = chunk_size;
        got = filestream_read(file, chunk, count);
        if (got <= 0 || got > count)
        {
          result.status = ROM_SCAN_READ_ERROR;
          break;
        }
        scan_chunk(&scan, chunk, (uint32_t)got);
      }
    }
    free(chunk);
  }
  result.signatures = scan.found;
  return result;
}

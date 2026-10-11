/* ROM backup signature scanning.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GBA_ROM_SCAN_H
#define GBA_ROM_SCAN_H

#include <stdint.h>
#include "streams/file_stream.h"

enum
{
  ROM_SIG_EEPROM  = (1 << 0),
  ROM_SIG_SRAM    = (1 << 1),
  ROM_SIG_FLASH1M = (1 << 2),
  ROM_SIG_FLASH5  = (1 << 3)
};

enum rom_scan_status
{
  ROM_SCAN_OK,
  ROM_SCAN_SIZE_ERROR,
  ROM_SCAN_NO_MEMORY,
  ROM_SCAN_SEEK_ERROR,
  ROM_SCAN_READ_ERROR
};

struct rom_scan_result
{
  uint32_t signatures;
  enum rom_scan_status status;
};

/* Blocks contain the file's initial bytes in order. On failure, signatures
   contains only the matches found before the error. */
struct rom_scan_result rom_scan_signatures(RFILE *file,
    uint8_t *const *blocks, uint32_t block_count, uint32_t block_size);

#endif

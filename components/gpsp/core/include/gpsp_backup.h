/* gpSP cartridge save size for frontends.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BACKUP_H
#define GPSP_BACKUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of the detected save type as a raw .sav file holds them: 32 KiB
 * SRAM, 64 or 128 KiB flash, 512 B or 8 KiB EEPROM. Zero while unknown. */
size_t gpsp_backup_size(void);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_BACKUP_H */

/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_MEMORY_H
#define GPSP_MEMORY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the ROM buffer limit before retro_init(), in MiB. Returns false if
 * buffers are allocated or the limit is outside 1..ROM_BUFFER_SIZE. */
bool gpsp_set_rom_buffer_limit(unsigned mib);

#ifdef __cplusplus
}
#endif

#endif

/* ESP32-P4 cache settings.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_CACHE_H
#define GPSP_CACHE_H

/** Turn on L1 data cache autoload over PSRAM. */
void gpsp_cache_init(void);

#endif

/* ESP32-P4 cache settings.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_cache.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_DCACHE_AUTOLOAD

#include "esp_log.h"
#include "esp_psram.h"
#include "soc/cache_reg.h"
#include "soc/ext_mem_defs.h"
#include "soc/soc.h"

/* Trigger mode 2: both misses and hits load the next lines. */
#define DCACHE_AUTOLOAD_MISS_HIT 2

static const char *TAG = "gpsp_cache";

/* L1 instruction cache autoload and L2 miss-hit autoload crashed in tests,
   so only the L1 data cache uses it. */
void gpsp_cache_init(void)
{
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_SCT0_ADDR_REG,
            SOC_DRAM_PSRAM_ADDRESS_LOW);
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_SCT0_SIZE_REG, esp_psram_get_size());
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG,
            CACHE_L1_DCACHE_AUTOLOAD_ENA | CACHE_L1_DCACHE_AUTOLOAD_SCT0_ENA |
            (DCACHE_AUTOLOAD_MISS_HIT <<
                    CACHE_L1_DCACHE_AUTOLOAD_TRIGGER_MODE_S));
    ESP_LOGI(TAG, "L1 data cache autoload over PSRAM: ctrl=0x%08lx",
            (unsigned long)REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG));
}

#endif

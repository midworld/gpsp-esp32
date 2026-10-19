/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SPI_H
#define GPSP_SPI_H
#include <stdbool.h>
#include "esp_err.h"
esp_err_t gpsp_spi_init(void);
esp_err_t gpsp_spi_draw(int x0, int y0, int x1, int y1, const void *pixels);
void gpsp_spi_backlight(bool enabled);
void gpsp_spi_shutdown(void);
#endif

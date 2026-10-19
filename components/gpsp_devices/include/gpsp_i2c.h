/* Shared display and codec control bus.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_I2C_H
#define GPSP_I2C_H

#include "driver/i2c_master.h"

/* Called serially during board initialization and shutdown. */
esp_err_t gpsp_i2c_acquire(i2c_master_bus_handle_t *bus);
void gpsp_i2c_release(void);

#endif

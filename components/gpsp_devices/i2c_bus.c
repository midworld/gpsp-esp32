/* Shared display and codec control bus.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"

#include "gpsp_i2c.h"

static i2c_master_bus_handle_t s_bus;
static unsigned s_users;

esp_err_t gpsp_i2c_acquire(i2c_master_bus_handle_t *bus)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = gpsp_hardware.i2c.port,
        .sda_io_num = gpsp_hardware.i2c.sda,
        .scl_io_num = gpsp_hardware.i2c.scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    *bus = NULL;
    if (!s_bus)
    {
        esp_err_t result = i2c_new_master_bus(&config, &s_bus);
        if (result != ESP_OK)
            return result;
    }
    s_users++;
    *bus = s_bus;
    return ESP_OK;
}

void gpsp_i2c_release(void)
{
    if (s_users && --s_users == 0)
    {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

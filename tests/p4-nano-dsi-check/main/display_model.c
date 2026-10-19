/* Test-only peripheral model for firmware lifecycle and layout checks.
 * Nearest sampling checks geometry, not the PPA bilinear filter.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int bus_token;
static int device_token;
static int dsi_token;
static int io_token;
static int power_token;
static bool bus_open;
static bool device_open;
static bool fail_panel_init;
static unsigned last_scale;
static unsigned last_source_pixels;
static unsigned last_core;

unsigned display_model_ppa_source_pixels(void)
{
    return last_source_pixels;
}

unsigned display_model_ppa_core(void)
{
    return last_core;
}

unsigned display_model_ppa_scale(void)
{
    return last_scale;
}

void display_model_fail_init(void)
{
    fail_panel_init = true;
}

esp_err_t __real_esp_ldo_acquire_channel(const esp_ldo_channel_config_t *config,
        esp_ldo_channel_handle_t *handle);
esp_err_t __real_esp_ldo_release_channel(esp_ldo_channel_handle_t handle);

struct model_panel
{
    void *frames[3];
    const void *active;
    const void *pending;
    esp_lcd_dpi_panel_event_callbacks_t callbacks;
    void *context;
    esp_timer_handle_t timer;
};
static struct model_panel *panel_model;

struct model_client
{
    ppa_event_callbacks_t callbacks;
    void *context;
    esp_timer_handle_t timer;
};

esp_err_t __wrap_esp_ldo_acquire_channel(const esp_ldo_channel_config_t *config,
        esp_ldo_channel_handle_t *handle)
{
    if (config->chan_id != 3)
        return __real_esp_ldo_acquire_channel(config, handle);
    assert(config->chan_id == 3 && config->voltage_mv == 2500);
    *handle = (void *)&power_token;
    return ESP_OK;
}

esp_err_t __wrap_esp_ldo_release_channel(esp_ldo_channel_handle_t handle)
{
    if (handle != (void *)&power_token)
        return __real_esp_ldo_release_channel(handle);
    return ESP_OK;
}

esp_err_t __wrap_i2c_new_master_bus(const i2c_master_bus_config_t *config,
        i2c_master_bus_handle_t *handle)
{
    assert(!bus_open && config->sda_io_num == 7 && config->scl_io_num == 8);
    bus_open = true;
    *handle = (void *)&bus_token;
    return ESP_OK;
}

esp_err_t __wrap_i2c_del_master_bus(i2c_master_bus_handle_t handle)
{
    assert(bus_open && !device_open && handle == (void *)&bus_token);
    bus_open = false;
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
        const i2c_device_config_t *config, i2c_master_dev_handle_t *device)
{
    assert(bus_open && bus == (void *)&bus_token && !device_open);
    assert(config->device_address == 0x45);
    device_open = true;
    *device = (void *)&device_token;
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device_open && device == (void *)&device_token);
    device_open = false;
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_transmit(i2c_master_dev_handle_t device,
        const uint8_t *data, size_t size, int timeout)
{
    (void)timeout;
    assert(device_open && device == (void *)&device_token && size == 2);
    assert(data[0] == 0x95 || data[0] == 0x96);
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *config,
        esp_lcd_dsi_bus_handle_t *bus)
{
    assert(config->num_data_lanes == 2 && config->lane_bit_rate_mbps == 500);
    *bus = (void *)&dsi_token;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_del_dsi_bus(esp_lcd_dsi_bus_handle_t bus)
{
    assert(bus == (void *)&dsi_token);
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t bus,
        const esp_lcd_dbi_io_config_t *config, esp_lcd_panel_io_handle_t *io)
{
    assert(bus == (void *)&dsi_token && config->lcd_cmd_bits == 8);
    *io = (void *)&io_token;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,
        int command, const void *data, size_t size)
{
    (void)command;
    assert(io == (void *)&io_token && (!size || data));
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    assert(io == (void *)&io_token);
    return ESP_OK;
}

static void frame_tick(void *arg)
{
    struct model_panel *panel = arg;
    panel->active = panel->pending;
    if (panel->callbacks.on_frame_buf_complete((void *)panel, NULL,
            panel->context))
        esp_timer_isr_dispatch_need_yield();
}

esp_err_t __wrap_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus,
        const esp_lcd_dpi_panel_config_t *config, esp_lcd_panel_handle_t *handle)
{
    assert(bus == (void *)&dsi_token && config->num_fbs == 3);
    assert(config->video_timing.h_size == 480 && config->video_timing.v_size == 800);
    assert(config->in_color_format == LCD_COLOR_FMT_RGB565);
    if (fail_panel_init)
    {
        fail_panel_init = false;
        return ESP_FAIL;
    }
    struct model_panel *panel = calloc(1, sizeof(*panel));
    assert(panel);
    for (unsigned i = 0; i < 3; i++)
    {
        panel->frames[i] = heap_caps_aligned_calloc(64, 1, 480 * 800 * 2,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        assert(panel->frames[i]);
    }
    panel->active = panel->pending = panel->frames[0];
    const esp_timer_create_args_t timer = {
        .callback = frame_tick, .arg = panel,
        .dispatch_method = ESP_TIMER_ISR, .name = "dsi_model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &panel->timer));
    panel_model = panel;
    *handle = (void *)panel;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t handle,
        uint32_t count, void **first, ...)
{
    struct model_panel *panel = (void *)handle;
    assert(count == 3);
    *first = panel->frames[0];
    va_list args;
    va_start(args, first);
    for (unsigned i = 1; i < count; i++)
        *va_arg(args, void **) = panel->frames[i];
    va_end(args);
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_dpi_panel_register_event_callbacks(
        esp_lcd_panel_handle_t handle, const esp_lcd_dpi_panel_event_callbacks_t *cbs,
        void *context)
{
    struct model_panel *panel = (void *)handle;
    panel->callbacks = *cbs;
    panel->context = context;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_init(esp_lcd_panel_handle_t handle)
{
    struct model_panel *panel = (void *)handle;
    return esp_timer_start_periodic(panel->timer, 16667);
}

esp_err_t __wrap_esp_lcd_panel_del(esp_lcd_panel_handle_t handle)
{
    struct model_panel *panel = (void *)handle;
    esp_timer_stop(panel->timer);
    ESP_ERROR_CHECK(esp_timer_delete(panel->timer));
    for (unsigned i = 0; i < 3; i++)
        heap_caps_free(panel->frames[i]);
    free(panel);
    panel_model = NULL;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t handle,
        int x0, int y0, int x1, int y1, const void *data)
{
    struct model_panel *panel = (void *)handle;
    assert(x0 == 0 && y0 == 0 && x1 == 480 && y1 == 800);
    assert(data != panel->active);
    ESP_ERROR_CHECK(esp_cache_msync((void *)data, 480 * 800 * 2,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    panel->pending = data;
    return ESP_OK;
}

static void ppa_tick(void *arg)
{
    struct model_client *client = arg;
    if (client->callbacks.on_trans_done((void *)client, NULL, client->context))
        esp_timer_isr_dispatch_need_yield();
}

esp_err_t __wrap_ppa_register_client(const ppa_client_config_t *config,
        ppa_client_handle_t *handle)
{
    assert(config->oper_type == PPA_OPERATION_SRM);
    struct model_client *client = calloc(1, sizeof(*client));
    assert(client);
    const esp_timer_create_args_t timer = {
        .callback = ppa_tick, .arg = client,
        .dispatch_method = ESP_TIMER_ISR, .name = "ppa_model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &client->timer));
    *handle = (void *)client;
    return ESP_OK;
}

esp_err_t __wrap_ppa_unregister_client(ppa_client_handle_t handle)
{
    struct model_client *client = (void *)handle;
    ESP_ERROR_CHECK(esp_timer_delete(client->timer));
    free(client);
    return ESP_OK;
}

esp_err_t __wrap_ppa_client_register_event_callbacks(ppa_client_handle_t handle,
        const ppa_event_callbacks_t *callbacks)
{
    struct model_client *client = (void *)handle;
    client->callbacks = *callbacks;
    return ESP_OK;
}

esp_err_t __wrap_ppa_do_scale_rotate_mirror(ppa_client_handle_t handle,
        const ppa_srm_oper_config_t *op)
{
    struct model_client *client = (void *)handle;
    assert(op->in.buffer != op->out.buffer && ((uintptr_t)op->out.buffer % 64) == 0);
    assert(op->out.buffer_size >= op->out.pic_w * op->out.pic_h * 2);
    assert(op->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 &&
            op->out.srm_cm == PPA_SRM_COLOR_MODE_RGB565);
    assert(!panel_model || op->out.buffer != panel_model->active);
    unsigned scale = op->scale_x;
    last_scale = scale;
    last_source_pixels = op->in.block_w * op->in.block_h;
    last_core = xPortGetCoreID();
    assert(scale == op->scale_x && scale == op->scale_y);
    unsigned w = op->in.block_h * scale;
    unsigned h = op->in.block_w * scale;
    assert(op->out.block_offset_x + w <= op->out.pic_w);
    assert(op->out.block_offset_y + h <= op->out.pic_h);
    const uint16_t *input = op->in.buffer;
    uint16_t *output = op->out.buffer;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
        {
            unsigned sx, sy;
            if (op->rotation_angle == PPA_SRM_ROTATION_ANGLE_90)
            {
                sx = (h - 1 - y) / scale;
                sy = x / scale;
            }
            else
            {
                assert(op->rotation_angle == PPA_SRM_ROTATION_ANGLE_270);
                sx = y / scale;
                sy = (w - 1 - x) / scale;
            }
            output[(y + op->out.block_offset_y) * op->out.pic_w +
                    x + op->out.block_offset_x] = input[sy * op->in.pic_w + sx];
        }
    ESP_ERROR_CHECK(esp_cache_msync(output, op->out.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    if (op->mode == PPA_TRANS_MODE_NON_BLOCKING)
    {
        client->context = op->user_data;
        return esp_timer_start_once(client->timer, 100);
    }
    return ESP_OK;
}

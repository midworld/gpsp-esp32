/* Test-only peripheral model for firmware lifecycle and layout checks.
 * Nearest sampling checks geometry, not the PPA bilinear filter.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
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
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#include "esp_async_color_convert.h"
#endif
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
static unsigned ppa_calls;
static unsigned blend_calls;
static unsigned ppa_clients, fail_ppa_client;
static unsigned fail_menu_alloc;
static bool fail_hud_runs;

void display_model_fail_hud_runs(void)
{
    fail_hud_runs = true;
}

void display_model_fail_ppa_client(unsigned registration)
{
    assert(!ppa_clients);
    fail_ppa_client = registration;
}

void display_model_ppa_idle(void)
{
    assert(!ppa_clients && !fail_ppa_client);
}

void display_model_fail_menu_alloc(unsigned allocation)
{
    fail_menu_alloc = allocation;
}

static bool menu_alloc_failed(size_t bytes)
{
    return fail_menu_alloc &&
            (bytes == 320 * 240 * 2 || bytes == 320 * 240 * 4 ||
                    bytes == 480 * 640 * 4) && !--fail_menu_alloc;
}

void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    return menu_alloc_failed(size) ? NULL : __real_heap_caps_aligned_alloc(alignment, size, caps);
}

void *__real_heap_caps_malloc(size_t size, uint32_t caps);
void *__wrap_heap_caps_malloc(size_t size, uint32_t caps)
{
    if (fail_hud_runs && size == 720 * 26 * 2 * 6)
    {
        fail_hud_runs = false;
        return NULL;
    }
    return menu_alloc_failed(size) ? NULL : __real_heap_caps_malloc(size, caps);
}

unsigned display_model_blend_calls(void)
{
    return blend_calls;
}

unsigned display_model_ppa_calls(void)
{
    return ppa_calls;
}

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
    assert(config->num_data_lanes == 2);
#if CONFIG_GPSP_LCD_DSI_OSPTEK
    assert(config->lane_bit_rate_mbps == 820);
#else
    assert(config->lane_bit_rate_mbps == 500);
#endif
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
    assert(xPortGetCoreID() == 1);
    if (fail_ppa_client && !--fail_ppa_client)
        return ESP_ERR_NO_MEM;
    assert(config->oper_type == PPA_OPERATION_SRM ||
            config->oper_type == PPA_OPERATION_BLEND);
    struct model_client *client = calloc(1, sizeof(*client));
    assert(client);
    const esp_timer_create_args_t timer = {
        .callback = ppa_tick, .arg = client,
        .dispatch_method = ESP_TIMER_ISR, .name = "ppa_model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &client->timer));
    ppa_clients++;
    *handle = (void *)client;
    return ESP_OK;
}

esp_err_t __wrap_ppa_unregister_client(ppa_client_handle_t handle)
{
    struct model_client *client = (void *)handle;
    ESP_ERROR_CHECK(esp_timer_delete(client->timer));
    free(client);
    assert(ppa_clients);
    ppa_clients--;
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
    ppa_calls++;
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

esp_err_t __wrap_ppa_do_blend(ppa_client_handle_t handle,
        const ppa_blend_oper_config_t *op)
{
    (void)handle;
    blend_calls++;
    assert(op->in_bg.blend_cm == PPA_BLEND_COLOR_MODE_RGB565 &&
            op->in_fg.blend_cm == PPA_BLEND_COLOR_MODE_ARGB8888 &&
            op->out.blend_cm == PPA_BLEND_COLOR_MODE_RGB565);
    assert(op->bg_alpha_update_mode == PPA_ALPHA_FIX_VALUE &&
            op->bg_alpha_fix_val == 255 &&
            (op->fg_alpha_update_mode == PPA_ALPHA_NO_CHANGE ||
                    op->fg_alpha_update_mode == PPA_ALPHA_SCALE));
    assert(op->mode == PPA_TRANS_MODE_BLOCKING);
    const uint16_t *bg = op->in_bg.buffer;
    const uint32_t *fg = op->in_fg.buffer;
    uint16_t *output = op->out.buffer;
    assert(!panel_model || output != panel_model->active);
    for (unsigned y = 0; y < op->in_bg.block_h; y++)
        for (unsigned x = 0; x < op->in_bg.block_w; x++)
        {
            uint16_t b = bg[(y + op->in_bg.block_offset_y) * op->in_bg.pic_w +
                    x + op->in_bg.block_offset_x];
            uint32_t f = fg[(y + op->in_fg.block_offset_y) * op->in_fg.pic_w +
                    x + op->in_fg.block_offset_x];
            unsigned alpha = f >> 24;
            if (op->fg_alpha_update_mode == PPA_ALPHA_SCALE)
                alpha = (unsigned)(alpha * op->fg_alpha_scale_ratio);
            unsigned r = ((f >> 16 & 255) * alpha +
                    (b >> 11) * 255 / 31 * (255 - alpha) + 127) / 255;
            unsigned g = ((f >> 8 & 255) * alpha +
                    (b >> 5 & 63) * 255 / 63 * (255 - alpha) + 127) / 255;
            unsigned blue = ((f & 255) * alpha +
                    (b & 31) * 255 / 31 * (255 - alpha) + 127) / 255;
            uint16_t pixel =
                    ((r * 31 + 127) / 255 << 11) |
                    ((g * 63 + 127) / 255 << 5) | ((blue * 31 + 127) / 255);
            if (f == 0xc0000000u && op->fg_alpha_update_mode == PPA_ALPHA_NO_CHANGE)
                pixel = (b >> 2) & 0x39e7;
            output[(y + op->out.block_offset_y) * op->out.pic_w +
                    x + op->out.block_offset_x] = pixel;
        }
    return esp_cache_msync(output, op->out.buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
struct model_copy;
struct model_copy_job
{
    struct model_copy *copy;
    async_color_convert_request_t request;
    async_color_convert_isr_cb_t callback;
    void *arg;
    esp_timer_handle_t timer;
    atomic_bool busy;
    atomic_bool owned;
    unsigned sequence;
    uint32_t checksum;
};

struct model_copy
{
    struct model_copy_job jobs[3];
    unsigned sequence, fail_at;
};
static struct model_copy *copy_model;
static atomic_uint copy_peak, copy_reordered;

esp_err_t __wrap_esp_color_convert_blocking(async_color_convert_handle_t handle,
        const async_color_convert_request_t *request, int timeout)
{
    (void)timeout;
    assert((void *)handle == copy_model);
    /* Stats borrows both tiles only after the scaling queue is drained. */
    for (unsigned i = 0; i < 3; i++)
        assert(!atomic_load(&copy_model->jobs[i].owned));
    assert(request->src_color_format == ESP_COLOR_FOURCC_RGB16 &&
            request->dst_color_format == ESP_COLOR_FOURCC_RGB16);
    assert(request->copy_width == 26 &&
            (request->copy_height == 252 || request->copy_height == 720));
    const uint16_t *source = request->src_buffer;
    uint16_t *dest = request->dst_buffer;

    assert(!panel_model || dest != panel_model->active);
    for (unsigned y = 0; y < request->copy_height; y++)
        memcpy(dest + (y + request->dst_y) * request->dst_stride + request->dst_x,
                source + (y + request->src_y) * request->src_stride + request->src_x,
                request->copy_width * sizeof(*dest));
    return esp_cache_msync(dest, request->dst_stride * request->dst_height * 2,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void display_model_dma_fail_after(unsigned requests)
{
    assert(copy_model);
    copy_model->fail_at = copy_model->sequence + requests;
}

void display_model_dma_check(void)
{
    assert(atomic_load(&copy_peak) == 2);
    assert(atomic_load(&copy_reordered));
}

static uint32_t copy_checksum(const async_color_convert_request_t *request)
{
    const uint16_t *source = request->src_buffer;
    uint32_t sum = 2166136261u;
    for (unsigned y = 0; y < request->copy_height; y++)
        for (unsigned x = 0; x < request->copy_width; x++)
            sum = (sum ^ source[(y + request->src_y) * request->src_stride +
                    x + request->src_x]) * 16777619u;
    return sum;
}

static void copy_tick(void *arg)
{
    struct model_copy_job *job = arg;
    /* Delayed completion keeps source ownership observable after submission. */
    assert(job->checksum == copy_checksum(&job->request));
    for (unsigned i = 0; i < 3; i++)
        if (atomic_load(&job->copy->jobs[i].owned) &&
                job->copy->jobs[i].sequence < job->sequence)
            atomic_fetch_add(&copy_reordered, 1);
    atomic_store(&job->owned, false);
    async_color_convert_event_data_t event = {};
    bool yield = job->callback((void *)job->copy, &event, job->arg);
    atomic_store(&job->busy, false);
    if (yield)
        esp_timer_isr_dispatch_need_yield();
}

esp_err_t __wrap_esp_async_color_convert_install_dma2d(
        const async_color_convert_config_t *config,
        async_color_convert_handle_t *handle)
{
    assert(!copy_model && config->backlog == 3);
    struct model_copy *copy = calloc(1, sizeof(*copy));
    assert(copy);
    for (unsigned i = 0; i < 3; i++)
    {
        struct model_copy_job *job = &copy->jobs[i];
        job->copy = copy;
        atomic_init(&job->busy, false);
        atomic_init(&job->owned, false);
        const esp_timer_create_args_t timer = {
            .callback = copy_tick, .arg = job,
            .dispatch_method = ESP_TIMER_ISR, .name = "copy_model",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer, &job->timer));
    }
    copy_model = copy;
    *handle = (void *)copy;
    return ESP_OK;
}

esp_err_t __wrap_esp_async_color_convert_uninstall(async_color_convert_handle_t handle)
{
    struct model_copy *copy = (void *)handle;
    assert(copy == copy_model);
    for (unsigned i = 0; i < 3; i++)
        if (atomic_load(&copy->jobs[i].busy))
            return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 3; i++)
        ESP_ERROR_CHECK(esp_timer_delete(copy->jobs[i].timer));
    free(copy);
    copy_model = NULL;
    return ESP_OK;
}

esp_err_t __wrap_esp_async_color_convert(async_color_convert_handle_t handle,
        const async_color_convert_request_t *request,
        async_color_convert_isr_cb_t callback, void *arg)
{
    struct model_copy *copy = (void *)handle;
    assert(copy == copy_model);
    if (++copy->sequence == copy->fail_at)
        return ESP_FAIL;
    unsigned active = 0;
    struct model_copy_job *job = NULL;
    for (unsigned i = 0; i < 3; i++)
    {
        if (atomic_load(&copy->jobs[i].owned))
            active++;
        if (!atomic_load(&copy->jobs[i].busy) && !job)
            job = &copy->jobs[i];
    }
    assert(job && active < 2);
    if (active + 1 > atomic_load(&copy_peak))
        atomic_store(&copy_peak, active + 1);
    job->request = *request;
    job->callback = callback;
    job->arg = arg;
    job->sequence = copy->sequence;
    job->checksum = copy_checksum(request);
    const uint16_t *source = request->src_buffer;
    uint16_t *dest = request->dst_buffer;
    for (unsigned y = 0; y < request->copy_height; y++)
        memcpy(dest + (y + request->dst_y) * request->dst_stride + request->dst_x,
                source + (y + request->src_y) * request->src_stride + request->src_x,
                request->copy_width * sizeof(*dest));
    ESP_ERROR_CHECK(esp_cache_msync(dest, request->dst_stride * request->dst_height * 2,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    atomic_store(&job->busy, true);
    atomic_store(&job->owned, true);
    return esp_timer_start_once(job->timer, job->sequence % 2 ? 20000 : 100);
}
#endif

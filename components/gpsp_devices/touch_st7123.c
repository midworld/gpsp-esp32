/* ST7123 polling on the shared panel/codec bus.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"
#include "gpsp_touch.h"
#include "gpsp_hardware.h"
#include "gpsp_i2c.h"
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gpsp_touch";
static i2c_master_dev_handle_t s_device;
static bool s_bus_ready;
static atomic_bool s_running, s_ready;
static atomic_uint s_snapshot;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct gpsp_virtual_pad s_pad;
static void (*s_changed)(void);
static unsigned s_width, s_height, s_contacts;

static esp_err_t touch_read(unsigned reg, void *data, size_t bytes)
{
    const uint8_t address[2] = {reg >> 8, reg};
    return i2c_master_transmit_receive(s_device, address, 2, data, bytes, 5);
}

static bool touch_publish(void)
{
    unsigned value = (s_pad.enabled ? GPSP_VIRTUAL_PAD_VISIBLE : 0) | s_pad.buttons;
    return atomic_exchange_explicit(&s_snapshot, value, memory_order_relaxed) != value;
}

static void touch_task(void *arg)
{
    unsigned errors = 0, failures = 0, samples = 0, read_max = 0, max_contacts = 0;
    unsigned seen_buttons = 0;
    int64_t log_us = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    (void)arg;

    ESP_LOGI(TAG, "Poll core=%d priority=6 period=8ms", xPortGetCoreID());
    while (atomic_load(&s_running))
    {
        uint8_t report[4 + GPSP_TOUCH_CONTACTS * 7];
        struct gpsp_touch_contact contacts[GPSP_TOUCH_CONTACTS];
        int64_t start = esp_timer_get_time();
        esp_err_t result = touch_read(0x10, report, 4 + s_contacts * 7);
        bool valid = result == ESP_OK && gpsp_st7123_decode(report,
                4 + s_contacts * 7, s_contacts, s_width, s_height,
#if CONFIG_GPSP_LCD_ROTATE_180
                true,
#else
                false,
#endif
                contacts);
        unsigned elapsed = esp_timer_get_time() - start;
        if (elapsed > read_max)
            read_max = elapsed;
        samples++;
        if (!valid)
        {
            errors++;
            if (!failures++)
                ESP_LOGW(TAG, "Read failed: %s; releasing contacts",
                        result == ESP_OK ? "invalid report" : esp_err_to_name(result));
        }
        else
        {
            failures = 0;
            unsigned count = 0;
            for (unsigned i = 0; i < GPSP_TOUCH_CONTACTS; i++)
                count += contacts[i].down;
            if (count > max_contacts)
                max_contacts = count;
        }
        portENTER_CRITICAL(&s_lock);
        if (valid)
            gpsp_virtual_pad_update(&s_pad, contacts, start / 1000);
        else
            gpsp_virtual_pad_cancel(&s_pad);
        bool changed = touch_publish();
        portEXIT_CRITICAL(&s_lock);
        if (changed && s_changed)
            s_changed();
        seen_buttons |= gpsp_touch_read() & 0x1ffff;
        if (start - log_us >= 10000000)
        {
            ESP_LOGI(TAG, "samples=%u errors=%u read_max_us=%u contacts_max=%u "
                    "pad=%d seen=0x%05x stack=%u", samples, errors,
                    read_max, max_contacts, gpsp_touch_enabled(),
                    seen_buttons,
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
            samples = errors = read_max = max_contacts = 0;
            seen_buttons = 0;
            log_us = start;
        }
        TickType_t period = pdMS_TO_TICKS(failures ? 50 : 8);
        xTaskDelayUntil(&wake, period ? period : 1);
    }
    atomic_store(&s_ready, false);
    vTaskDelete(NULL);
}

esp_err_t gpsp_touch_init(void (*changed)(void))
{
    i2c_master_bus_handle_t bus;
    if (!gpsp_hardware.touch.address)
        return ESP_ERR_NOT_SUPPORTED;
    if (s_device)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = gpsp_i2c_acquire(&bus);
    if (result != ESP_OK)
        return result;
    s_bus_ready = true;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = gpsp_hardware.touch.address,
        .scl_speed_hz = 400000,
    };
    result = i2c_master_bus_add_device(bus, &config, &s_device);
    if (result != ESP_OK)
        goto fail;
    uint8_t info[5], version;
    result = touch_read(0x05, info, sizeof(info));
    if (result != ESP_OK)
        goto fail;
    s_width = ((info[0] & 63) << 8) | info[1];
    s_height = ((info[2] & 63) << 8) | info[3];
    s_contacts = info[4];
    if (!s_width || !s_height || !s_contacts || s_contacts > GPSP_TOUCH_CONTACTS)
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    result = touch_read(0, &version, 1);
    if (result != ESP_OK)
        goto fail;
    ESP_LOGI(TAG, "ST7123 address=0x%02x firmware=%u size=%ux%u contacts=%u",
            config.device_address, version, s_width, s_height, s_contacts);
    gpsp_virtual_pad_enable(&s_pad, false);
    s_changed = changed;
    atomic_store(&s_running, true);
    atomic_store(&s_ready, true);
    if (xTaskCreatePinnedToCore(touch_task, "gpsp_touch", 3072, NULL, 6, NULL,
#if CONFIG_FREERTOS_UNICORE
            tskNO_AFFINITY
#else
            1
#endif
            ) != pdPASS)
    {
        atomic_store(&s_running, false);
        atomic_store(&s_ready, false);
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    return ESP_OK;
fail:
    ESP_LOGW(TAG, "Initialization failed: %s", esp_err_to_name(result));
    gpsp_touch_shutdown();
    return result;
}

bool gpsp_touch_available(void) { return atomic_load(&s_ready); }
uint32_t gpsp_touch_read(void) { return atomic_load_explicit(&s_snapshot, memory_order_relaxed); }
bool gpsp_touch_enabled(void) { return (gpsp_touch_read() & GPSP_VIRTUAL_PAD_VISIBLE) != 0; }

void gpsp_touch_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    gpsp_virtual_pad_enable(&s_pad, enabled && gpsp_touch_available());
    bool changed = touch_publish();
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_changed)
        s_changed();
}

void gpsp_touch_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    gpsp_virtual_pad_cancel(&s_pad);
    bool changed = touch_publish();
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_changed)
        s_changed();
}

void gpsp_touch_shutdown(void)
{
    atomic_store(&s_running, false);
    while (atomic_load(&s_ready))
        vTaskDelay(1);
    s_changed = NULL;
    gpsp_touch_set_enabled(false);
    if (s_device)
        i2c_master_bus_rm_device(s_device);
    s_device = NULL;
    if (s_bus_ready)
        gpsp_i2c_release();
    s_bus_ready = false;
}

/* Drive the production audio worker with modeled codec and DMA writes.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include "sdkconfig.h"
#define CONFIG_GPSP_AUDIO 1
#define CONFIG_GPSP_AUDIO_VOLUME_DB (-60)
#include "../../../components/gpsp_devices/audio_es8311.c"
#include "esp_timer.h"

static atomic_uint blocks, control, dac_volume, block_writes;
static atomic_bool fail_write, nonzero;

esp_err_t __wrap_i2c_master_transmit(i2c_master_dev_handle_t device,
        const uint8_t *data, size_t size, int timeout)
{
    assert(device == s_codec && size == 2 && timeout == 5);
    assert(atomic_fetch_add(&block_writes, 1) == 0);
    if (atomic_exchange(&fail_write, false))
        return ESP_FAIL;
    if (data[0] == ES_DAC_CONTROL)
    {
        assert((data[1] & ~AUDIO_MUTE) == 4);
        atomic_store(&control, data[1]);
    }
    else
    {
        assert(data[0] == ES_DAC_VOLUME);
        atomic_store(&dac_volume, data[1]);
    }
    return ESP_OK;
}

esp_err_t __wrap_i2s_channel_write(i2s_chan_handle_t channel, const void *data,
        size_t size, size_t *written, uint32_t timeout)
{
    assert(channel == s_channel && timeout == 1000);
    const int16_t *samples = data;
    for (unsigned i = 0; i < size / sizeof(*samples); i++)
        if (samples[i])
            atomic_store(&nonzero, true);
    *written = size;
    atomic_store(&block_writes, 0);
    atomic_fetch_add(&blocks, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static void wait_register(atomic_uint *reg, unsigned value)
{
    int64_t end = esp_timer_get_time() + 1000000;
    while (atomic_load(reg) != value)
    {
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    }
}

void audio_mute_check(void)
{
    s_queue = heap_caps_calloc(AUDIO_QUEUE, sizeof(*s_queue), MALLOC_CAP_SPIRAM);
    int16_t *samples = heap_caps_malloc(AUDIO_QUEUE * 4, MALLOC_CAP_SPIRAM);
    assert(s_queue && samples);
    for (unsigned i = 0; i < AUDIO_QUEUE * 2; i++)
        samples[i] = 1000;
    s_codec = (void *)1;
    s_channel = (void *)2;
    s_dac_control = AUDIO_MUTE | 4;
    atomic_store(&control, s_dac_control);
    atomic_store(&dac_volume, 71);
    atomic_store(&s_volume_db, -24);
    atomic_store(&s_running, true);
    gpsp_audio_set_muted(true);
    assert(xTaskCreatePinnedToCore(audio_task, "mute_check", 4096, NULL,
            6, &s_task, 1) == pdPASS);
    assert(gpsp_audio_write(samples, AUDIO_QUEUE) == AUDIO_QUEUE);
    int64_t end = esp_timer_get_time() + 1000000;
    while (atomic_load(&blocks) < 4)
    {
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    }
    assert(atomic_load(&s_tail) && atomic_load(&nonzero));
    assert((atomic_load(&control) & AUDIO_MUTE) == AUDIO_MUTE);
    assert(gpsp_audio_volume_step(0) == -24);
    atomic_store(&fail_write, true);
    gpsp_audio_set_muted(false);
    wait_register(&control, 4);
    assert(!gpsp_audio_muted() && gpsp_audio_volume_step(0) == -24);
    gpsp_audio_set_muted(true);
    wait_register(&control, AUDIO_MUTE | 4);
    assert(gpsp_audio_volume_step(1) == -21);
    wait_register(&dac_volume, 149);
    assert((atomic_load(&control) & AUDIO_MUTE) == AUDIO_MUTE);
    atomic_store(&s_running, false);
    while (s_task)
        vTaskDelay(1);
    vTaskDelay(20);
    heap_caps_free(samples);
    heap_caps_free(s_queue);
    s_queue = NULL;
    s_codec = NULL;
    s_channel = NULL;
    puts("USB_BT_CHECK DAC mute/retry/volume/queue pacing: pass");
}

/* Speaker output through the ES8311 DAC.
 * Codec setup follows the ES8311 register specification, revision 10.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"

#include "gpsp_audio.h"
#include "gpsp_i2c.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_AUDIO

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_I2C_ADDRESS gpsp_hardware.audio.address
#define AUDIO_PIN_AMPLIFIER gpsp_hardware.audio.amplifier
#define AUDIO_RATE GPSP_AUDIO_RATE

/* I2S DMA: 4 x about 5 ms. Align 16-bit stereo buffers to 64 bytes,
   and fill two whole DMA buffers per write. */
#define AUDIO_DMA_COUNT 4
#define AUDIO_DMA_FRAMES (((AUDIO_RATE / 200 + 15) / 16) * 16)
#define AUDIO_BLOCK (AUDIO_DMA_FRAMES * 2)

/* Mono queue (power of two). Play starts at the target. The frontend
   keeps the level near it by pacing frames (see audio_balance), and the
   target covers the frames that run late when a game changes scenes. */
#define AUDIO_QUEUE 8192u
#define AUDIO_TARGET (AUDIO_RATE * 100 / 1000)

/* When the game cannot keep up, frame pacing cannot refill the queue.
   Below this level the player stretches sound instead, up to 6% slower
   two blocks above a shortage, so it lowers pitch, not gaps. The level
   settles where the stretch matches the game's shortfall (55 ms at 3%). */
#define AUDIO_STRETCH_START (AUDIO_RATE * 90 / 1000)
#define AUDIO_STRETCH_FULL (AUDIO_BLOCK * 2)
#define AUDIO_STRETCH_MAX (65536u * 6 / 100)

/* Viewer volume keys move the DAC volume in these steps, 0 dB at most. */
#define AUDIO_VOLUME_STEP_DB 3
#define AUDIO_VOLUME_MIN_DB (-60)

#define AUDIO_CLOCKS ((1u << 5) | (1u << 4) | (1u << 2) | (1u << 0))
#define AUDIO_MUTE ((1u << 6) | (1u << 5))
#define AUDIO_POWER ((1u << 5) | (1u << 4) | (1u << 2))

#if CONFIG_FREERTOS_UNICORE
#define AUDIO_TASK_CORE tskNO_AFFINITY
#else
#define AUDIO_TASK_CORE 1
#endif

/* Addresses and bit fields are hardware facts from datasheet section 8. */
enum es8311_register
{
    ES_RESET = 0x00,
    ES_CLOCK_GATE = 0x01,
    ES_CLOCK_PRE = 0x02,
    ES_DAC_OSR = 0x04,
    ES_CLOCK_DIV = 0x05,
    ES_LRCK_HIGH = 0x07,
    ES_LRCK_LOW = 0x08,
    ES_DAC_SERIAL = 0x09,
    ES_ADC_SERIAL = 0x0a,
    ES_ANALOG_POWER = 0x0d,
    ES_ADC_POWER = 0x0e,
    ES_DAC_POWER = 0x12,
    ES_OUTPUT_DRIVE = 0x13,
    ES_INPUT_SELECT = 0x14,
    ES_DAC_CONTROL = 0x31,
    ES_DAC_VOLUME = 0x32,
    ES_DAC_RAMP = 0x37,
    ES_REGISTER_RESET = 0xfa,
    ES_ID_HIGH = 0xfd,
    ES_ID_LOW = 0xfe,
};

static const char *TAG = "gpsp_audio";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_channel;
static bool s_channel_enabled;
static int16_t *s_queue;
static atomic_uint s_head;
static atomic_uint s_tail;
static atomic_uint s_underruns;
/* Requested DAC volume; the audio task owns the codec and applies it. */
static atomic_int s_volume_db;
static atomic_bool s_muted;
static uint8_t s_dac_control;
/* Running average of the level while playing; the raw level otherwise. */
static atomic_uint s_smooth_level;
static atomic_uint s_i2s_overflows;
static atomic_bool s_running;
static TaskHandle_t s_task;

/* The I2S send queue overflowed: the writer fell behind the DMA, which
   usually plays a cleared (silent) buffer. */
static bool IRAM_ATTR audio_i2s_overflow(i2s_chan_handle_t channel,
        i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    (void)arg;
    atomic_fetch_add(&s_i2s_overflows, 1);
    return false;
}

static void audio_delay(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t codec_transfer(uint8_t reg, uint8_t *value, bool read)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;

    for (unsigned attempt = 0; s_codec && attempt < 3; attempt++)
    {
        if (read)
            result = i2c_master_transmit_receive(s_codec, &reg, 1, value, 1,
                    100);
        else
        {
            uint8_t message[2] = {reg, *value};
            result = i2c_master_transmit(s_codec, message, sizeof(message), 100);
        }
        if (result == ESP_OK)
            return result;
        audio_delay(5);
    }
    ESP_LOGE(TAG, "Codec %s at 0x%02x failed: %s", read ? "read" : "write",
            reg, esp_err_to_name(result));
    return result;
}

static esp_err_t codec_expect(uint8_t reg, uint8_t mask, uint8_t expected)
{
    uint8_t actual;
    esp_err_t result = codec_transfer(reg, &actual, true);

    if (result != ESP_OK)
        return result;
    if ((actual & mask) == expected)
        return ESP_OK;
    ESP_LOGE(TAG, "Codec 0x%02x masked=0x%02x expected=0x%02x", reg,
            actual & mask, expected);
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t codec_set(uint8_t reg, uint8_t value)
{
    return codec_transfer(reg, &value, false);
}

static esp_err_t codec_field(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    esp_err_t result = codec_transfer(reg, &current, true);

    if (result != ESP_OK)
        return result;
    result = codec_set(reg, (uint8_t)((current & ~mask) | (value & mask)));
    if (result != ESP_OK)
        return result;
    return codec_expect(reg, mask, value & mask);
}

/* DAC volume is -95.5 dB + register / 2 dB. */
static esp_err_t codec_volume(int db)
{
    return codec_set(ES_DAC_VOLUME, (uint8_t)(2 * db + 191));
}

/* Use one short write while playing; retry on a later block to limit
   blocking while feeding the DMA. */
static bool codec_try_volume(int db)
{
    uint8_t message[2] = {ES_DAC_VOLUME, (uint8_t)(2 * db + 191)};

    return s_codec &&
            i2c_master_transmit(s_codec, message, sizeof(message), 5) == ESP_OK;
}

static esp_err_t codec_mute(bool mute)
{
    esp_err_t result = codec_field(ES_DAC_CONTROL, AUDIO_MUTE,
            mute ? AUDIO_MUTE : 0);

    return result == ESP_OK ?
            codec_transfer(ES_DAC_CONTROL, &s_dac_control, true) : result;
}

static bool codec_try_mute(bool mute)
{
    uint8_t value = (s_dac_control & ~AUDIO_MUTE) | (mute ? AUDIO_MUTE : 0);
    uint8_t message[2] = {ES_DAC_CONTROL, value};

    if (!s_codec || i2c_master_transmit(s_codec, message, sizeof(message), 5) != ESP_OK)
        return false;
    s_dac_control = value;
    return true;
}

#define CODEC_TRY(call) do { result = (call); if (result != ESP_OK) return result; } while (0)

/* The register values depend only on MCLK / Fs = 256, not on the rate. */
static esp_err_t codec_start(void)
{
    esp_err_t result;

    CODEC_TRY(codec_expect(ES_ID_HIGH, 0xff, 0x83));
    CODEC_TRY(codec_expect(ES_ID_LOW, 0xff, 0x11));
    /* Reset registers as well as digital logic after an MCU-only reset. */
    CODEC_TRY(codec_set(ES_RESET, 0x1f));
    CODEC_TRY(codec_set(ES_REGISTER_RESET, 1));
    CODEC_TRY(codec_set(ES_REGISTER_RESET, 0));
    audio_delay(5);
    CODEC_TRY(codec_mute(true));
    CODEC_TRY(codec_volume(-60));

    /* DAC clock = MCLK, DAC oversampling = 64 * Fs. */
    CODEC_TRY(codec_field(ES_CLOCK_PRE, 0xf8, 0));
    CODEC_TRY(codec_field(ES_CLOCK_DIV, 0x0f, 0));
    CODEC_TRY(codec_field(ES_DAC_OSR, 0x7f, 64 / 4));
    CODEC_TRY(codec_field(ES_LRCK_HIGH, 0x3f, 1u << 4));
    CODEC_TRY(codec_set(ES_LRCK_LOW, 256 - 1));
    CODEC_TRY(codec_field(ES_DAC_SERIAL, 0xff, 3u << 2));

    /* Only the DAC path is used; preserve reserved bits in analog registers. */
    CODEC_TRY(codec_field(ES_ADC_SERIAL, 1u << 6, 1u << 6));
    CODEC_TRY(codec_field(ES_ADC_POWER, 0x70, 0x70));
    CODEC_TRY(codec_field(ES_INPUT_SELECT, 0x50, 0));
    CODEC_TRY(codec_field(ES_OUTPUT_DRIVE, 1u << 4, 0));
    CODEC_TRY(codec_field(ES_DAC_RAMP, 0xf8, (4u << 4) | (1u << 3)));
    CODEC_TRY(codec_field(ES_CLOCK_GATE, 0x3f, AUDIO_CLOCKS));
    CODEC_TRY(codec_field(ES_ANALOG_POWER, 0xff, AUDIO_POWER | 3));
    CODEC_TRY(codec_field(ES_DAC_POWER, 0x03, 0));
    /* Slave port; ADC and unused master logic stay in reset. */
    CODEC_TRY(codec_set(ES_RESET, (1u << 7) | (1u << 2) | (1u << 1)));
    audio_delay(100);
    CODEC_TRY(codec_field(ES_ANALOG_POWER, 0x03, 2));
    CODEC_TRY(codec_expect(ES_RESET, 0xff, 0x86));
    return ESP_OK;
}

#undef CODEC_TRY

/* Tail first: head only grows, so the difference never goes negative even
   while both sides move. */
static unsigned audio_level(void)
{
    unsigned tail = atomic_load(&s_tail);
    unsigned level = atomic_load(&s_head) - tail;

    return level < AUDIO_QUEUE ? level : AUDIO_QUEUE;
}

/* Writes one block of mono as the stereo slots the ES8311 reads. */
static void audio_send(const int16_t *mono, unsigned frames)
{
    static int16_t slots[(AUDIO_BLOCK + 1) * 2];
    size_t written = 0;

    for (unsigned i = 0; i < frames; i++)
        slots[2 * i] = slots[2 * i + 1] = mono[i];
    /* A stalled I2S must not keep shutdown waiting forever. */
    if (i2s_channel_write(s_channel, slots, frames * 4, &written, 1000) !=
            ESP_OK)
        ESP_LOGW(TAG, "I2S write timed out");
}

/* CPU1: blocks on the I2S DMA, so the speaker clock paces the queue. */
static void audio_task(void *arg)
{
    static int16_t block[AUDIO_BLOCK];
    static const int16_t silence[AUDIO_BLOCK];
    bool playing = false;
    bool ready = false;
    bool muted = true;
    int volume_db = -60;
    unsigned average = 0;
    uint32_t fraction = 0;

    (void)arg;
    /* Overflows before the first write come from codec setup, not play. */
    atomic_store(&s_i2s_overflows, 0);
    while (atomic_load(&s_running))
    {
        bool wanted_mute = atomic_load(&s_muted) || !ready;
        int wanted_volume = atomic_load(&s_volume_db);

        /* At most one short codec write per block; mute takes priority. */
        if (wanted_mute && !muted)
        {
            if (codec_try_mute(true))
                muted = true;
        }
        else if (volume_db != wanted_volume)
        {
            if (codec_try_volume(wanted_volume))
                volume_db = wanted_volume;
        }
        else if (!wanted_mute && muted && codec_try_mute(false))
            muted = false;
        unsigned level = audio_level();

        /* Start, or restart after a shortage, only with the target queued,
           so the next GBA frame never finds an almost empty queue. */
        if (!playing && level >= AUDIO_TARGET)
        {
            playing = true;
            average = level << 4;
            fraction = 0;
        }
        else if (playing && level < AUDIO_BLOCK + 2)
        {
            playing = false;
            atomic_fetch_add(&s_underruns, 1);
        }
        if (!playing)
        {
            atomic_store(&s_smooth_level, level);
            audio_send(silence, AUDIO_BLOCK);
            continue;
        }

        /* Sound arrives one GBA frame (about 17 ms) at a time: decide on
           a running average (16 blocks) of the level. */
        average += level - (average >> 4);
        unsigned smooth = average >> 4;
        atomic_store(&s_smooth_level, smooth);
        uint32_t step = 65536;
        if (smooth < AUDIO_STRETCH_START)
        {
            unsigned depth = AUDIO_STRETCH_START - smooth;
            if (depth > AUDIO_STRETCH_START - AUDIO_STRETCH_FULL)
                depth = AUDIO_STRETCH_START - AUDIO_STRETCH_FULL;
            step -= AUDIO_STRETCH_MAX * depth /
                    (AUDIO_STRETCH_START - AUDIO_STRETCH_FULL);
        }

        unsigned tail = atomic_load(&s_tail);
        if (step == 65536 && fraction)
        {
            /* Back at full speed: round to the nearest sample (at most half
               a sample of phase) and copy samples unchanged again. */
            tail += fraction >> 15;
            fraction = 0;
        }
        if (step == 65536)
        {
            for (unsigned i = 0; i < AUDIO_BLOCK; i++)
                block[i] = s_queue[(tail + i) & (AUDIO_QUEUE - 1)];
            tail += AUDIO_BLOCK;
        }
        else
        {
            /* Read at a 16.16 position; 15 fraction bits keep the product
               within 32 bits. */
            uint32_t position = fraction;
            for (unsigned i = 0; i < AUDIO_BLOCK; i++, position += step)
            {
                unsigned index = tail + (position >> 16);
                int32_t a = s_queue[index & (AUDIO_QUEUE - 1)];
                int32_t b = s_queue[(index + 1) & (AUDIO_QUEUE - 1)];
                int32_t f = (position & 0xffff) >> 1;
                block[i] = (int16_t)(a + (((b - a) * f) >> 15));
            }
            tail += position >> 16;
            fraction = position & 0xffff;
        }
        atomic_store(&s_tail, tail);
        audio_send(block, AUDIO_BLOCK);
        ready = true;
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

size_t gpsp_audio_write(const int16_t *stereo, size_t frames)
{
    unsigned head;
    unsigned room;

    if (!s_task)
        return 0;
    head = atomic_load(&s_head);
    room = AUDIO_QUEUE - (head - atomic_load(&s_tail));
    if (frames > room)
        frames = room;
    /* One speaker: mix both channels. */
    for (size_t i = 0; i < frames; i++)
        s_queue[(head + i) & (AUDIO_QUEUE - 1)] =
                (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
    atomic_store(&s_head, head + (unsigned)frames);
    return frames;
}

float gpsp_audio_balance(void)
{
    if (!s_task)
        return 0.0f;

    /* The average: sound arrives a GBA frame at a time, so the raw level
       swings by about 17 ms. */
    float balance = ((float)atomic_load(&s_smooth_level) - AUDIO_TARGET) /
            AUDIO_TARGET;
    return balance > 1.0f ? 1.0f : balance;
}

float gpsp_audio_fill(void)
{
    return s_task ? (float)audio_level() / AUDIO_TARGET : 0.0f;
}

int gpsp_audio_volume_step(int steps)
{
    int db = atomic_load(&s_volume_db);

    if (!steps)
        return db;
    /* The menu and serial input may update the volume together. */
    for (;;)
    {
        int next = db + steps * AUDIO_VOLUME_STEP_DB;

        if (next > 0)
            next = 0;
        if (next < AUDIO_VOLUME_MIN_DB)
            next = AUDIO_VOLUME_MIN_DB;
        if (atomic_compare_exchange_weak(&s_volume_db, &db, next))
            return next;
    }
}

bool gpsp_audio_muted(void)
{
    return atomic_load(&s_muted);
}

void gpsp_audio_set_muted(bool muted)
{
    atomic_store(&s_muted, muted);
}

void gpsp_audio_stats(unsigned *queued_ms, unsigned *underruns,
        unsigned *i2s_overflows)
{
    *queued_ms = s_task ? audio_level() * 1000u / AUDIO_RATE : 0;
    *underruns = atomic_load(&s_underruns);
    *i2s_overflows = atomic_load(&s_i2s_overflows);
}

#define AUDIO_TRY(call) do { result = (call); if (result != ESP_OK) goto fail; } while (0)

esp_err_t gpsp_audio_init(void)
{
    const gpio_config_t amplifier = {
        .pin_bit_mask = 1ULL << AUDIO_PIN_AMPLIFIER,
        .mode = GPIO_MODE_OUTPUT,
    };
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AUDIO_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2s_chan_config_t dma = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
            I2S_ROLE_MASTER);
    i2s_std_config_t wire = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = gpsp_hardware.audio.mclk,
            .bclk = gpsp_hardware.audio.bclk,
            .ws = gpsp_hardware.audio.ws,
            .dout = gpsp_hardware.audio.dout,
            .din = I2S_GPIO_UNUSED,
        },
    };
    const i2s_event_callbacks_t callbacks = {
        .on_send_q_ovf = audio_i2s_overflow,
    };
    i2s_chan_info_t clocks;
    esp_err_t result;

    if (s_task)
        return ESP_ERR_INVALID_STATE;
    s_queue = heap_caps_malloc(AUDIO_QUEUE * sizeof(*s_queue),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue)
        return ESP_ERR_NO_MEM;
    atomic_store(&s_head, 0);
    atomic_store(&s_tail, 0);
    atomic_store(&s_underruns, 0);
    atomic_store(&s_volume_db, CONFIG_GPSP_AUDIO_VOLUME_DB);
    atomic_store(&s_muted, false);
    atomic_store(&s_smooth_level, 0);
    atomic_store(&s_i2s_overflows, 0);

    AUDIO_TRY(gpio_set_level(AUDIO_PIN_AMPLIFIER, 0));
    AUDIO_TRY(gpio_config(&amplifier));
    AUDIO_TRY(gpsp_i2c_acquire(&s_bus));
    AUDIO_TRY(i2c_master_probe(s_bus, AUDIO_I2C_ADDRESS, 100));
    AUDIO_TRY(i2c_master_bus_add_device(s_bus, &device, &s_codec));
    dma.dma_desc_num = AUDIO_DMA_COUNT;
    dma.dma_frame_num = AUDIO_DMA_FRAMES;
    dma.auto_clear_after_cb = true;
    /* APLL gives MCLK = 256 x 32768 Hz. */
    wire.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    AUDIO_TRY(i2s_new_channel(&dma, &s_channel, NULL));
    AUDIO_TRY(i2s_channel_init_std_mode(s_channel, &wire));
    AUDIO_TRY(i2s_channel_register_event_callback(s_channel, &callbacks,
            NULL));
    AUDIO_TRY(i2s_channel_enable(s_channel));
    s_channel_enabled = true;
    AUDIO_TRY(codec_start());
    /* The DAC stays muted until the first queued sound plays. */
    AUDIO_TRY(gpio_set_level(AUDIO_PIN_AMPLIFIER, 1));

    atomic_store(&s_running, true);
    if (xTaskCreatePinnedToCore(audio_task, "gpsp_audio", 3072, NULL,
            tskIDLE_PRIORITY + 6, &s_task, AUDIO_TASK_CORE) != pdPASS)
    {
        atomic_store(&s_running, false);
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    if (i2s_channel_get_info(s_channel, &clocks) == ESP_OK)
        ESP_LOGI(TAG, "ES8311 %u Hz, MCLK %lu Hz, volume %d dB", AUDIO_RATE,
                (unsigned long)clocks.mclk_hz, CONFIG_GPSP_AUDIO_VOLUME_DB);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Audio setup failed: %s", esp_err_to_name(result));
    gpsp_audio_shutdown();
    return result;
}

#undef AUDIO_TRY

void gpsp_audio_shutdown(void)
{
    if (s_task)
    {
        atomic_store(&s_running, false);
        while (s_task)
            audio_delay(5);
    }
    gpio_set_level(AUDIO_PIN_AMPLIFIER, 0);
    if (s_codec)
    {
        codec_mute(true);
        codec_field(ES_DAC_POWER, 1u << 1, 1u << 1);
        codec_set(ES_RESET, 0x1f);
        codec_set(ES_CLOCK_GATE, 0);
        codec_set(ES_ANALOG_POWER, 0xfc);
    }
    if (s_channel)
    {
        if (s_channel_enabled)
            i2s_channel_disable(s_channel);
        i2s_del_channel(s_channel);
        s_channel = NULL;
        s_channel_enabled = false;
    }
    if (s_codec)
    {
        i2c_master_bus_rm_device(s_codec);
        s_codec = NULL;
    }
    if (s_bus)
    {
        gpsp_i2c_release();
        s_bus = NULL;
    }
    heap_caps_free(s_queue);
    s_queue = NULL;
}

#else

esp_err_t gpsp_audio_init(void)
{
    return ESP_OK;
}

size_t gpsp_audio_write(const int16_t *stereo, size_t frames)
{
    (void)stereo;
    (void)frames;
    return 0;
}

float gpsp_audio_balance(void)
{
    return 0.0f;
}

float gpsp_audio_fill(void)
{
    return 0.0f;
}

int gpsp_audio_volume_step(int steps)
{
    (void)steps;
    return 0;
}

void gpsp_audio_stats(unsigned *queued_ms, unsigned *underruns,
        unsigned *i2s_overflows)
{
    *queued_ms = 0;
    *underruns = 0;
    *i2s_overflows = 0;
}

void gpsp_audio_shutdown(void)
{
}

#endif

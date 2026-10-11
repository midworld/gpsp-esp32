/* P4 Nano DAC output using the ES8311 register specification, revision 10.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_test_output.h"
#include "audio_test_pcm.h"

#include <inttypes.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OUTPUT_I2C_ADDRESS 0x18
#define OUTPUT_DMA_COUNT 4
#define OUTPUT_DMA_FRAMES (AUDIO_TEST_RATE / 200)
#define OUTPUT_CLOCKS ((1u << 5) | (1u << 4) | (1u << 2) | (1u << 0))
#define OUTPUT_MUTE ((1u << 6) | (1u << 5))
#define OUTPUT_POWER ((1u << 5) | (1u << 4) | (1u << 2))

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

static const char *TAG = "audio_output";
static struct
{
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t channel;
    bool running;
} s_output;

static void output_delay(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t codec_transfer(uint8_t reg, uint8_t *value, bool read)
{
    unsigned attempt;
    esp_err_t result = ESP_ERR_INVALID_STATE;

    if (!s_output.codec)
        return result;
    for (attempt = 0; attempt < 3; attempt++)
    {
        if (read)
            result = i2c_master_transmit_receive(s_output.codec, &reg, 1,
                    value, 1, 100);
        else
        {
            uint8_t message[2] = {reg, *value};
            result = i2c_master_transmit(s_output.codec, message,
                    sizeof(message), 100);
        }
        if (result == ESP_OK)
            return result;
        output_delay(5);
    }
    ESP_LOGE(TAG, "Codec %s at 0x%02x failed: %s",
            read ? "read" : "write", reg, esp_err_to_name(result));
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
    ESP_LOGE(TAG, "Codec 0x%02x masked=0x%02x expected=0x%02x",
            reg, actual & mask, expected);
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
    current = (uint8_t)((current & ~mask) | (value & mask));
    result = codec_set(reg, current);
    if (result != ESP_OK)
        return result;
    return codec_expect(reg, mask, value & mask);
}

esp_err_t audio_test_output_volume(int db)
{
    /* DAC volume is -95.5 dB + register / 2 dB. */
    if (db < -60 || db > 0)
        return ESP_ERR_INVALID_ARG;
    return codec_field(ES_DAC_VOLUME, 0xff, (uint8_t)(2 * db + 191));
}

esp_err_t audio_test_output_mute(bool mute)
{
    return codec_field(ES_DAC_CONTROL, OUTPUT_MUTE, mute ? OUTPUT_MUTE : 0);
}

esp_err_t audio_test_output_amplifier(bool enable)
{
    if (enable && !s_output.running)
        return ESP_ERR_INVALID_STATE;
    return gpio_set_level(GPIO_NUM_53, enable);
}

#define CODEC_TRY(call) do { result = (call); if (result != ESP_OK) return result; } while (0)

static esp_err_t codec_start(void)
{
    esp_err_t result;

    CODEC_TRY(codec_expect(ES_ID_HIGH, 0xff, 0x83));
    CODEC_TRY(codec_expect(ES_ID_LOW, 0xff, 0x11));
    /* Reset registers as well as digital logic after an MCU-only reset. */
    CODEC_TRY(codec_set(ES_RESET, 0x1f));
    CODEC_TRY(codec_set(ES_REGISTER_RESET, 1));
    CODEC_TRY(codec_set(ES_REGISTER_RESET, 0));
    output_delay(5);
    CODEC_TRY(audio_test_output_mute(true));
    CODEC_TRY(audio_test_output_volume(-60));

    /* MCLK / Fs = 256; DAC clock = MCLK, DAC oversampling = 64 * Fs. */
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
    CODEC_TRY(codec_field(ES_CLOCK_GATE, 0x3f, OUTPUT_CLOCKS));
    CODEC_TRY(codec_field(ES_ANALOG_POWER, 0xff, OUTPUT_POWER | 3));
    CODEC_TRY(codec_field(ES_DAC_POWER, 0x03, 0));
    /* Slave port; ADC and unused master logic stay in reset. */
    CODEC_TRY(codec_set(ES_RESET, (1u << 7) | (1u << 2) | (1u << 1)));
    output_delay(100);
    CODEC_TRY(codec_field(ES_ANALOG_POWER, 0x03, 2));
    CODEC_TRY(codec_expect(ES_RESET, 0xff, 0x86));
    CODEC_TRY(codec_expect(ES_CLOCK_GATE, 0x3f, OUTPUT_CLOCKS));
    CODEC_TRY(codec_expect(ES_DAC_SERIAL, 0xff, 3u << 2));
    CODEC_TRY(codec_expect(ES_LRCK_LOW, 0xff, 255));
    ESP_LOGI(TAG, "AUDIO_CODEC id=0x8311 address=0x18 register_checks=PASS "
            "DAC=left implementation=datasheet");
    return ESP_OK;
}

#undef CODEC_TRY

#define CLOCK_TRY(call) do { result = (call); if (result != ESP_OK) goto done; } while (0)

static esp_err_t output_clock_check(const char *name, int gpio, unsigned expected)
{
    pcnt_unit_config_t counter = {
        .clk_src = PCNT_CLK_SRC_DEFAULT,
        .low_limit = -30000,
        .high_limit = 30000,
        .flags.accum_count = true,
    };
    pcnt_chan_config_t input = {
        .edge_gpio_num = gpio,
        .level_gpio_num = -1,
    };
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t channel = NULL;
    bool enabled = false;
    bool started = false;
    int count;
    int64_t begin;
    int64_t elapsed;
    double measured;
    long error_ppm;
    esp_err_t result;

    /* Read the output pad through GPIO input; no loopback wire is needed. */
    CLOCK_TRY(pcnt_new_unit(&counter, &unit));
    CLOCK_TRY(pcnt_new_channel(unit, &input, &channel));
    CLOCK_TRY(pcnt_channel_set_edge_action(channel,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    CLOCK_TRY(pcnt_unit_add_watch_point(unit, counter.high_limit));
    CLOCK_TRY(pcnt_unit_enable(unit));
    enabled = true;
    CLOCK_TRY(pcnt_unit_clear_count(unit));
    CLOCK_TRY(pcnt_unit_start(unit));
    started = true;
    begin = esp_timer_get_time();
    output_delay(250);
    elapsed = esp_timer_get_time() - begin;
    CLOCK_TRY(pcnt_unit_stop(unit));
    started = false;
    CLOCK_TRY(pcnt_unit_get_count(unit, &count));
    if (elapsed <= 0 || count < 0)
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    measured = count * 1000000.0 / elapsed;
    error_ppm = lround((measured / expected - 1.0) * 1000000.0);
    result = fabs(measured - expected) <= expected * 0.002 ?
            ESP_OK : ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG, "AUDIO_CLOCK_CHECK signal=%s gpio=%d expected_hz=%u "
            "measured_hz=%.1f error_ppm=%ld count=%d window_us=%" PRId64
            " tolerance_ppm=2000 status=%s", name, gpio, expected,
            measured, error_ppm, count, elapsed, result == ESP_OK ? "PASS" : "FAIL");

done:
    if (started)
        pcnt_unit_stop(unit);
    if (enabled)
        pcnt_unit_disable(unit);
    if (channel)
        pcnt_del_channel(channel);
    if (unit)
        pcnt_del_unit(unit);
    return result;
}

#undef CLOCK_TRY

void audio_test_output_stop(void)
{
    gpio_set_level(GPIO_NUM_53, 0);
    if (s_output.codec)
    {
        audio_test_output_mute(true);
        codec_field(ES_DAC_POWER, 1u << 1, 1u << 1);
        codec_set(ES_RESET, 0x1f);
        codec_set(ES_CLOCK_GATE, 0);
        codec_set(ES_ANALOG_POWER, 0xfc);
    }
    if (s_output.channel)
    {
        if (s_output.running)
            i2s_channel_disable(s_output.channel);
        i2s_del_channel(s_output.channel);
        s_output.channel = NULL;
    }
    s_output.running = false;
    if (s_output.codec)
    {
        i2c_master_bus_rm_device(s_output.codec);
        s_output.codec = NULL;
    }
    if (s_output.bus)
    {
        i2c_del_master_bus(s_output.bus);
        s_output.bus = NULL;
    }
}

#define OUTPUT_TRY(call) do { result = (call); if (result != ESP_OK) goto fail; } while (0)

esp_err_t audio_test_output_init(void)
{
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << GPIO_NUM_53,
        .mode = GPIO_MODE_OUTPUT,
    };
    i2c_master_bus_config_t control = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_7,
        .scl_io_num = GPIO_NUM_8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OUTPUT_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2s_chan_config_t dma = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_std_config_t wire = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_TEST_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13,
            .bclk = GPIO_NUM_12,
            .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9,
            .din = I2S_GPIO_UNUSED,
        },
    };
    static const int16_t zero[OUTPUT_DMA_COUNT * OUTPUT_DMA_FRAMES * 2];
    i2s_chan_info_t clocks;
    size_t count = 0;
    esp_err_t result;

    if (s_output.bus || s_output.channel)
        return ESP_ERR_INVALID_STATE;
    OUTPUT_TRY(gpio_set_level(GPIO_NUM_53, 0));
    OUTPUT_TRY(gpio_config(&pa));
    OUTPUT_TRY(i2c_new_master_bus(&control, &s_output.bus));
    OUTPUT_TRY(i2c_master_probe(s_output.bus, OUTPUT_I2C_ADDRESS, 100));
    OUTPUT_TRY(i2c_master_bus_add_device(s_output.bus, &device, &s_output.codec));
    dma.dma_desc_num = OUTPUT_DMA_COUNT;
    dma.dma_frame_num = OUTPUT_DMA_FRAMES;
    dma.auto_clear_after_cb = true;
    wire.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    OUTPUT_TRY(i2s_new_channel(&dma, &s_output.channel, NULL));
    OUTPUT_TRY(i2s_channel_init_std_mode(s_output.channel, &wire));
    OUTPUT_TRY(i2s_channel_preload_data(s_output.channel, zero, sizeof(zero), &count));
    if (count != sizeof(zero))
    {
        result = ESP_ERR_INVALID_SIZE;
        goto fail;
    }
    OUTPUT_TRY(i2s_channel_enable(s_output.channel));
    s_output.running = true;
    OUTPUT_TRY(codec_start());
    OUTPUT_TRY(i2s_channel_get_info(s_output.channel, &clocks));
    ESP_LOGI(TAG, "AUDIO_CLOCK requested_fs=%u driver_bclk=%lu driver_mclk=%lu "
            "dma_bytes=%lu dma_us=%lu APLL=1", AUDIO_TEST_RATE,
            (unsigned long)clocks.bclk_hz, (unsigned long)clocks.mclk_hz,
            (unsigned long)clocks.total_dma_buf_size,
            (unsigned long)((uint64_t)clocks.total_dma_buf_size * 1000000 /
                (AUDIO_TEST_RATE * sizeof(int16_t) * 2)));
    OUTPUT_TRY(output_clock_check("WS", GPIO_NUM_10, AUDIO_TEST_RATE));
    OUTPUT_TRY(output_clock_check("BCLK", GPIO_NUM_12, AUDIO_TEST_RATE * 32u));
    OUTPUT_TRY(output_clock_check("MCLK", GPIO_NUM_13, AUDIO_TEST_RATE * 256u));
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Output setup failed: %s", esp_err_to_name(result));
    audio_test_output_stop();
    return result;
}

#undef OUTPUT_TRY

esp_err_t audio_test_output_write(const int16_t *mono, size_t frames)
{
    int16_t slots[AUDIO_TEST_BLOCK_FRAMES * 2];
    size_t i;
    size_t remaining;
    const uint8_t *cursor = (const uint8_t *)slots;

    if (!s_output.running)
        return ESP_ERR_INVALID_STATE;
    if (frames > AUDIO_TEST_BLOCK_FRAMES)
        return ESP_ERR_INVALID_SIZE;
    if (frames && !mono)
        return ESP_ERR_INVALID_ARG;
    for (i = 0; i < frames; i++)
    {
        slots[2 * i] = mono[i];
        slots[2 * i + 1] = mono[i];
    }
    remaining = frames * sizeof(int16_t) * 2;
    while (remaining)
    {
        size_t sent = 0;
        esp_err_t result = i2s_channel_write(s_output.channel, cursor,
                remaining, &sent, 1000);
        if (result != ESP_OK)
            return result;
        if (!sent || sent > remaining)
            return ESP_ERR_INVALID_SIZE;
        cursor += sent;
        remaining -= sent;
    }
    return ESP_OK;
}

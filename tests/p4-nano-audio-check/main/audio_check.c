/* Sequential speaker tests for ESP32-P4-NANO-WIFI6-DB.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio_test_output.h"
#include "audio_test_pcm.h"

#define AUDIO_QUEUE_BLOCKS 8u
#define AUDIO_PREFILL_BLOCKS 6u
#define AUDIO_RECOVERY_BLOCKS 4u
#define AUDIO_STALL_MS 120u

enum audio_test_timing
{
    AUDIO_TIMING_CONTINUOUS,
    AUDIO_TIMING_GBA,
    AUDIO_TIMING_JITTER,
    AUDIO_TIMING_STALL,
};

struct audio_test_stage
{
    const char *name;
    const char *listen;
    uint32_t rate;
    unsigned seconds;
    enum audio_test_pattern pattern;
    float left_hz;
    float right_hz;
    int volume_offset;
    bool mute;
    bool amplifier_off;
    enum audio_test_timing timing;
};

static const struct audio_test_stage s_stages[] = {
    {.name = "silence", .listen = "silence; check hiss and hum",
        .rate = 48000, .seconds = 3, .pattern = AUDIO_PATTERN_SILENCE},
    {.name = "440_hz", .listen = "steady 440 Hz",
        .rate = 48000, .seconds = 3, .left_hz = 440, .right_hz = 440},
    {.name = "880_hz", .listen = "one octave above 440 Hz",
        .rate = 48000, .seconds = 3, .left_hz = 880, .right_hz = 880},
    {.name = "125_hz", .listen = "low tone; small speaker may be quiet",
        .rate = 48000, .seconds = 2, .left_hz = 125, .right_hz = 125},
    {.name = "1000_hz", .listen = "steady midrange tone",
        .rate = 48000, .seconds = 2, .left_hz = 1000, .right_hz = 1000},
    {.name = "4000_hz", .listen = "steady high tone",
        .rate = 48000, .seconds = 2, .left_hz = 4000, .right_hz = 4000},
    {.name = "sweep", .listen = "smooth rise from 120 Hz to 8000 Hz",
        .rate = 48000, .seconds = 6, .pattern = AUDIO_PATTERN_SWEEP},
    {.name = "amplitude_ramp", .listen = "quiet, louder, then quiet",
        .rate = 48000, .seconds = 6, .pattern = AUDIO_PATTERN_RAMP,
        .left_hz = 440, .right_hz = 440},
    {.name = "stereo_left", .listen = "440 Hz from LEFT input, mixed to mono",
        .rate = 48000, .seconds = 3, .left_hz = 440},
    {.name = "stereo_right", .listen = "880 Hz from RIGHT input, mixed to mono",
        .rate = 48000, .seconds = 3, .right_hz = 880},
    {.name = "stereo_both", .listen = "440 Hz and 880 Hz together",
        .rate = 48000, .seconds = 4, .left_hz = 440, .right_hz = 880},
    {.name = "stereo_antiphase", .listen = "silence from exact L=-R cancellation",
        .rate = 48000, .seconds = 3, .pattern = AUDIO_PATTERN_ANTIPHASE,
        .left_hz = 440, .right_hz = 440},
    {.name = "stereo_pan", .listen = "440 Hz fades out as 880 Hz fades in",
        .rate = 48000, .seconds = 6, .pattern = AUDIO_PATTERN_PAN,
        .left_hz = 440, .right_hz = 880},
    {.name = "volume_low", .listen = "quiet 440 Hz",
        .rate = 48000, .seconds = 2, .left_hz = 440, .right_hz = 440,
        .volume_offset = -18},
    {.name = "volume_medium", .listen = "louder 440 Hz",
        .rate = 48000, .seconds = 2, .left_hz = 440, .right_hz = 440,
        .volume_offset = -6},
    {.name = "volume_high", .listen = "louder again; check distortion",
        .rate = 48000, .seconds = 2, .left_hz = 440, .right_hz = 440,
        .volume_offset = 6},
    {.name = "dac_mute", .listen = "silence despite nonzero PCM",
        .rate = 48000, .seconds = 2, .left_hz = 440, .right_hz = 440,
        .mute = true},
    {.name = "amplifier_off", .listen = "silence with GPIO53 low",
        .rate = 48000, .seconds = 2, .left_hz = 440, .right_hz = 440,
        .amplifier_off = true},
    {.name = "44100_input", .listen = "same 440 Hz and 880 Hz pitch",
        .rate = 44100, .seconds = 4, .left_hz = 440, .right_hz = 880},
    {.name = "32768_input", .listen = "same pitch with the gpSP 32768 Hz option",
        .rate = 32768, .seconds = 5, .left_hz = 440, .right_hz = 880},
    {.name = "65536_input", .listen = "same pitch with 65536 Hz input",
        .rate = 65536, .seconds = 5, .left_hz = 440, .right_hz = 880},
    {.name = "stereo_melody", .listen = "two repeating voices mixed to mono",
        .rate = 32768, .seconds = 8, .pattern = AUDIO_PATTERN_MELODY},
    {.name = "continuous", .listen = "continuous 440 Hz without gaps",
        .rate = 32768, .seconds = CONFIG_AUDIO_TEST_LONG_SECONDS,
        .left_hz = 440, .right_hz = 440},
    {.name = "gba_frame_supply", .listen = "steady two-tone sound at GBA pacing",
        .rate = 32768, .seconds = 10, .left_hz = 440, .right_hz = 880,
        .timing = AUDIO_TIMING_GBA},
    {.name = "gba_frame_supply_65536", .listen = "same sound with larger core batches",
        .rate = 65536, .seconds = 10, .left_hz = 440, .right_hz = 880,
        .timing = AUDIO_TIMING_GBA},
    {.name = "gba_frame_jitter", .listen = "steady sound despite small delays",
        .rate = 32768, .seconds = 10, .left_hz = 440, .right_hz = 880,
        .timing = AUDIO_TIMING_JITTER},
#if CONFIG_AUDIO_TEST_INJECT_STALL
    {.name = "intentional_stall", .listen = "one short gap, then sound returns",
        .rate = 32768, .seconds = 8, .left_hz = 440, .right_hz = 880,
        .timing = AUDIO_TIMING_STALL},
#endif
    {.name = "recovery", .listen = "clean continuous sound after the stall",
        .rate = 32768, .seconds = 10, .left_hz = 440, .right_hz = 880,
        .timing = AUDIO_TIMING_GBA},
    {.name = "final_silence", .listen = "silence; amplifier turns off at end",
        .rate = 48000, .seconds = 2, .pattern = AUDIO_PATTERN_SILENCE},
};

struct audio_test_block
{
    /* Zero frames marks the end of a stage. Queue copies own the samples. */
    unsigned frames;
    int16_t samples[AUDIO_TEST_BLOCK_FRAMES];
};

struct audio_test_result
{
    uint32_t frames;
    unsigned underrun_blocks;
    unsigned recoveries;
    unsigned gap_count;
    struct
    {
        uint32_t frames;
        int64_t begin_us;
        int64_t end_us;
    } gaps[4];
    unsigned queue_min;
    unsigned queue_max;
    int peak;
    uint64_t square_sum;
    int64_t elapsed_us;
    int64_t write_max_us;
};

struct audio_test_producer
{
    struct audio_test_block block;
    unsigned filled;
    unsigned blocks;
    bool started;
};

static const char *TAG = "audio_check";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_start;
static SemaphoreHandle_t s_done;
static atomic_bool s_producer_done;
static struct audio_test_result s_result;
static int16_t s_generated[2048];

static void audio_test_require(esp_err_t err)
{
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "AUDIO_FATAL error=%s", esp_err_to_name(err));
        audio_test_output_stop();
        abort();
    }
}

static void audio_test_consumer(void *arg)
{
    struct audio_test_block block;
    const int16_t silence[AUDIO_TEST_BLOCK_FRAMES] = {0};

    (void)arg;
    for (;;)
    {
        struct audio_test_result result = {.queue_min = AUDIO_QUEUE_BLOCKS};
        int64_t started;
        bool gap = false;

        xSemaphoreTake(s_start, portMAX_DELAY);
        started = esp_timer_get_time();
        for (;;)
        {
            bool ready = !gap ||
                    atomic_load_explicit(&s_producer_done, memory_order_acquire) ||
                    uxQueueMessagesWaiting(s_queue) >= AUDIO_RECOVERY_BLOCKS;
            bool received;
            int64_t write_started;
            int64_t write_us;

            /* Refill during playback, but drain a completed producer's tail. */
            if (!ready && xQueuePeek(s_queue, &block, 0) == pdTRUE && !block.frames)
                ready = true;
            received = ready && xQueueReceive(s_queue, &block, 0) == pdTRUE;
            if (received && !block.frames)
                break;
            if (received)
            {
                unsigned i;
                unsigned queued = uxQueueMessagesWaiting(s_queue);
                if (queued < result.queue_min)
                    result.queue_min = queued;
                if (queued > result.queue_max)
                    result.queue_max = queued;
                if (gap)
                {
                    if (result.gap_count <= 4)
                        result.gaps[result.gap_count - 1].end_us = esp_timer_get_time() - started;
                    result.recoveries++;
                }
                gap = false;
                for (i = 0; i < block.frames; i++)
                {
                    int sample = block.samples[i];
                    int peak = sample < 0 ? -sample : sample;
                    if (peak > result.peak)
                        result.peak = peak;
                    result.square_sum += (uint64_t)((int64_t)sample * sample);
                }
                result.frames += block.frames;
            }
            else
            {
                if (!gap)
                {
                    if (result.gap_count < 4)
                    {
                        result.gaps[result.gap_count].frames = result.frames;
                        result.gaps[result.gap_count].begin_us = esp_timer_get_time() - started;
                    }
                    result.gap_count++;
                }
                result.underrun_blocks++;
                gap = true;
            }

            write_started = esp_timer_get_time();
            audio_test_require(audio_test_output_write(
                    received ? block.samples : silence, AUDIO_TEST_BLOCK_FRAMES));
            write_us = esp_timer_get_time() - write_started;
            if (write_us > result.write_max_us)
                result.write_max_us = write_us;
        }

        /* Three silence blocks drain the DMA before the next stage's settings. */
        audio_test_require(audio_test_output_write(silence, AUDIO_TEST_BLOCK_FRAMES));
        audio_test_require(audio_test_output_write(silence, AUDIO_TEST_BLOCK_FRAMES));
        audio_test_require(audio_test_output_write(silence, AUDIO_TEST_BLOCK_FRAMES));
        result.elapsed_us = esp_timer_get_time() - started;
        s_result = result;
        xSemaphoreGive(s_done);
    }
}

static void audio_test_enqueue(struct audio_test_producer *producer)
{
    producer->block.frames = producer->filled;
    if (xQueueSend(s_queue, &producer->block, pdMS_TO_TICKS(2000)) != pdTRUE)
        audio_test_require(ESP_ERR_TIMEOUT);
    producer->blocks++;
    producer->filled = 0;
    if (!producer->started && producer->blocks >= AUDIO_PREFILL_BLOCKS)
    {
        producer->started = true;
        xSemaphoreGive(s_start);
    }
}

static void audio_test_submit(struct audio_test_producer *producer,
        const int16_t *samples, unsigned frames)
{
    unsigned i;

    for (i = 0; i < frames; i++)
    {
        producer->block.samples[producer->filled++] = samples[i];
        if (producer->filled == AUDIO_TEST_BLOCK_FRAMES)
            audio_test_enqueue(producer);
    }
}

static void audio_test_wait_until(int64_t deadline)
{
    for (;;)
    {
        int64_t remaining = deadline - esp_timer_get_time();
        TickType_t ticks;
        if (remaining <= 0)
            return;
        ticks = pdMS_TO_TICKS(remaining / 1000);
        vTaskDelay(ticks ? ticks : 1);
    }
}

static bool audio_test_run(const struct audio_test_stage *stage, unsigned number)
{
    static const int jitter_us[] = {0, 3000, -2000, 4000, -1000, 1000, -3000, 0};
    struct audio_test_signal signal = {
        .pattern = stage->pattern,
        .rate = stage->rate,
        .total = stage->rate * stage->seconds,
        .amplitude = CONFIG_AUDIO_TEST_AMPLITUDE,
        .left_hz = stage->left_hz,
        .right_hz = stage->right_hz,
    };
    struct audio_test_resampler resampler;
    struct audio_test_producer producer = {0};
    struct audio_test_block end = {0};
    uint64_t frame_remainder = 0;
    unsigned paced_frames = 0;
    unsigned batch_min = UINT32_MAX;
    unsigned batch_max = 0;
    int64_t generation_max_us = 0;
    int64_t base = 0;
    int64_t stall_offset = 0;
    int volume = CONFIG_AUDIO_TEST_VOLUME_DB + stage->volume_offset;
    bool injected = false;
    bool pass;
    bool expects_zero = stage->pattern == AUDIO_PATTERN_SILENCE ||
            stage->pattern == AUDIO_PATTERN_ANTIPHASE;

    atomic_store_explicit(&s_producer_done, false, memory_order_release);
    if (volume > 0)
        volume = 0;
    ESP_LOGI(TAG, "AUDIO_STAGE number=%u name=%s source_hz=%" PRIu32
            " output_hz=%u seconds=%u volume_db=%d mute=%d pa=%d",
            number, stage->name, stage->rate, AUDIO_TEST_RATE, stage->seconds,
            volume, stage->mute, !stage->amplifier_off);
    ESP_LOGI(TAG, "LISTEN: %s", stage->listen);
    audio_test_require(audio_test_output_amplifier(false));
    audio_test_require(audio_test_output_mute(true));
    audio_test_require(audio_test_output_volume(volume));
    audio_test_require(audio_test_output_mute(stage->mute));
    audio_test_require(audio_test_output_amplifier(!stage->amplifier_off));
    audio_test_resampler_init(&resampler, stage->rate);

    while (signal.position <= signal.total)
    {
        unsigned frames = stage->timing == AUDIO_TIMING_CONTINUOUS ?
                stage->rate / 100 : audio_test_gba_frames(stage->rate, &frame_remainder);
        unsigned available = signal.total + 1 - signal.position;
        unsigned generated = 0;
        unsigned i;
        int64_t generation_started;
        int64_t generation_us;

        if (frames < batch_min)
            batch_min = frames;
        if (frames > batch_max)
            batch_max = frames;
        if (frames > available)
            frames = available;
        generation_started = esp_timer_get_time();
        for (i = 0; i < frames; i++)
        {
            int16_t left;
            int16_t right;
            int16_t converted[AUDIO_TEST_RESAMPLE_MAX];
            unsigned j;
            unsigned count;

            audio_test_signal_next(&signal, &left, &right);
            count = audio_test_resampler_push(&resampler, left, right, converted);
            for (j = 0; j < count; j++)
            {
                if (generated >= sizeof(s_generated) / sizeof(s_generated[0]))
                    audio_test_require(ESP_ERR_INVALID_SIZE);
                s_generated[generated++] = converted[j];
            }
        }
        generation_us = esp_timer_get_time() - generation_started;
        if (generation_us > generation_max_us)
            generation_max_us = generation_us;
        audio_test_submit(&producer, s_generated, generated);

        if (producer.started && stage->timing != AUDIO_TIMING_CONTINUOUS &&
            signal.position <= signal.total)
        {
            int64_t deadline;
            if (!base)
                base = esp_timer_get_time();
            paced_frames++;
            if (stage->timing == AUDIO_TIMING_STALL && !injected && paced_frames == 120)
            {
                ESP_LOGW(TAG, "AUDIO_INJECT producer_stall_ms=%u expected_gap=1",
                        AUDIO_STALL_MS);
                stall_offset += AUDIO_STALL_MS * 1000;
                injected = true;
            }
            deadline = base + (int64_t)paced_frames * AUDIO_TEST_GBA_CYCLES *
                    1000000 / AUDIO_TEST_GBA_CLOCK + stall_offset;
            if (stage->timing == AUDIO_TIMING_JITTER)
                deadline += jitter_us[paced_frames % 8];
            audio_test_wait_until(deadline);
        }
    }

    if (producer.filled)
    {
        memset(producer.block.samples + producer.filled, 0,
                (AUDIO_TEST_BLOCK_FRAMES - producer.filled) * sizeof(int16_t));
        audio_test_enqueue(&producer);
    }
    if (!producer.started)
        xSemaphoreGive(s_start);
    if (xQueueSend(s_queue, &end, pdMS_TO_TICKS(2000)) != pdTRUE)
        audio_test_require(ESP_ERR_TIMEOUT);
    atomic_store_explicit(&s_producer_done, true, memory_order_release);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE)
        audio_test_require(ESP_ERR_TIMEOUT);

    pass = s_result.frames == AUDIO_TEST_RATE * stage->seconds &&
            (expects_zero ? s_result.peak == 0 : s_result.peak > 0);
    if (stage->timing == AUDIO_TIMING_STALL)
    {
        /* One bounded gap must recover; additional gaps are failures. */
        pass = pass && injected && s_result.gap_count == 1 &&
                s_result.underrun_blocks > 0 &&
                (uint64_t)s_result.underrun_blocks * AUDIO_TEST_BLOCK_FRAMES * 1000 <=
                (uint64_t)AUDIO_STALL_MS * AUDIO_TEST_RATE +
                AUDIO_TEST_BLOCK_FRAMES * 1000u &&
                s_result.recoveries == 1;
    }
    else
        pass = pass && s_result.underrun_blocks == 0;

    for (unsigned i = 0; i < s_result.gap_count && i < 4; i++)
        ESP_LOGI(TAG, "AUDIO_GAP stage=%u gap=%u frames=%" PRIu32
                " begin_us=%" PRId64 " end_us=%" PRId64, number, i + 1,
                s_result.gaps[i].frames, s_result.gaps[i].begin_us, s_result.gaps[i].end_us);
    ESP_LOGI(TAG, "AUDIO_RESULT number=%u name=%s status=%s frames=%" PRIu32
            " underrun_blocks=%u missing_ms=%u recoveries=%u gaps=%u queue_min=%u queue_max=%u"
            " peak=%d rms=%.1f elapsed_ms=%" PRId64 " write_max_us=%" PRId64
            " generate_max_us=%" PRId64 " source_batch_min=%u source_batch_max=%u"
            " expected_gap=%d listening_required=1",
            number, stage->name, pass ? "PASS" : "FAIL", s_result.frames,
            s_result.underrun_blocks, (unsigned)((uint64_t)s_result.underrun_blocks *
                    AUDIO_TEST_BLOCK_FRAMES * 1000 / AUDIO_TEST_RATE),
            s_result.recoveries, s_result.gap_count, s_result.queue_min, s_result.queue_max,
            s_result.peak, s_result.frames ? sqrtf(
                    (float)s_result.square_sum / s_result.frames) : 0.0f,
            s_result.elapsed_us / 1000, s_result.write_max_us, generation_max_us,
            batch_min, batch_max, stage->timing == AUDIO_TIMING_STALL);
    return pass;
}

void app_main(void)
{
    unsigned count = sizeof(s_stages) / sizeof(s_stages[0]);

    ESP_LOGI(TAG, "P4 Nano speaker check: ES8311 + NS4150B, mono speaker");
    ESP_LOGI(TAG, "Stereo PCM is mixed as (L+R)/2; no spatial stereo output");
    ESP_LOGI(TAG, "Output=%u Hz queue_us=%lu prefill_us=%lu; no ROM needed",
            AUDIO_TEST_RATE, (unsigned long)((uint64_t)AUDIO_QUEUE_BLOCKS *
                    AUDIO_TEST_BLOCK_FRAMES * 1000000 / AUDIO_TEST_RATE),
            (unsigned long)((uint64_t)AUDIO_PREFILL_BLOCKS *
                    AUDIO_TEST_BLOCK_FRAMES * 1000000 / AUDIO_TEST_RATE));
    if (!audio_test_pcm_self_check())
        audio_test_require(ESP_FAIL);
    ESP_LOGI(TAG, "AUDIO_PCM_CHECK downmix=PASS resample_rates=PASS gba_batches=PASS");
    audio_test_require(audio_test_output_init());

    s_queue = xQueueCreate(AUDIO_QUEUE_BLOCKS, sizeof(struct audio_test_block));
    s_start = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (!s_queue || !s_start || !s_done)
        audio_test_require(ESP_ERR_NO_MEM);
    if (xTaskCreate(audio_test_consumer, "audio_tx", 8192, NULL, 5, NULL) != pdPASS)
        audio_test_require(ESP_ERR_NO_MEM);

    for (;;)
    {
        unsigned i;
        unsigned passed = 0;
        unsigned selected = 0;
        for (i = 0; i < count; i++)
        {
#if CONFIG_AUDIO_TEST_RECOVERY_ONLY
            if (s_stages[i].timing != AUDIO_TIMING_STALL &&
                strcmp(s_stages[i].name, "recovery") != 0)
                continue;
#endif
            selected++;
            passed += audio_test_run(&s_stages[i], i + 1) ? 1 : 0;
        }
        audio_test_require(audio_test_output_amplifier(false));
        audio_test_require(audio_test_output_mute(true));
        ESP_LOGI(TAG, "AUDIO_SUITE_RESULT stages=%u transport_pass=%u transport_fail=%u"
                " listening_required=1", selected, passed, selected - passed);
#if !CONFIG_AUDIO_TEST_REPEAT
        break;
#endif
    }

    audio_test_output_stop();
    ESP_LOGI(TAG, "Audio tests finished; speaker muted. Reset to run again.");
}

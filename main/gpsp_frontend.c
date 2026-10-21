/* Minimal libretro frontend for gpSP.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_frontend.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_profile.h"
#include "gpsp_save.h"
#include "gpsp_state.h"
#include "gpsp_frame_stats.h"
#include "gpsp_screenshot.h"
#include "gpsp_settings.h"
#include "gpsp_ui.h"
#if CONFIG_GPSP_MENU
#include "gpsp_menu.h"
#endif
#include "gpsp_storage.h"
#include "gpsp_screen.h"
#include "libretro.h"
#if CONFIG_GPSP_BENCHMARK
#include "gpsp_benchmark.h"
#endif

#define GPSP_PERFORMANCE_REPORT_INTERVAL_US 2000000

static const char *TAG = "gpsp_frontend";
static const char *CORE_TAG = "gpsp_core";
static uint16_t s_input_state;
#if CONFIG_GPSP_MENU
static uint32_t s_input_prev;
static bool s_menu_pending;
/* The game sees no buttons until all are up: set when the menu is asked
   for and when it closes. */
static bool s_input_blocked;
#endif
static uint32_t s_frame_count;
#if CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
static uint32_t s_report_frame_count;
/* Core frames, drawn or skipped, at the report start. */
static uint32_t s_report_run_count;
static int64_t s_report_start_us;
#endif

#if CONFIG_GPSP_BENCHMARK
#define GPSP_BENCHMARK_FRAME_COUNT CONFIG_GPSP_BENCHMARK_FRAMES
#define GPSP_BENCHMARK_SLICE_FRAMES 64
#define GPSP_BENCHMARK_SLICE_COUNT \
        ((GPSP_BENCHMARK_FRAME_COUNT + \
        GPSP_BENCHMARK_SLICE_FRAMES - 1) / GPSP_BENCHMARK_SLICE_FRAMES)
#define GPSP_BENCHMARK_IMAGE_WIDTH 30
#define GPSP_BENCHMARK_IMAGE_HEIGHT 20
#define GPSP_BENCHMARK_IMAGE_PIXELS \
        (GPSP_BENCHMARK_IMAGE_WIDTH * GPSP_BENCHMARK_IMAGE_HEIGHT)
struct gpsp_benchmark_window
{
    uint64_t run_us;
    uint64_t execute_us;
    uint64_t audio_us;
    uint64_t video_us;
    uint64_t wait_us;
    uint32_t run_max_us;
    uint32_t frames;
};

struct gpsp_benchmark_slice
{
    struct gpsp_benchmark_window frame;
    uint64_t cycles[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
#if CONFIG_GPSP_BENCHMARK_INSTRET
    uint64_t instret[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
#endif
#if CONFIG_GPSP_BENCHMARK_PHASE_WORK
    uint64_t enters[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
    uint64_t dma_bytes;
    uint64_t direct_calls;
    uint64_t direct_samples;
    uint64_t direct_active_calls;
    uint64_t direct_active_samples;
    uint64_t psg_calls;
    uint64_t psg_samples;
#endif
#if CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL
    struct gpsp_benchmark_video_detail video;
#endif
    uint32_t checksum;
    uint32_t video_frames;
    uint32_t submitted;
    uint32_t sent;
    uint32_t dropped;
    bool image_ok;
};
static struct gpsp_benchmark_slice s_benchmark_slices[
        GPSP_BENCHMARK_SLICE_COUNT];
static uint16_t *s_benchmark_images;
static struct gpsp_benchmark_engine *s_benchmark_engines;
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
static struct gpsp_benchmark_bios_probe *s_benchmark_bios_probes;
#endif
#if !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT
static struct gpsp_benchmark_slice s_benchmark_live_slice;
static struct gpsp_benchmark_engine s_benchmark_live_engines[2];
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
static struct gpsp_benchmark_bios_probe s_benchmark_live_bios_probes[2];
#endif
#endif

static const uint8_t *s_benchmark_pixels;
static unsigned s_benchmark_width;
static unsigned s_benchmark_height;
static size_t s_benchmark_pitch;
static struct gpsp_board_output_stats s_benchmark_output_prev;
static uint32_t s_benchmark_video_prev;

#endif

#if CONFIG_GPSP_AUDIO
/* Largest frame pace change for the speaker queue (d in Arntzen's
   "Dynamic Rate Control for Retro Game Emulators"). */
#define GPSP_RATE_CONTROL 0.005
/* About 200 ms: covers the late frames of a scene change. */
#define GPSP_CATCH_UP_FRAMES 12
/* Queue surplus (against its target) that ends a catch-up: about one
   frame of sound over a 100 ms target. */
#define GPSP_CATCH_UP_SURPLUS 0.2f
#else
#define GPSP_CATCH_UP_FRAMES 1
#endif

#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
static void gpsp_frame_timer_callback(void *arg)
{
    xTaskNotifyGive((TaskHandle_t)arg);
}
#endif

/* Benchmarks keep every frame. */
#define GPSP_FRAMESKIP (!CONFIG_GPSP_BENCHMARK)

#if GPSP_FRAMESKIP
/* One digit: settings keep the interval within 1 to 9. */
static char s_frameskip_interval[2] = "0";

/* Crossfade at each sound cut: about 2 ms. */
#define GPSP_FAST_FORWARD_FADE 64

static bool s_fast_forward;
/* The fast forward button, as last read. */
static bool s_fast_forward_held;
/* Core frames per paced frame; above 1 while fast forward runs. */
static unsigned s_fast_forward_runs = 1;
static bool s_fast_forward_update;
/* Whether the sound of the running core frame is played, and of the one
   before it. */
static bool s_fast_forward_keep;
static bool s_fast_forward_kept;
/* The start of the sound after the kept part, faded out at the cut. */
static int16_t s_fast_forward_tail[GPSP_FAST_FORWARD_FADE * 2];
static size_t s_fast_forward_tail_frames;

static const char *gpsp_frameskip_mode(void)
{
    int frameskip = gpsp_settings_frameskip();

    /* Draw one frame per paced iteration during fast forward. */
    if (s_fast_forward_runs > 1)
    {
        s_frameskip_interval[0] = (char)('0' + s_fast_forward_runs - 1);
        return "fixed_interval";
    }
    if (frameskip == GPSP_SETTINGS_FRAMESKIP_AUTO)
        return "auto";
    s_frameskip_interval[0] = (char)('0' + (frameskip > 0 ? frameskip : 0));
    return frameskip > 0 ? "fixed_interval" : "disabled";
}
#endif

static const char *gpsp_option_value(const char *key)
{
    if (strcmp(key, "gpsp_bios") == 0)
        return "builtin";
    if (strcmp(key, "gpsp_boot_mode") == 0)
        return "game";
    if (strcmp(key, "gpsp_frameskip") == 0)
#if GPSP_FRAMESKIP
        return gpsp_frameskip_mode();
    if (strcmp(key, "gpsp_frameskip_interval") == 0)
        return s_frameskip_interval;
#else
        return "disabled";
#endif
    if (strcmp(key, "gpsp_rtc_time_source") == 0)
        return "deterministic";
#if CONFIG_GPSP_AUDIO
    /* The speaker's I2S clock runs at this rate: no resampling. */
    if (strcmp(key, "gpsp_sound_rate") == 0)
        return "32768";
    /* Loud mixes clip at full scale; the DAC volume makes up the 6 dB. */
    if (strcmp(key, "gpsp_sound_headroom") == 0)
        return "6";
#endif

    return NULL;
}

static void gpsp_core_log(enum retro_log_level level, const char *format, ...)
{
    esp_log_level_t esp_level;
    va_list args;

    switch (level)
    {
        case RETRO_LOG_DEBUG:
            esp_level = ESP_LOG_DEBUG;
            break;
        case RETRO_LOG_WARN:
            esp_level = ESP_LOG_WARN;
            break;
        case RETRO_LOG_ERROR:
            esp_level = ESP_LOG_ERROR;
            break;
        case RETRO_LOG_INFO:
        default:
            esp_level = ESP_LOG_INFO;
            break;
    }

    va_start(args, format);
    esp_log_writev(esp_level, CORE_TAG, format, args);
    va_end(args);
}

#if CONFIG_GPSP_BENCHMARK
static retro_time_t gpsp_perf_get_time_usec(void)
{
    return (retro_time_t)esp_timer_get_time();
}

static retro_perf_tick_t gpsp_perf_get_counter(void)
{
    return (retro_perf_tick_t)esp_cpu_get_cycle_count();
}

#if CONFIG_GPSP_BENCHMARK_INSTRET
static uint32_t gpsp_instret_counter(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, minstret" : "=r"(value));
    return value;
}
#endif
#endif

#if GPSP_FRAMESKIP
/* The core's frameskip reads the speaker queue through this callback. */
static retro_audio_buffer_status_callback_t s_audio_status_cb;
static bool s_audio_live;

/* Speaker queue state for the core: occupancy is 50% on the queue target
   and 100% from twice the target, and an underrun is likely below the
   frameskip threshold. The automatic mode reads only the underrun flag. */
static void gpsp_report_audio_status(void)
{
    if (!s_audio_status_cb)
        return;
    float fill = gpsp_board_audio_fill();
    unsigned occupancy = fill >= 2.0f ? 100 : (unsigned)(fill * 50.0f);
    s_audio_status_cb(s_audio_live, occupancy,
            s_audio_live &&
            fill * 100.0f < (float)gpsp_settings_frameskip_threshold());
}
#endif

static bool gpsp_environment(unsigned command, void *data)
{
    switch (command)
    {
#if GPSP_FRAMESKIP
        case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
        {
            const struct retro_audio_buffer_status_callback *callback = data;
            s_audio_status_cb = callback ? callback->callback : NULL;
            return true;
        }
#endif
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned *)data = 1;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = gpsp_core_log;
            return true;
#if CONFIG_GPSP_BENCHMARK
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
            /* Keep the core's defaults for unused performance callbacks. */
            ((struct retro_perf_callback *)data)->get_time_usec =
                    gpsp_perf_get_time_usec;
            ((struct retro_perf_callback *)data)->get_perf_counter =
                    gpsp_perf_get_counter;
            return true;
#endif
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = "/storage";
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            struct retro_variable *variable = data;
            variable->value = gpsp_option_value(variable->key);
            return variable->value != NULL;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
#if GPSP_FRAMESKIP
            /* The menu or fast forward changed the frameskip. */
            *(bool *)data = gpsp_settings_take_core_update() ||
                    s_fast_forward_update;
            s_fast_forward_update = false;
#else
            *(bool *)data = false;
#endif
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *(const enum retro_pixel_format *)data ==
                    RETRO_PIXEL_FORMAT_RGB565;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
            return true;
        default:
            return false;
    }
}

#if CONFIG_GPSP_BENCHMARK || CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
static uint32_t gpsp_frame_checksum(
        const uint8_t *pixels, unsigned width, unsigned height, size_t pitch)
{
    uint32_t checksum = 2166136261u;
    unsigned y;

    for (y = 0; y < height; y++)
    {
        const uint8_t *row = pixels + y * pitch;
        size_t row_size = width * sizeof(uint16_t);
        size_t x;

        for (x = 0; x < row_size; x++)
        {
            checksum ^= row[x];
            checksum *= 16777619u;
        }
    }

    return checksum;
}
#endif

#if CONFIG_GPSP_BENCHMARK
static void gpsp_benchmark_add(struct gpsp_benchmark_window *window,
        uint32_t run_us, uint32_t wait_us,
        const struct gpsp_benchmark_frame *core)
{
    window->frames++;
    window->run_us += run_us;
    window->execute_us += core->execute_us;
    window->audio_us += core->audio_us;
    window->video_us += core->video_us;
    window->wait_us += wait_us;
    if (run_us > window->run_max_us)
        window->run_max_us = run_us;
}

static bool gpsp_benchmark_image_take(unsigned index)
{
    if (!s_benchmark_pixels || s_benchmark_width != 240 ||
            s_benchmark_height != 160)
        return false;

    uint16_t *output = s_benchmark_images +
            index * GPSP_BENCHMARK_IMAGE_PIXELS;

    for (unsigned y = 0; y < GPSP_BENCHMARK_IMAGE_HEIGHT; y++)
        for (unsigned x = 0; x < GPSP_BENCHMARK_IMAGE_WIDTH; x++)
        {
            unsigned red = 0, green = 0, blue = 0;

            for (unsigned dy = 0; dy < 8; dy++)
            {
                const uint16_t *row = (const uint16_t *)(s_benchmark_pixels +
                        (y * 8 + dy) * s_benchmark_pitch);
                for (unsigned dx = 0; dx < 8; dx++)
                {
                    uint16_t pixel = row[x * 8 + dx];
                    red += (pixel >> 11) & 31;
                    green += (pixel >> 5) & 63;
                    blue += pixel & 31;
                }
            }

            output[y * GPSP_BENCHMARK_IMAGE_WIDTH + x] =
                    (((red + 32) / 64) << 11) |
                    (((green + 32) / 64) << 5) |
                    ((blue + 32) / 64);
        }

    return true;
}

static void gpsp_benchmark_report_images(void)
{
    static const char hex[] = "0123456789abcdef";

    /* Serial output follows timing; yield so the idle task can run. */
    for (unsigned i = 0; i < GPSP_BENCHMARK_SLICE_COUNT; i++)
    {
        unsigned first = i * GPSP_BENCHMARK_SLICE_FRAMES + 1;
        unsigned last = first + GPSP_BENCHMARK_SLICE_FRAMES - 1;

        if (last > GPSP_BENCHMARK_FRAME_COUNT)
            last = GPSP_BENCHMARK_FRAME_COUNT;
        if (!s_benchmark_slices[i].image_ok)
        {
            ESP_LOGE(TAG, "BENCH_IMAGE_MISSING first=%u last=%u", first, last);
            continue;
        }

        const uint16_t *image = s_benchmark_images +
                i * GPSP_BENCHMARK_IMAGE_PIXELS;
        for (unsigned y = 0; y < GPSP_BENCHMARK_IMAGE_HEIGHT; y++)
        {
            char row[GPSP_BENCHMARK_IMAGE_WIDTH * 4 + 1];

            for (unsigned x = 0; x < GPSP_BENCHMARK_IMAGE_WIDTH; x++)
            {
                uint16_t pixel = image[y * GPSP_BENCHMARK_IMAGE_WIDTH + x];
                row[x * 4] = hex[(pixel >> 12) & 15];
                row[x * 4 + 1] = hex[(pixel >> 8) & 15];
                row[x * 4 + 2] = hex[(pixel >> 4) & 15];
                row[x * 4 + 3] = hex[pixel & 15];
            }
            row[GPSP_BENCHMARK_IMAGE_WIDTH * 4] = '\0';
            ESP_LOGI(TAG, "BENCH_IMAGE version=1 first=%u last=%u row=%u rgb565=%s",
                    first, last, y, row);
            if ((y & 7) == 7)
                vTaskDelay(1);
        }
    }
}

static void gpsp_benchmark_slice_add(struct gpsp_benchmark_slice *slice,
        uint32_t run_us, uint32_t wait_us,
        const struct gpsp_benchmark_frame *core)
{
    gpsp_benchmark_add(&slice->frame, run_us, wait_us, core);
    for (unsigned i = 0; i < GPSP_BENCHMARK_TIMING_BUCKET_COUNT; i++)
    {
        slice->cycles[i] += core->cpu.cycles[i];
#if CONFIG_GPSP_BENCHMARK_INSTRET
        slice->instret[i] += core->cpu.instret[i];
#endif
#if CONFIG_GPSP_BENCHMARK_PHASE_WORK
        slice->enters[i] += core->cpu.enters[i];
#endif
    }
#if CONFIG_GPSP_BENCHMARK_PHASE_WORK
    slice->dma_bytes += core->cpu.dma_bytes;
    slice->direct_calls += core->cpu.direct_calls;
    slice->direct_samples += core->cpu.direct_samples;
    slice->direct_active_calls += core->cpu.direct_active_calls;
    slice->direct_active_samples += core->cpu.direct_active_samples;
    slice->psg_calls += core->cpu.psg_calls;
    slice->psg_samples += core->cpu.psg_samples;
#endif
#if CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL
    struct gpsp_benchmark_video_detail *video = &slice->video;
    const struct gpsp_benchmark_video_detail *frame = &core->video_detail;
    video->oam_cycles += frame->oam_cycles;
    video->layers_cycles += frame->layers_cycles;
    video->draw_cycles += frame->draw_cycles;
    video->affine_cycles += frame->affine_cycles;
    video->text_bg_cycles += frame->text_bg_cycles;
    video->affine_bg_cycles += frame->affine_bg_cycles;
    video->bitmap_bg_cycles += frame->bitmap_bg_cycles;
    video->obj_layer_cycles += frame->obj_layer_cycles;
    video->effects_cycles += frame->effects_cycles;
    video->oam_updates += frame->oam_updates;
    video->forced_blank_lines += frame->forced_blank_lines;
    video->window_lines += frame->window_lines;
    for (unsigned i = 0; i < 6; i++)
        video->mode_lines[i] += frame->mode_lines[i];
#endif
}

static void gpsp_benchmark_capture_output(struct gpsp_benchmark_slice *slice)
{
    struct gpsp_board_output_stats output;

    gpsp_board_output_stats(&output);
    slice->video_frames = s_frame_count - s_benchmark_video_prev;
    slice->submitted = output.submitted - s_benchmark_output_prev.submitted;
    slice->sent = output.sent - s_benchmark_output_prev.sent;
    slice->dropped = output.dropped - s_benchmark_output_prev.dropped;
    s_benchmark_video_prev = s_frame_count;
    s_benchmark_output_prev = output;
}

static void gpsp_benchmark_slice_capture(unsigned index)
{
    struct gpsp_benchmark_slice *slice = &s_benchmark_slices[index];

    gpsp_benchmark_get_engine(&s_benchmark_engines[index + 1]);
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
    gpsp_benchmark_take_hot_sites(
            &s_benchmark_engines[index + 1].jit_hot);
    gpsp_benchmark_get_bios_probe(&s_benchmark_bios_probes[index + 1]);
#endif
    if (s_benchmark_pixels)
        slice->checksum = gpsp_frame_checksum(s_benchmark_pixels,
                s_benchmark_width, s_benchmark_height, s_benchmark_pitch);
    slice->image_ok = gpsp_benchmark_image_take(index);
    gpsp_benchmark_capture_output(slice);
}

static void gpsp_benchmark_report_jit(unsigned first, unsigned last,
        const struct gpsp_benchmark_engine *start,
        const struct gpsp_benchmark_engine *end
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
        , const struct gpsp_benchmark_bios_probe *bios_start,
        const struct gpsp_benchmark_bios_probe *bios_end
#endif
        )
{
#define JIT_DELTA(field) (unsigned long long)(end->field - start->field)
    ESP_LOGI(TAG, "BENCH_ENGINE version=2 first=%u last=%u "
            "blocks=%llu scalar_steps=%llu scalar_runs=%llu ready=%s",
            first, last, JIT_DELTA(blocks), JIT_DELTA(scalar_steps),
            JIT_DELTA(scalar_runs), end->ready ? "true" : "false");
    ESP_LOGI(TAG, "BENCH_JIT_PHASE version=2 first=%u last=%u "
            "total_cycles=%llu lookup_cycles=%llu block_cycles=%llu "
            "scalar_cycles=%llu translated_blocks=%llu "
            "total_scope=engine_with_update lookup_scope=includes_translation",
            first, last, JIT_DELTA(total_cycles), JIT_DELTA(lookup_cycles),
            JIT_DELTA(block_cycles), JIT_DELTA(scalar_cycles),
            JIT_DELTA(translated_blocks));
    ESP_LOGI(TAG, "BENCH_JIT_TRANSLATE version=2 first=%u last=%u "
            "blocks=%llu guest_ops=%llu code_bytes=%llu "
            "flow_blocks=%llu flow_arm_blocks=%llu "
            "flow_thumb_blocks=%llu flow_ram_blocks=%llu "
            "flow_legacy_blocks=%llu flow_guest_ops=%llu "
            "flow_helper_ops=%llu flow_regions=%llu flow_entries=%llu "
            "flow_code_bytes=%llu "
            "bundled_guest_ops=%llu flushed_bytes=%llu "
            "rom_code_bytes=%u ram_code_bytes=%u "
            "max_guest_ops=%u max_thumb_ops=%u max_block_bytes=%u "
            "max_flush_bytes=%u",
            first, last, JIT_DELTA(translated_blocks),
            JIT_DELTA(translated_guest_ops), JIT_DELTA(translated_code_bytes),
            JIT_DELTA(flow_blocks), JIT_DELTA(flow_arm_blocks),
            JIT_DELTA(flow_thumb_blocks), JIT_DELTA(flow_ram_blocks),
            JIT_DELTA(flow_legacy_blocks), JIT_DELTA(flow_guest_ops),
            JIT_DELTA(flow_helper_ops), JIT_DELTA(flow_regions),
            JIT_DELTA(flow_entries),
            JIT_DELTA(flow_code_bytes),
            JIT_DELTA(bundled_guest_ops), JIT_DELTA(flushed_code_bytes),
            end->code_bytes, end->ram_code_bytes,
            end->max_guest_ops, end->max_thumb_guest_ops,
            end->max_block_bytes, end->max_flush_bytes);
    ESP_LOGI(TAG, "BENCH_JIT_CACHE version=3 first=%u last=%u "
            "flushes=%u ram_flushes=%u "
            "rom_used_start_bytes=%u rom_used_bytes=%u "
            "rom_capacity_bytes=%u rom_free_bytes=%u "
            "ram_used_start_bytes=%u ram_used_bytes=%u "
            "ram_capacity_bytes=%u ram_free_bytes=%u "
            "rom_flushes_total=%u ram_flushes_total=%u "
            "rom_discarded_bytes=%llu ram_discarded_bytes=%llu "
            "chunk_retries=%u chunk_tail_bytes=%llu",
            first, last, end->cache_flushes - start->cache_flushes,
            end->ram_flushes - start->ram_flushes,
            start->code_bytes, end->code_bytes,
            end->rom_code_capacity_bytes,
            end->rom_code_capacity_bytes > end->code_bytes ?
                    end->rom_code_capacity_bytes - end->code_bytes : 0,
            start->ram_code_bytes, end->ram_code_bytes,
            end->ram_code_capacity_bytes,
            end->ram_code_capacity_bytes > end->ram_code_bytes ?
                    end->ram_code_capacity_bytes - end->ram_code_bytes : 0,
            end->cache_flushes, end->ram_flushes,
            JIT_DELTA(rom_discarded_bytes),
            JIT_DELTA(ram_discarded_bytes),
            end->chunk_retries - start->chunk_retries,
            JIT_DELTA(chunk_tail_bytes));
    ESP_LOGI(TAG, "BENCH_JIT_SPANS version=2 first=%u last=%u "
            "n3_4=%u n5_8=%u n9_16=%u n17_32=%u guest_ops=%llu",
            first, last,
            end->thumb_span_count[0] - start->thumb_span_count[0],
            end->thumb_span_count[1] - start->thumb_span_count[1],
            end->thumb_span_count[2] - start->thumb_span_count[2],
            end->thumb_span_count[3] - start->thumb_span_count[3],
            JIT_DELTA(bundled_guest_ops));
    ESP_LOGI(TAG, "BENCH_JIT_SITES version=2 first=%u last=%u "
            "arm_ops=%llu thumb_ops=%llu thumb_loads=%llu "
            "thumb_stores=%llu thumb_literals=%llu "
            "thumb_nonmem_fallbacks=%llu thumb_push=%llu thumb_pop=%llu "
            "thumb_stmia=%llu thumb_ldmia=%llu",
            first, last, JIT_DELTA(jit_sites.arm_ops),
            JIT_DELTA(jit_sites.thumb_ops), JIT_DELTA(jit_sites.thumb_loads),
            JIT_DELTA(jit_sites.thumb_stores),
            JIT_DELTA(jit_sites.thumb_literals),
            JIT_DELTA(jit_sites.thumb_nonmem_fallbacks),
            JIT_DELTA(jit_sites.thumb_push), JIT_DELTA(jit_sites.thumb_pop),
            JIT_DELTA(jit_sites.thumb_stmia), JIT_DELTA(jit_sites.thumb_ldmia));
#if CONFIG_GPSP_BENCHMARK_JIT_LOOKUP
    static const char *regions[] = {"bios", "rom", "ram"};
    const struct gpsp_benchmark_jit_lookup_region *before[] = {
        &start->jit_lookup.bios, &start->jit_lookup.rom,
        &start->jit_lookup.ram};
    const struct gpsp_benchmark_jit_lookup_region *after[] = {
        &end->jit_lookup.bios, &end->jit_lookup.rom,
        &end->jit_lookup.ram};

    ESP_LOGI(TAG, "BENCH_JIT_LOOKUP_TOP version=2 first=%u last=%u "
            "hit_calls=%llu miss_calls=%llu hit_cycles=%llu miss_cycles=%llu "
            "side_create_calls=%llu side_create_failures=%llu "
            "side_create_cycles=%llu "
            "rom_flushes=%u ram_flushes=%u",
            first, last,
            JIT_DELTA(jit_lookup.top_hit_calls),
            JIT_DELTA(jit_lookup.top_miss_calls),
            JIT_DELTA(jit_lookup.top_hit_cycles),
            JIT_DELTA(jit_lookup.top_miss_cycles),
            JIT_DELTA(jit_lookup.side_create_calls),
            JIT_DELTA(jit_lookup.side_create_failures),
            JIT_DELTA(jit_lookup.side_create_cycles),
            (unsigned)(end->jit_lookup.rom_capacity_flushes -
                    start->jit_lookup.rom_capacity_flushes),
            (unsigned)(end->jit_lookup.ram_capacity_flushes -
                    start->jit_lookup.ram_capacity_flushes));
    ESP_LOGI(TAG, "BENCH_JIT_SIDE_COST version=2 first=%u last=%u "
            "page_cycles=%llu emit_cycles=%llu publish_cycles=%llu "
            "publish_pending_sum=%llu publish_pending_max_total=%u "
            "scope=included_in_side_create",
            first, last, JIT_DELTA(jit_lookup.side_page_cycles),
            JIT_DELTA(jit_lookup.side_emit_cycles),
            JIT_DELTA(jit_lookup.side_publish_cycles),
            JIT_DELTA(jit_lookup.side_publish_pending),
            end->jit_lookup.side_publish_max_pending);
    ESP_LOGI(TAG, "BENCH_JIT_TRANSLATE_PHASE version=1 first=%u last=%u "
            "join_cycles=%llu scan_cycles=%llu emit_cycles=%llu "
            "record_cycles=%llu publish_cycles=%llu sync_cycles=%llu "
            "patch_syncs=%llu scope=thumb_translation",
            first, last, JIT_DELTA(jit_lookup.phase_join_cycles),
            JIT_DELTA(jit_lookup.phase_scan_cycles),
            JIT_DELTA(jit_lookup.phase_emit_cycles),
            JIT_DELTA(jit_lookup.phase_record_cycles),
            JIT_DELTA(jit_lookup.phase_publish_cycles),
            JIT_DELTA(jit_lookup.phase_sync_cycles),
            JIT_DELTA(jit_lookup.phase_patch_syncs));
    for (unsigned i = 0; i < 3; i++)
    {
        ESP_LOGI(TAG, "BENCH_JIT_LOOKUP version=2 first=%u last=%u region=%s "
                "hits=%llu misses=%llu hash_probes=%llu "
                "translate_calls=%llu translate_failures=%llu "
                "translate_cycles=%llu",
                first, last, regions[i],
                (unsigned long long)(after[i]->hits - before[i]->hits),
                (unsigned long long)(after[i]->misses - before[i]->misses),
                (unsigned long long)(after[i]->hash_probes - before[i]->hash_probes),
                (unsigned long long)(after[i]->translate_calls - before[i]->translate_calls),
                (unsigned long long)(after[i]->translate_failures - before[i]->translate_failures),
                (unsigned long long)(after[i]->translate_cycles - before[i]->translate_cycles));
#if CONFIG_GPSP_BENCHMARK_JIT_MISS_CLASS
        ESP_LOGI(TAG, "BENCH_JIT_MISS_CLASS version=2 first=%u last=%u region=%s "
                "repeated=%llu overlap=%llu new=%llu unknown=%llu",
                first, last, regions[i],
                (unsigned long long)(after[i]->repeated_misses - before[i]->repeated_misses),
                (unsigned long long)(after[i]->overlap_misses - before[i]->overlap_misses),
                (unsigned long long)(after[i]->new_misses - before[i]->new_misses),
                (unsigned long long)(after[i]->unknown_misses - before[i]->unknown_misses));
        ESP_LOGI(TAG, "BENCH_JIT_MISS_REASON version=2 first=%u last=%u region=%s "
                "initial=%llu budget=%llu update_pc_change=%llu ready=%llu "
                "irq=%llu smc=%llu halt=%llu other=%llu guard=%llu",
                first, last, regions[i],
                (unsigned long long)(after[i]->overlap_resume[0] - before[i]->overlap_resume[0]),
                (unsigned long long)(after[i]->overlap_resume[1] - before[i]->overlap_resume[1]),
                (unsigned long long)(after[i]->overlap_resume[2] - before[i]->overlap_resume[2]),
                (unsigned long long)(after[i]->overlap_resume[3] - before[i]->overlap_resume[3]),
                (unsigned long long)(after[i]->overlap_resume[4] - before[i]->overlap_resume[4]),
                (unsigned long long)(after[i]->overlap_resume[5] - before[i]->overlap_resume[5]),
                (unsigned long long)(after[i]->overlap_resume[6] - before[i]->overlap_resume[6]),
                (unsigned long long)(after[i]->overlap_resume[7] - before[i]->overlap_resume[7]),
                (unsigned long long)(after[i]->overlap_resume[8] - before[i]->overlap_resume[8]));
#if CONFIG_GPSP_BENCHMARK_JIT_CODE_BYTES
        ESP_LOGI(TAG, "BENCH_JIT_MISS_ENTRY version=2 first=%u last=%u region=%s "
                "safe=%llu batch=%llu unknown=%llu",
                first, last, regions[i],
                (unsigned long long)(after[i]->overlap_safe_misses -
                        before[i]->overlap_safe_misses),
                (unsigned long long)(after[i]->overlap_batch_misses -
                        before[i]->overlap_batch_misses),
                (unsigned long long)(after[i]->overlap_unknown_misses -
                        before[i]->overlap_unknown_misses));
#endif
#endif
    }
#if CONFIG_GPSP_BENCHMARK_JIT_MISS_CLASS
    ESP_LOGI(TAG, "BENCH_JIT_GUARD version=2 first=%u last=%u "
            "exits=%llu bios_exits=%llu lookups=%llu bios_lookups=%llu",
            first, last, JIT_DELTA(jit_lookup.guard_exits),
            JIT_DELTA(jit_lookup.guard_exits_bios),
            JIT_DELTA(jit_lookup.guard_lookups),
            JIT_DELTA(jit_lookup.guard_lookups_bios));
    ESP_LOGI(TAG, "BENCH_JIT_DIAG version=2 first=%u last=%u "
            "miss_classify_cycles=%llu overlap_classify_cycles=%llu "
            "scope=included_in_lookup overlap_scope=also_in_translation",
            first, last, JIT_DELTA(jit_lookup.miss_classify_cycles),
            JIT_DELTA(jit_lookup.overlap_classify_cycles));
#endif
#endif
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
    const struct gpsp_benchmark_jit_detail *detail_start = &start->jit_detail;
    const struct gpsp_benchmark_jit_detail *detail_end = &end->jit_detail;
#define DETAIL_DELTA(field) (unsigned long long)(detail_end->field - detail_start->field)
    ESP_LOGI(TAG, "BENCH_JIT_DETAIL version=2 first=%u last=%u "
            "code_cycles=%llu helper_cycles=%llu helper_calls=%llu "
            "ram_load_fast=%llu ram_load_slow=%llu "
            "ram_store_fast=%llu ram_store_slow=%llu "
            "multi_fast=%llu multi_slow=%llu "
            "block_bios_cycles=%llu block_ewram_cycles=%llu "
            "block_iwram_cycles=%llu block_rom_cycles=%llu "
            "block_other_cycles=%llu",
            first, last, DETAIL_DELTA(code_cycles),
            DETAIL_DELTA(helper_cycles), DETAIL_DELTA(helper_calls),
            DETAIL_DELTA(ram_load_fast), DETAIL_DELTA(ram_load_slow),
            DETAIL_DELTA(ram_store_fast), DETAIL_DELTA(ram_store_slow),
            DETAIL_DELTA(multi_fast), DETAIL_DELTA(multi_slow),
            DETAIL_DELTA(block_cycles.bios), DETAIL_DELTA(block_cycles.ewram),
            DETAIL_DELTA(block_cycles.iwram), DETAIL_DELTA(block_cycles.rom),
            DETAIL_DELTA(block_cycles.other));
    ESP_LOGI(TAG, "BENCH_JIT_EXITS version=2 first=%u last=%u "
            "ready=%llu budget=%llu halt=%llu irq_alert=%llu "
            "smc_alert=%llu resume_candidates=%llu resume_attempts=%llu "
            "resume_successes=%llu resume_rejections=%llu",
            first, last, DETAIL_DELTA(block_ready_exits),
            DETAIL_DELTA(block_budget_exits), DETAIL_DELTA(block_halt_exits),
            DETAIL_DELTA(block_irq_alerts), DETAIL_DELTA(block_smc_alerts),
            DETAIL_DELTA(resume_candidates), DETAIL_DELTA(resume_attempts),
            DETAIL_DELTA(resume_successes), DETAIL_DELTA(resume_rejections));
    ESP_LOGI(TAG, "BENCH_JIT_HOT version=2 first=%u last=%u "
            "stride=64 samples=%llu overflow=%llu "
            "pc_scope=entry cycles_scope=block_inclusive rank=sampled_cycles "
            "pc0=%08x thumb0=%u count0=%u cycles0=%llu "
            "pc1=%08x thumb1=%u count1=%u cycles1=%llu "
            "pc2=%08x thumb2=%u count2=%u cycles2=%llu "
            "pc3=%08x thumb3=%u count3=%u cycles3=%llu",
            first, last,
            (unsigned long long)end->jit_hot.samples,
            (unsigned long long)end->jit_hot.overflow,
            end->jit_hot.top[0].pc & ~1u, end->jit_hot.top[0].pc & 1u,
            end->jit_hot.top[0].samples,
            (unsigned long long)end->jit_hot.top[0].cycles,
            end->jit_hot.top[1].pc & ~1u, end->jit_hot.top[1].pc & 1u,
            end->jit_hot.top[1].samples,
            (unsigned long long)end->jit_hot.top[1].cycles,
            end->jit_hot.top[2].pc & ~1u, end->jit_hot.top[2].pc & 1u,
            end->jit_hot.top[2].samples,
            (unsigned long long)end->jit_hot.top[2].cycles,
            end->jit_hot.top[3].pc & ~1u, end->jit_hot.top[3].pc & 1u,
            end->jit_hot.top[3].samples,
            (unsigned long long)end->jit_hot.top[3].cycles);
    ESP_LOGI(TAG, "BENCH_JIT_MEMORY version=2 first=%u last=%u "
            "memory_cycles=%llu memory_calls=%llu "
            "memory_rom_loads=%llu memory_ram_loads=%llu "
            "memory_io_loads=%llu memory_other_loads=%llu memory_stores=%llu",
            first, last, DETAIL_DELTA(memory_cycles), DETAIL_DELTA(memory_calls),
            DETAIL_DELTA(memory_rom_loads), DETAIL_DELTA(memory_ram_loads),
            DETAIL_DELTA(memory_io_loads), DETAIL_DELTA(memory_other_loads),
            DETAIL_DELTA(memory_stores));
    ESP_LOGI(TAG, "BENCH_JIT_HELPERS version=2 first=%u last=%u "
            "arm_memory_cycles=%llu arm_memory_calls=%llu "
            "arm_block_cycles=%llu arm_block_calls=%llu system_calls=%llu",
            first, last, DETAIL_DELTA(arm_memory_cycles),
            DETAIL_DELTA(arm_memory_calls), DETAIL_DELTA(arm_block_cycles),
            DETAIL_DELTA(arm_block_calls), DETAIL_DELTA(system_calls));
    ESP_LOGI(TAG, "BENCH_JIT_ARM_MEMORY version=2 first=%u last=%u "
            "ram=%llu io=%llu video=%llu rom=%llu save=%llu other=%llu stores=%llu",
            first, last, DETAIL_DELTA(arm_memory_ram), DETAIL_DELTA(arm_memory_io),
            DETAIL_DELTA(arm_memory_video), DETAIL_DELTA(arm_memory_rom),
            DETAIL_DELTA(arm_memory_save), DETAIL_DELTA(arm_memory_other),
            DETAIL_DELTA(arm_memory_stores));
    static const char *video_regions[] = {"palette", "vram", "oam"};
    static const char *video_ops[] = {"read", "write"};
    static const unsigned video_widths[] = {8, 16, 32};
    for (unsigned region = 0; region < 3; region++)
    {
        for (unsigned op = 0; op < 2; op++)
        {
            for (unsigned width = 0; width < 3; width++)
            {
                ESP_LOGI(TAG, "BENCH_JIT_VIDEO_MEM version=2 first=%u last=%u "
                        "region=%s op=%s width=%u calls=%llu cycles=%llu",
                        first, last, video_regions[region], video_ops[op],
                        video_widths[width],
                        (unsigned long long)(end->arm_video_memory.calls[region][op][width] -
                                start->arm_video_memory.calls[region][op][width]),
                        (unsigned long long)(end->arm_video_memory.cycles[region][op][width] -
                                start->arm_video_memory.cycles[region][op][width]));
            }
        }
        ESP_LOGI(TAG, "BENCH_JIT_BIOS_VIDEO_STORE version=2 first=%u last=%u "
                "region=%s bios_calls=%llu bios_cycles=%llu "
                "pc_7b4_calls=%llu pc_7b4_cycles=%llu",
                first, last, video_regions[region],
                (unsigned long long)(end->arm_video_source.bios_store_calls[region] -
                        start->arm_video_source.bios_store_calls[region]),
                (unsigned long long)(end->arm_video_source.bios_store_cycles[region] -
                        start->arm_video_source.bios_store_cycles[region]),
                (unsigned long long)(end->arm_video_source.pc_7b4_store_calls[region] -
                        start->arm_video_source.pc_7b4_store_calls[region]),
                (unsigned long long)(end->arm_video_source.pc_7b4_store_cycles[region] -
                        start->arm_video_source.pc_7b4_store_cycles[region]));
    }
    ESP_LOGI(TAG, "BENCH_JIT_ROM version=2 first=%u last=%u "
            "read_body_cycles=%llu read_body_calls=%llu page_misses=%llu "
            "read_bridge_cycles=%llu read_bridge_calls=%llu",
            first, last, DETAIL_DELTA(rom_read_body_cycles),
            DETAIL_DELTA(rom_read_body_calls), DETAIL_DELTA(rom_read_page_misses),
            DETAIL_DELTA(rom_read_bridge_cycles),
            DETAIL_DELTA(rom_read_bridge_calls));
    ESP_LOGI(TAG, "BENCH_JIT_BIOS version=2 first=%u last=%u "
            "block_calls=%llu block_to_bios=%llu "
            "block_bios_to_bios=%llu block_bios_to_bios_ready=%llu "
            "block_bios_to_other=%llu block_bios_to_other_ready=%llu "
            "update_to_bios=%llu update_from_bios=%llu "
            "irq_to_bios=%llu irq_from_bios=%llu "
            "interpreter_to_bios=%llu interpreter_from_bios=%llu",
            first, last,
            (unsigned long long)(bios_end->block_calls - bios_start->block_calls),
            (unsigned long long)(bios_end->block_to_bios - bios_start->block_to_bios),
            (unsigned long long)(bios_end->block_bios_to_bios - bios_start->block_bios_to_bios),
            (unsigned long long)(bios_end->block_bios_to_bios_ready -
                    bios_start->block_bios_to_bios_ready),
            (unsigned long long)(bios_end->block_bios_to_other - bios_start->block_bios_to_other),
            (unsigned long long)(bios_end->block_bios_to_other_ready -
                    bios_start->block_bios_to_other_ready),
            (unsigned long long)(bios_end->update_to_bios - bios_start->update_to_bios),
            (unsigned long long)(bios_end->update_from_bios - bios_start->update_from_bios),
            (unsigned long long)(bios_end->irq_to_bios - bios_start->irq_to_bios),
            (unsigned long long)(bios_end->irq_from_bios - bios_start->irq_from_bios),
            (unsigned long long)(bios_end->interpreter_to_bios - bios_start->interpreter_to_bios),
            (unsigned long long)(bios_end->interpreter_from_bios -
                    bios_start->interpreter_from_bios));
    ESP_LOGI(TAG, "BENCH_JIT_BIOS_EDGE version=2 first=%u last=%u "
            "fastset_7a8_to_7a4_ready=%llu fastset_7b8_to_7a4_ready=%llu "
            "fastset_7b8_to_7a8_ready=%llu swi_8_to_64_ready=%llu",
            first, last,
            (unsigned long long)(bios_end->fastset_outer_from_7a8_ready -
                    bios_start->fastset_outer_from_7a8_ready),
            (unsigned long long)(bios_end->fastset_outer_from_7b8_ready -
                    bios_start->fastset_outer_from_7b8_ready),
            (unsigned long long)(bios_end->fastset_inner_from_7b8_ready -
                    bios_start->fastset_inner_from_7b8_ready),
            (unsigned long long)(bios_end->swi_entry_to_64_ready -
                    bios_start->swi_entry_to_64_ready));
    for (unsigned rank = 0; rank < 8; rank++)
        ESP_LOGI(TAG, "BENCH_JIT_BIOS_TOP version=2 first=%u last=%u "
                "scope=cumulative rank=%u pc=%u calls=%u",
                first, last, rank, bios_end->top_blocks[rank].pc,
                bios_end->top_blocks[rank].calls);
#undef DETAIL_DELTA
#endif
#if CONFIG_GPSP_BENCHMARK_JIT_CODE_BYTES
    ESP_LOGI(TAG, "BENCH_JIT_CODE_BYTES version=2 first=%u last=%u "
            "total=%llu guard=%llu body=%llu timing=%llu "
            "preflight=%llu slow=%llu link_exit=%llu residual=%llu "
            "overcount_blocks=%llu",
            first, last, JIT_DELTA(jit_code_bytes.total),
            JIT_DELTA(jit_code_bytes.guard), JIT_DELTA(jit_code_bytes.body),
            JIT_DELTA(jit_code_bytes.timing),
            JIT_DELTA(jit_code_bytes.batch_preflight),
            JIT_DELTA(jit_code_bytes.batch_slow),
            JIT_DELTA(jit_code_bytes.link_exit),
            JIT_DELTA(jit_code_bytes.residual),
            JIT_DELTA(jit_code_bytes.overcount_blocks));
    ESP_LOGI(TAG, "BENCH_JIT_OVERLAP_BYTES version=2 first=%u last=%u "
            "new_blocks=%llu new_bytes=%llu repeated_blocks=%llu "
            "repeated_bytes=%llu safe_blocks=%llu safe_bytes=%llu "
            "batch_blocks=%llu batch_bytes=%llu "
            "unknown_blocks=%llu unknown_bytes=%llu",
            first, last, JIT_DELTA(jit_overlap_bytes.new_blocks),
            JIT_DELTA(jit_overlap_bytes.new_bytes),
            JIT_DELTA(jit_overlap_bytes.repeated_blocks),
            JIT_DELTA(jit_overlap_bytes.repeated_bytes),
            JIT_DELTA(jit_overlap_bytes.safe_blocks),
            JIT_DELTA(jit_overlap_bytes.safe_bytes),
            JIT_DELTA(jit_overlap_bytes.batch_blocks),
            JIT_DELTA(jit_overlap_bytes.batch_bytes),
            JIT_DELTA(jit_overlap_bytes.unknown_blocks),
            JIT_DELTA(jit_overlap_bytes.unknown_bytes));
    ESP_LOGI(TAG, "BENCH_JIT_ALIAS version=2 first=%u last=%u "
            "hits=%llu joins=%llu alloc_failures=%llu "
            "entries=%u peak_entries=%u table_bytes=%u "
            "side_pending=%u "
            "side_entry_created=%llu side_entry_bytes=%llu "
            "fused_branch_regions=%llu "
            "branch_side_created=%llu branch_side_bytes=%llu "
            "side_entry_hits=%llu side_entry_failures=%llu",
            first, last, JIT_DELTA(jit_overlap_bytes.alias_hits),
            JIT_DELTA(jit_overlap_bytes.joins),
            JIT_DELTA(jit_overlap_bytes.alias_alloc_failures),
            end->jit_overlap_bytes.alias_entries,
            end->jit_overlap_bytes.alias_peak_entries,
            end->jit_overlap_bytes.alias_table_bytes,
            end->jit_overlap_bytes.side_pending,
            JIT_DELTA(jit_overlap_bytes.side_entry_created),
            JIT_DELTA(jit_overlap_bytes.side_entry_bytes),
            JIT_DELTA(jit_overlap_bytes.fused_branch_regions),
            JIT_DELTA(jit_overlap_bytes.branch_side_created),
            JIT_DELTA(jit_overlap_bytes.branch_side_bytes),
            JIT_DELTA(jit_overlap_bytes.side_entry_hits),
            JIT_DELTA(jit_overlap_bytes.side_entry_failures));
    ESP_LOGI(TAG, "BENCH_JIT_SLOW_POOL version=2 first=%u last=%u "
            "rom_hits=%llu rom_emitted=%llu rom_saved_bytes=%llu "
            "rom_replacements=%llu rom_table_bytes=%u "
            "ram_hits=%llu ram_emitted=%llu ram_saved_bytes=%llu "
            "ram_replacements=%llu ram_table_bytes=%u",
            first, last,
            JIT_DELTA(jit_overlap_bytes.slow_rom_hits),
            JIT_DELTA(jit_overlap_bytes.slow_rom_emitted),
            JIT_DELTA(jit_overlap_bytes.slow_rom_saved_bytes),
            JIT_DELTA(jit_overlap_bytes.slow_rom_replacements),
            end->jit_overlap_bytes.slow_rom_table_bytes,
            JIT_DELTA(jit_overlap_bytes.slow_ram_hits),
            JIT_DELTA(jit_overlap_bytes.slow_ram_emitted),
            JIT_DELTA(jit_overlap_bytes.slow_ram_saved_bytes),
            JIT_DELTA(jit_overlap_bytes.slow_ram_replacements),
            end->jit_overlap_bytes.slow_ram_table_bytes);
#endif
    ESP_LOGI(TAG, "BENCH_JIT_FALLBACK version=2 first=%u last=%u "
            "generated=%llu arm=%llu thumb=%llu direct=%llu "
            "bulk_runs=%llu bulk_steps=%llu",
            first, last, JIT_DELTA(generated_steps),
            JIT_DELTA(generated_arm), JIT_DELTA(generated_thumb),
            JIT_DELTA(direct_steps), JIT_DELTA(bulk_runs),
            JIT_DELTA(bulk_steps));
    ESP_LOGI(TAG, "BENCH_JIT_REGIONS version=2 first=%u last=%u "
            "dispatch_bios=%llu dispatch_ewram=%llu dispatch_iwram=%llu "
            "dispatch_rom=%llu dispatch_other=%llu "
            "generated_bios=%llu generated_ewram=%llu "
            "generated_iwram=%llu generated_rom=%llu generated_other=%llu "
            "bulk_bios=%llu bulk_ewram=%llu bulk_iwram=%llu bulk_rom=%llu "
            "bulk_other=%llu",
            first, last, JIT_DELTA(block_region.bios),
            JIT_DELTA(block_region.ewram), JIT_DELTA(block_region.iwram),
            JIT_DELTA(block_region.rom), JIT_DELTA(block_region.other),
            JIT_DELTA(generated_region.bios),
            JIT_DELTA(generated_region.ewram), JIT_DELTA(generated_region.iwram),
            JIT_DELTA(generated_region.rom), JIT_DELTA(generated_region.other),
            JIT_DELTA(bulk_region.bios),
            JIT_DELTA(bulk_region.ewram), JIT_DELTA(bulk_region.iwram),
            JIT_DELTA(bulk_region.rom), JIT_DELTA(bulk_region.other));
    for (unsigned rank = 0; rank < 8; rank++)
        ESP_LOGI(TAG, "BENCH_JIT_FALLBACK_TOP version=2 first=%u last=%u "
                "scope=cumulative rank=%u "
                "thumb_op=%02x thumb_steps=%u arm_op=%02x arm_steps=%u",
                first, last, rank,
                end->fallback_thumb_top[rank].opcode_high_byte,
                end->fallback_thumb_top[rank].steps,
                end->fallback_arm_top[rank].opcode_high_byte,
                end->fallback_arm_top[rank].steps);
#undef JIT_DELTA
}

static void gpsp_benchmark_report_slice(unsigned first, unsigned last,
        const struct gpsp_benchmark_slice *slice
        , const struct gpsp_benchmark_engine *engine_start,
        const struct gpsp_benchmark_engine *engine_end
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
        , const struct gpsp_benchmark_bios_probe *bios_start,
        const struct gpsp_benchmark_bios_probe *bios_end
#endif
        )
{
    const struct gpsp_benchmark_window *frame = &slice->frame;

    ESP_LOGI(TAG, "BENCH_SLICE version=1 first=%u last=%u frames=%u "
            "run_us=%llu run_max_us=%u execute_us=%llu audio_us=%llu "
            "video_us=%llu wait_us=%llu cpu_cycles=%llu "
            "scanline_cycles=%llu video_frames=%u submitted=%u sent=%u "
            "dropped=%u checksum=%08lx",
            first, last, frame->frames,
            (unsigned long long)frame->run_us, frame->run_max_us,
            (unsigned long long)frame->execute_us,
            (unsigned long long)frame->audio_us,
            (unsigned long long)frame->video_us,
            (unsigned long long)frame->wait_us,
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_CPU],
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_SCANLINE],
            slice->video_frames, slice->submitted, slice->sent, slice->dropped,
            (unsigned long)slice->checksum);
    ESP_LOGI(TAG, "BENCH_CPU_SLICE version=1 first=%u last=%u "
            "sched_cycles=%llu timer_cycles=%llu dma_cycles=%llu "
            "sound_cycles=%llu video_setup_cycles=%llu",
            first, last,
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_SCHED],
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_TIMER],
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_DMA],
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_SOUND],
            (unsigned long long)slice->cycles[GPSP_BENCHMARK_VIDEO_SETUP]);
#if CONFIG_GPSP_BENCHMARK_INSTRET
    ESP_LOGI(TAG, "BENCH_INSTRET_SLICE version=1 first=%u last=%u "
            "cpu=%llu scanline=%llu sched=%llu timer=%llu dma=%llu sound=%llu "
            "video_setup=%llu",
            first, last,
            (unsigned long long)slice->instret[GPSP_BENCHMARK_CPU],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_SCANLINE],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_SCHED],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_TIMER],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_DMA],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_SOUND],
            (unsigned long long)slice->instret[GPSP_BENCHMARK_VIDEO_SETUP]);
#endif
#if CONFIG_GPSP_BENCHMARK_PHASE_WORK
    ESP_LOGI(TAG, "BENCH_WORK_SLICE version=1 first=%u last=%u "
            "sched=%llu timer=%llu dma=%llu sound=%llu "
            "dma_bytes=%llu direct_calls=%llu direct_samples=%llu "
            "direct_active_calls=%llu direct_active_samples=%llu "
            "psg_calls=%llu psg_samples=%llu",
            first, last,
            (unsigned long long)slice->enters[GPSP_BENCHMARK_SCHED],
            (unsigned long long)slice->enters[GPSP_BENCHMARK_TIMER],
            (unsigned long long)slice->enters[GPSP_BENCHMARK_DMA],
            (unsigned long long)slice->enters[GPSP_BENCHMARK_SOUND],
            (unsigned long long)slice->dma_bytes,
            (unsigned long long)slice->direct_calls,
            (unsigned long long)slice->direct_samples,
            (unsigned long long)slice->direct_active_calls,
            (unsigned long long)slice->direct_active_samples,
            (unsigned long long)slice->psg_calls,
            (unsigned long long)slice->psg_samples);
#endif
#if CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL
    const struct gpsp_benchmark_video_detail *video = &slice->video;
    ESP_LOGI(TAG, "BENCH_VIDEO_SLICE version=1 first=%u last=%u "
            "oam_cycles=%llu layers_cycles=%llu draw_cycles=%llu "
            "affine_cycles=%llu text_bg_cycles=%llu affine_bg_cycles=%llu "
            "bitmap_bg_cycles=%llu obj_layer_cycles=%llu effects_cycles=%llu",
            first, last,
            (unsigned long long)video->oam_cycles,
            (unsigned long long)video->layers_cycles,
            (unsigned long long)video->draw_cycles,
            (unsigned long long)video->affine_cycles,
            (unsigned long long)video->text_bg_cycles,
            (unsigned long long)video->affine_bg_cycles,
            (unsigned long long)video->bitmap_bg_cycles,
            (unsigned long long)video->obj_layer_cycles,
            (unsigned long long)video->effects_cycles);
    ESP_LOGI(TAG, "BENCH_VIDEO_LINES_SLICE version=1 first=%u last=%u "
            "oam_updates=%u forced_blank=%u window=%u "
            "mode0=%u mode1=%u mode2=%u mode3=%u mode4=%u mode5=%u",
            first, last, (unsigned)video->oam_updates,
            (unsigned)video->forced_blank_lines, (unsigned)video->window_lines,
            (unsigned)video->mode_lines[0], (unsigned)video->mode_lines[1],
            (unsigned)video->mode_lines[2], (unsigned)video->mode_lines[3],
            (unsigned)video->mode_lines[4], (unsigned)video->mode_lines[5]);
#endif
    gpsp_benchmark_report_jit(first, last, engine_start, engine_end
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
            , bios_start, bios_end
#endif
            );
}

static void gpsp_benchmark_report_slices(void)
{
    for (unsigned i = 0; i < GPSP_BENCHMARK_SLICE_COUNT; i++)
    {
        unsigned first = i * GPSP_BENCHMARK_SLICE_FRAMES + 1;
        unsigned last = first + GPSP_BENCHMARK_SLICE_FRAMES - 1;

        if (last > GPSP_BENCHMARK_FRAME_COUNT)
            last = GPSP_BENCHMARK_FRAME_COUNT;

        gpsp_benchmark_report_slice(first, last, &s_benchmark_slices[i]
                , &s_benchmark_engines[i], &s_benchmark_engines[i + 1]
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
                , &s_benchmark_bios_probes[i], &s_benchmark_bios_probes[i + 1]
#endif
                );
        vTaskDelay(1);
    }
}

static void gpsp_benchmark_free(void)
{
    heap_caps_free(s_benchmark_images);
    s_benchmark_images = NULL;
    heap_caps_free(s_benchmark_engines);
    s_benchmark_engines = NULL;
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
    heap_caps_free(s_benchmark_bios_probes);
    s_benchmark_bios_probes = NULL;
#endif
}
#endif

/* JIT upkeep runs only when this much time is left before the frame's
   deadline. A table rehash can still take longer on large tables. */
#define GPSP_JIT_UPKEEP_SLACK_US 10000

#if CONFIG_GPSP_STATUS && CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
/* GBA frame period: 280,896 cycles at 16,777,216 Hz. */
#define GPSP_FRAME_PERIOD_US 16743u

/* Collect slow frames without formatting or serial output on CPU0.
   run_us includes upkeep_us. */
static void gpsp_slow_frame_note(uint32_t run_us, uint32_t upkeep_us)
{
    static uint32_t last_blocks;
    static uint32_t last_cycles;
    static uint32_t last_render;
    static uint32_t last_rom_loads;
    static uint32_t last_rom_reloads;
    static uint32_t last_rom_remaps;
    static uint32_t last_rom_cycles;
    uint32_t blocks = 0;
    uint32_t cycles = 0;

    gpsp_jit_translation_stats(&blocks, &cycles);
    uint32_t new_blocks = blocks - last_blocks;
    uint32_t translate_us = (cycles - last_cycles) /
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t render = gpsp_video_render_cycles();
    uint32_t render_us = (render - last_render) /
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    last_blocks = blocks;
    last_cycles = cycles;
    last_render = render;
    uint32_t rom_loads, rom_reloads, rom_remaps, rom_cycles;
    gpsp_rom_page_stats(&rom_loads, &rom_reloads, &rom_remaps, &rom_cycles);
    uint32_t new_rom_loads = rom_loads - last_rom_loads;
    uint32_t new_rom_reloads = rom_reloads - last_rom_reloads;
    uint32_t new_rom_remaps = rom_remaps - last_rom_remaps;
    uint32_t rom_us = (rom_cycles - last_rom_cycles) /
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    last_rom_loads = rom_loads;
    last_rom_reloads = rom_reloads;
    last_rom_remaps = rom_remaps;
    last_rom_cycles = rom_cycles;
    if (run_us <= GPSP_FRAME_PERIOD_US)
        return;

    const struct gpsp_slow_frame frame = {
        .frame = s_frame_count, .run_us = run_us, .render_us = render_us,
        .translated = new_blocks, .translate_us = translate_us,
        .upkeep_us = upkeep_us, .rom_pages = new_rom_loads,
        .rom_reloads = new_rom_reloads, .rom_remaps = new_rom_remaps,
        .rom_load_us = rom_us,
    };
    gpsp_profile_record_slow(&frame);
}
#endif

/* The core's own frame buffer while it renders into board buffers. */
static uint16_t *s_core_screen;
/* The last frame shown, for screenshots. */
static const uint16_t *s_last_frame;
static size_t s_last_pitch;

/* Return the unused board buffer before the core frees its own one. */
static void gpsp_unload_game(void)
{
    if (s_core_screen)
    {
        uint16_t *previous = gpsp_screen_set_buffer(s_core_screen);

        gpsp_board_video_release(previous);
        s_core_screen = NULL;
    }
    retro_unload_game();
}

static void gpsp_video_refresh(
        const void *data, unsigned width, unsigned height, size_t pitch)
{
#if CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
    uint32_t report_frames;
    int64_t report_time_us;
    int64_t now_us;
#endif

    if (!data)
        return;

    gpsp_board_video_present(data, width, height, pitch);
    s_last_frame = data;
    s_last_pitch = pitch;
    /* Between frames: the next one renders straight into a board buffer. */
    uint16_t *next = gpsp_board_video_buffer();
    if (next)
    {
        uint16_t *previous = gpsp_screen_set_buffer(next);

        if (!s_core_screen)
            s_core_screen = previous;
    }
    else if (s_core_screen)
        /* A submitted buffer may already be in use by the display. */
        gpsp_screen_set_buffer(s_core_screen);
    s_frame_count++;
#if CONFIG_GPSP_BENCHMARK
    s_benchmark_pixels = data;
    s_benchmark_width = width;
    s_benchmark_height = height;
    s_benchmark_pitch = pitch;
    return;
#elif CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
    now_us = esp_timer_get_time();

    if (s_frame_count == 1)
    {
        ESP_LOGI(TAG, "First video frame: %ux%u pitch=%u checksum=%08lx",
                width, height, (unsigned)pitch,
                (unsigned long)gpsp_frame_checksum(
                        data, width, height, pitch));
    }
    else if (s_frame_count >= 32 &&
            (s_frame_count & (s_frame_count - 1)) == 0)
    {
        /* Stable guest-frame checkpoints permit interpreter/JIT comparisons
         * even when the two engines advance at different wall-clock rates. */
        ESP_LOGI(TAG, "Video checkpoint: frame=%lu checksum=%08lx",
                (unsigned long)s_frame_count,
                (unsigned long)gpsp_frame_checksum(data, width, height, pitch));
    }

    report_time_us = now_us - s_report_start_us;

    if (report_time_us < GPSP_PERFORMANCE_REPORT_INTERVAL_US)
        return;

    report_frames = s_frame_count - s_report_frame_count;
    uint32_t run_count = gpsp_profile_frames();
    ESP_LOGI(TAG,
            "Video: frames=%lu guest_fps=%.3f checksum=%08lx run_fps=%.3f",
            (unsigned long)s_frame_count,
            (double)report_frames * 1000000.0 / (double)report_time_us,
            (unsigned long)gpsp_frame_checksum(data, width, height, pitch),
            (double)(run_count - s_report_run_count) * 1000000.0 /
                    (double)report_time_us);
    s_report_frame_count = s_frame_count;
    s_report_run_count = run_count;
    s_report_start_us = now_us;
#endif
}

#if GPSP_FRAMESKIP
/* Keep whole audio frames while the queue is below target; drop the rest.
   Crossfade the first dropped frame into the next kept frame without
   changing the sample rate. */
static size_t gpsp_fast_forward_audio(const int16_t *data, size_t frames)
{
    int16_t mixed[GPSP_FAST_FORWARD_FADE * 2];
    size_t count = s_fast_forward_tail_frames;
    size_t written = 0;

    if (!s_fast_forward_keep)
    {
        if (s_fast_forward_kept)
        {
            count = frames < GPSP_FAST_FORWARD_FADE ?
                    frames : GPSP_FAST_FORWARD_FADE;
            memcpy(s_fast_forward_tail, data, count * 2 * sizeof(*data));
            s_fast_forward_tail_frames = count;
        }
        s_fast_forward_kept = false;
        return frames;
    }
    s_fast_forward_kept = true;

    if (count > frames)
        count = frames;
    for (size_t i = 0; i < count * 2; i++)
    {
        int32_t in = (int32_t)(i / 2) + 1;

        mixed[i] = (int16_t)((data[i] * in +
                s_fast_forward_tail[i] * ((int32_t)count + 1 - in)) /
                ((int32_t)count + 1));
    }
    s_fast_forward_tail_frames = 0;
    if (count)
        written = gpsp_board_audio_write(mixed, count);
    if (written == count)
        written += gpsp_board_audio_write(data + count * 2, frames - count);
    return written;
}
#endif

static size_t gpsp_audio_sample_batch(const int16_t *data, size_t frames)
{
#if GPSP_FRAMESKIP
    size_t written = s_fast_forward_runs > 1 ?
            gpsp_fast_forward_audio(data, frames) :
            gpsp_board_audio_write(data, frames);
    /* Accepted samples show the speaker backend works. */
    if (written)
        s_audio_live = true;
    return written;
#else
    return gpsp_board_audio_write(data, frames);
#endif
}

#if GPSP_FRAMESKIP
/* The RetroPad buttons the core reads for each control. */
static const unsigned s_control_ids[] = {
    [GPSP_SETTINGS_CONTROL_A] = RETRO_DEVICE_ID_JOYPAD_A,
    [GPSP_SETTINGS_CONTROL_B] = RETRO_DEVICE_ID_JOYPAD_B,
    [GPSP_SETTINGS_CONTROL_L] = RETRO_DEVICE_ID_JOYPAD_L,
    [GPSP_SETTINGS_CONTROL_R] = RETRO_DEVICE_ID_JOYPAD_R,
    [GPSP_SETTINGS_CONTROL_START] = RETRO_DEVICE_ID_JOYPAD_START,
    [GPSP_SETTINGS_CONTROL_SELECT] = RETRO_DEVICE_ID_JOYPAD_SELECT,
    [GPSP_SETTINGS_CONTROL_TURBO_A] = RETRO_DEVICE_ID_JOYPAD_X,
    [GPSP_SETTINGS_CONTROL_TURBO_B] = RETRO_DEVICE_ID_JOYPAD_Y,
};

/* Pad buttons to the core's buttons; fast forward is taken out. */
static uint32_t gpsp_input_map(uint32_t input)
{
    uint32_t mapped = input & ((1u << RETRO_DEVICE_ID_JOYPAD_UP) |
            (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) |
            (1u << RETRO_DEVICE_ID_JOYPAD_LEFT) |
            (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT));
    unsigned button = gpsp_settings_control(
            GPSP_SETTINGS_CONTROL_FAST_FORWARD);
    bool held = button != GPSP_SETTINGS_BUTTON_NONE &&
            (input & (1u << button));

    for (int i = 0; i < GPSP_SETTINGS_CONTROL_FAST_FORWARD; i++)
    {
        button = gpsp_settings_control(i);
        if (button != GPSP_SETTINGS_BUTTON_NONE && (input & (1u << button)))
            mapped |= 1u << s_control_ids[i];
    }
    if (!gpsp_settings_fast_forward_toggle())
        s_fast_forward = held;
    else if (held && !s_fast_forward_held)
        s_fast_forward = !s_fast_forward;
    s_fast_forward_held = held;
    return mapped;
}
#endif

#if CONFIG_GPSP_MENU
/* Track button edges even while requests wait for all buttons to be up. */
static bool gpsp_menu_requested(uint32_t input)
{
    const uint32_t combo = (1u << RETRO_DEVICE_ID_JOYPAD_SELECT) |
            (1u << RETRO_DEVICE_ID_JOYPAD_START);
    uint32_t prev = s_input_prev;

    s_input_prev = input;
    return !s_input_blocked &&
            (((input & ~prev) & GPSP_BOARD_INPUT_HOME) ||
                    ((input & combo) == combo && (prev & combo) != combo));
}
#endif

static void gpsp_input_poll(void)
{
    uint32_t input = gpsp_board_input_read_physical();
    uint32_t touch = gpsp_board_virtual_input();

#if CONFIG_GPSP_MENU
    /* The menu opens after this frame, which already sees no buttons. */
    if (gpsp_menu_requested(input | touch))
    {
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
        ESP_LOGI(TAG, "Menu requested input=0x%05x", (unsigned)input);
#endif
        s_menu_pending = true;
        s_input_blocked = true;
#if GPSP_FRAMESKIP
        s_fast_forward = false;
#endif
    }
    if (s_input_blocked && (input | touch))
        input = touch = 0;
    else
        s_input_blocked = false;
#endif
#if CONFIG_GPSP_BENCHMARK
    input |= CONFIG_GPSP_BENCHMARK_HOLD_BUTTONS;
#endif
#if GPSP_FRAMESKIP
    input = gpsp_input_map(input);
#endif
    input |= touch;
    s_input_state = (uint16_t)input;
}

static int16_t gpsp_input_state(
        unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0)
        return 0;

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)s_input_state;

    if (id >= 16)
        return 0;

    return (s_input_state & (1u << id)) != 0;
}

static void gpsp_log_memory(const char *stage)
{
    ESP_LOGI(TAG, "%s memory: %u KiB internal, %u KiB PSRAM free "
            "(largest %u KiB)", stage,
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                    1024));
}

esp_err_t gpsp_frontend_run(const char *rom_path)
{
    const struct retro_game_info game = {
        .path = rom_path,
        .data = NULL,
        .size = 0,
        .meta = NULL,
    };
    struct retro_system_av_info av_info = {0};
    esp_timer_handle_t frame_timer = NULL;
    esp_err_t result;
#if CONFIG_GPSP_BENCHMARK
    uint32_t benchmark_frame = 0;
#endif
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
    double frame_period_us;
    double next_frame_us;
    int64_t last_idle_yield_us;
#else
    unsigned unthrottled_frames = 0;
#endif

    if (!rom_path)
        return ESP_ERR_INVALID_ARG;

#if CONFIG_GPSP_MENU
    /* Keep ROM selection buttons out of the game until they are released. */
    s_input_blocked = true;
#endif
#if GPSP_FRAMESKIP
    /* A game closed during fast forward leaves it on. */
    s_fast_forward = false;
    s_fast_forward_runs = 1;
#endif
    /* After a switch, screenshots wait for this game's first frame. */
    s_last_frame = NULL;

#if CONFIG_GPSP_BENCHMARK_INSTRET
    gpsp_benchmark_set_instret_counter(gpsp_instret_counter);
#endif
    retro_set_environment(gpsp_environment);
    retro_set_video_refresh(gpsp_video_refresh);
    retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(gpsp_audio_sample_batch);
    retro_set_input_poll(gpsp_input_poll);
    retro_set_input_state(gpsp_input_state);

    gpsp_log_memory("Before core init");
    gpsp_settings_load();
#if CONFIG_GPSP_BENCHMARK
    /* Match the core: benchmarks draw every frame. */
    gpsp_board_video_frameskip(0);
#endif
    gpsp_board_prepare_core();
    retro_init();

    if (!retro_load_game(&game))
    {
        ESP_LOGE(TAG, "Failed to load ROM: %s", rom_path);
        retro_deinit();
        return ESP_FAIL;
    }

    gpsp_ui_set_game(true);
    if (!gpsp_state_start())
        ESP_LOGW(TAG, "Manual state storage unavailable for this ROM");
#if !CONFIG_GPSP_BENCHMARK
    gpsp_settings_note_rom(rom_path);
#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
    bool skip_auto_load = gpsp_storage_skip_auto_load();
#else
    bool skip_auto_load = false;
#endif
    if (gpsp_settings_auto_load() && !skip_auto_load &&
            gpsp_state_used(GPSP_STATE_AUTO))
        gpsp_state_load(GPSP_STATE_AUTO);
#endif

#if CONFIG_GPSP_BENCHMARK && CONFIG_GPSP_BENCHMARK_LOAD_STATE
    if (!gpsp_state_load(0))
    {
        ESP_LOGE(TAG, "BENCH_STATE_LOAD_FAILED");
        gpsp_unload_game();
        retro_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BENCH_STATE_LOADED");
#endif

    retro_get_system_av_info(&av_info);

    if (av_info.timing.fps <= 0.0)
    {
        ESP_LOGE(TAG, "Core returned an invalid frame rate");
        gpsp_unload_game();
        retro_deinit();
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_GPSP_BENCHMARK
    /* Every measured frame must use the same connected output path. */
    int64_t ready_deadline_us = esp_timer_get_time() + 10000000;
    while (!gpsp_board_ready() && esp_timer_get_time() < ready_deadline_us)
        vTaskDelay(1);
    if (!gpsp_board_ready())
    {
        ESP_LOGE(TAG, "Benchmark receiver did not connect");
        gpsp_unload_game();
        retro_deinit();
        return ESP_ERR_TIMEOUT;
    }
#endif

#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
    const esp_timer_create_args_t timer_args = {
        .callback = gpsp_frame_timer_callback,
        .arg = xTaskGetCurrentTaskHandle(),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "gpsp_frame",
    };

    result = esp_timer_create(&timer_args, &frame_timer);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create frame timer: %s",
                esp_err_to_name(result));
        gpsp_unload_game();
        retro_deinit();
        return result;
    }
#endif

#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
    result = gpsp_save_start(rom_path);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start cartridge saves: %s",
                esp_err_to_name(result));
        if (frame_timer)
            esp_timer_delete(frame_timer);
        gpsp_unload_game();
        retro_deinit();
        return result;
    }
#endif

#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
    frame_period_us = 1000000.0 / av_info.timing.fps;
    next_frame_us = (double)esp_timer_get_time();
    last_idle_yield_us = (int64_t)next_frame_us;
#endif
    s_frame_count = 0;
#if CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
    s_report_frame_count = 0;
    s_report_run_count = gpsp_profile_frames();
    s_report_start_us = esp_timer_get_time();
#endif
#if CONFIG_GPSP_BENCHMARK
    s_benchmark_pixels = NULL;
    s_benchmark_images = heap_caps_malloc(
            GPSP_BENCHMARK_SLICE_COUNT * GPSP_BENCHMARK_IMAGE_PIXELS *
            sizeof(*s_benchmark_images), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_benchmark_engines = heap_caps_malloc(
            (GPSP_BENCHMARK_SLICE_COUNT + 1) * sizeof(*s_benchmark_engines),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
    s_benchmark_bios_probes = heap_caps_malloc(
            (GPSP_BENCHMARK_SLICE_COUNT + 1) *
            sizeof(*s_benchmark_bios_probes),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_benchmark_images
            || !s_benchmark_engines
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
            || !s_benchmark_bios_probes
#endif
            )
    {
        ESP_LOGE(TAG, "Benchmark storage unavailable");
        gpsp_benchmark_free();
        if (frame_timer)
            esp_timer_delete(frame_timer);
        gpsp_unload_game();
        retro_deinit();
        return ESP_ERR_NO_MEM;
    }
    memset(s_benchmark_slices, 0, sizeof(s_benchmark_slices));
    gpsp_board_output_stats(&s_benchmark_output_prev);
    s_benchmark_video_prev = s_frame_count;
#endif

    gpsp_log_memory("After ROM load");
#if CONFIG_GPSP_ESPEMU_UNTHROTTLED
    const char *pacing = "unthrottled";
#else
    const char *pacing = "guest-clock";
#endif
    const char *configured_mode = "rv32-jit";
    ESP_LOGI(TAG,
            "Execution started: configured_mode=%s pacing=%s target_fps=%.3f ROM=%s",
            configured_mode, pacing, av_info.timing.fps, rom_path);
#if CONFIG_GPSP_BENCHMARK
    gpsp_benchmark_get_engine(&s_benchmark_engines[0]);
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
    gpsp_benchmark_take_hot_sites(&s_benchmark_engines[0].jit_hot);
    gpsp_benchmark_get_bios_probe(&s_benchmark_bios_probes[0]);
#endif
#endif

    bool quit = false;

    for (;;)
    {
#if CONFIG_GPSP_BENCHMARK
        if (benchmark_frame >= GPSP_BENCHMARK_FRAME_COUNT)
#endif
        {
            struct gpsp_board_request request = {0};
            bool keep_files __attribute__((unused)) = false;
#if CONFIG_GPSP_MENU
            if (s_menu_pending)
            {
                s_menu_pending = false;
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
                /* A paused game is a good time to write the cartridge. */
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
                ESP_LOGI(TAG, "Menu save flush begin");
#endif
                gpsp_save_flush();
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
                ESP_LOGI(TAG, "Menu save flush done");
#endif
#endif
                enum gpsp_menu_result menu = gpsp_menu_run(true);

                if (menu != GPSP_MENU_RESUME)
                    request.command = GPSP_BOARD_COMMAND_QUIT;
                keep_files = menu == GPSP_MENU_RELOAD ||
                        menu == GPSP_MENU_QUIT_KEEP_FILES;
                s_input_blocked = true;
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
                next_frame_us = (double)esp_timer_get_time();
#endif
            }
#endif
            if (request.command == GPSP_BOARD_COMMAND_NONE)
                /* Includes a state command that came during the menu. */
                request = gpsp_ui_command();
            if (request.command == GPSP_BOARD_COMMAND_QUIT)
            {
                /* The viewer reads this line. The cleanup below writes the
                   cartridge save when autosave is on. */
                ESP_LOGI(TAG, "Game closed by request");
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
                if (keep_files)
                    gpsp_save_disable();
#endif
#if !CONFIG_GPSP_BENCHMARK
                if (gpsp_settings_auto_save() && !keep_files)
                    gpsp_state_save(GPSP_STATE_AUTO, s_last_frame, s_last_pitch);
#endif
                quit = true;
                break;
            }
            if (request.command == GPSP_BOARD_COMMAND_SCREENSHOT)
            {
                if (s_last_frame)
                    gpsp_screenshot_print(s_last_frame, 240, 160, s_last_pitch);
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
                next_frame_us = (double)esp_timer_get_time();
#endif
            }
            else if (request.command != GPSP_BOARD_COMMAND_NONE &&
                    gpsp_board_command_valid(request))
            {
                int slot = gpsp_state_slot();
                bool success = request.command == GPSP_BOARD_COMMAND_SAVE_STATE ?
                        gpsp_state_save(slot, s_last_frame, s_last_pitch) :
                        gpsp_state_load(slot);
                gpsp_board_command_report(request, success);
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
                next_frame_us = (double)esp_timer_get_time();
#endif
            }
        }
        int64_t run_start_us = esp_timer_get_time();
        int64_t run_end_us;
#if CONFIG_GPSP_BENCHMARK
        struct gpsp_benchmark_frame core_frame;
        uint32_t benchmark_wait_us = 0;
        bool fixed_frame __attribute__((unused)) = benchmark_frame < GPSP_BENCHMARK_FRAME_COUNT;
#endif
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
        int64_t now_us;
        int64_t wait_us;
        int64_t wait_start_us;
#endif

#if GPSP_FRAMESKIP
        unsigned runs = s_fast_forward ?
                gpsp_settings_fast_forward_speed() : 1;

        if (runs != s_fast_forward_runs)
        {
            s_fast_forward_runs = runs;
            s_fast_forward_update = true;
            s_fast_forward_tail_frames = 0;
            s_fast_forward_kept = true;
        }
        gpsp_report_audio_status();
        for (unsigned i = 0; i < runs; i++)
        {
            s_fast_forward_keep = gpsp_board_audio_fill() < 1.0f;
            retro_run();
        }
        /* Speed reports count every core frame. */
        gpsp_profile_add_frames(runs - 1);
#else
        retro_run();
#endif
        run_end_us = esp_timer_get_time();
        uint32_t upkeep_us __attribute__((unused)) = 0;
        /* Counted in the frame's run time, like other core work. */
#if !CONFIG_GPSP_ESPEMU_UNTHROTTLED
        if (next_frame_us + frame_period_us - (double)run_end_us >=
                GPSP_JIT_UPKEEP_SLACK_US)
#endif
        {
            gpsp_jit_upkeep();
            int64_t upkeep_end_us = esp_timer_get_time();
            upkeep_us = (uint32_t)(upkeep_end_us - run_end_us);
            run_end_us = upkeep_end_us;
        }
#if CONFIG_GPSP_STATUS && CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
        gpsp_slow_frame_note((uint32_t)(run_end_us - run_start_us), upkeep_us);
#endif
#if CONFIG_GPSP_BENCHMARK
        gpsp_benchmark_get_last_frame(&core_frame);
        benchmark_frame++;
#if CONFIG_GPSP_BENCHMARK_CAPTURE_STATE_FRAME > 0
        if (benchmark_frame == CONFIG_GPSP_BENCHMARK_CAPTURE_STATE_FRAME)
        {
            if (!gpsp_state_save(0, NULL, 0))
            {
                ESP_LOGE(TAG, "BENCH_STATE_CAPTURE_FAILED frame=%u",
                        benchmark_frame);
                if (frame_timer)
                    esp_timer_delete(frame_timer);
                gpsp_benchmark_free();
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
                gpsp_save_stop();
#endif
                gpsp_unload_game();
                retro_deinit();
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "BENCH_STATE_CAPTURED frame=%u", benchmark_frame);
        }
#endif
#endif

#if CONFIG_GPSP_ESPEMU_UNTHROTTLED
        /* Let this core's idle task service its watchdog without pacing every
         * emulated frame against the slower ESP-EMU guest clock. */
        if (++unthrottled_frames >= 60)
        {
            vTaskDelay(1);
            unthrottled_frames = 0;
        }
        gpsp_profile_record_frame(
                (uint32_t)(run_end_us - run_start_us), 0);
#else
#if CONFIG_GPSP_AUDIO
        /* Dynamic rate control: pace frames up to 0.5% fast while the
           speaker queue is below its target, and slow while above. */
        float audio_balance = gpsp_board_audio_balance();
        next_frame_us += frame_period_us *
                (1.0 + GPSP_RATE_CONTROL * audio_balance);
#else
        next_frame_us += frame_period_us;
#endif
        now_us = esp_timer_get_time();
        wait_us = (int64_t)(next_frame_us - (double)now_us);

        /* Late frames are made up by running the next ones back to back,
           so sound keeps up; past the limit the lost time is dropped. With
           the speaker queue clearly above its target, there is nothing to
           make up: catching up would only overfill it. A queue past twice
           its target ends a catch-up at once to limit queue growth.
           Without working audio both read 0 and late frames are made up as
           before. */
        if (wait_us <= -(int64_t)(frame_period_us * GPSP_CATCH_UP_FRAMES)
#if GPSP_FRAMESKIP
                /* Fast forward runs as fast as it can; nothing to make up. */
                || (wait_us <= 0 && s_fast_forward_runs > 1)
#endif
#if CONFIG_GPSP_AUDIO
                || (wait_us <= 0 && (audio_balance > GPSP_CATCH_UP_SURPLUS ||
                        gpsp_board_audio_fill() >= 2.0f))
#endif
                )
        {
            next_frame_us = (double)now_us;
            wait_us = 1;
        }
        else if (wait_us <= 0)
        {
            wait_us = 1;
        }

        if (wait_us < (int64_t)portTICK_PERIOD_MS * 1000 &&
                now_us - last_idle_yield_us >= 1000000)
        {
            /* Short timer waits may never run CPU0's idle task. */
            vTaskDelay(1);
            last_idle_yield_us = esp_timer_get_time();
            wait_us = (int64_t)(next_frame_us -
                    (double)last_idle_yield_us);
            if (wait_us <= 0)
                wait_us = 1;
        }

        wait_start_us = esp_timer_get_time();
        result = esp_timer_start_once(frame_timer, (uint64_t)wait_us);

        if (result != ESP_OK)
            break;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t completed_wait_us =
                (uint32_t)(esp_timer_get_time() - wait_start_us);
        gpsp_profile_record_frame(
                (uint32_t)(run_end_us - run_start_us),
                completed_wait_us);
#if CONFIG_GPSP_BENCHMARK
        benchmark_wait_us = completed_wait_us;
#endif
#endif

#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
#if CONFIG_GPSP_BENCHMARK
        if (!fixed_frame && (s_frame_count & 63) == 0)
#else
        if ((s_frame_count & 63) == 0)
#endif
            gpsp_save_poll();
#endif

#if CONFIG_GPSP_BENCHMARK
        if (benchmark_frame <= GPSP_BENCHMARK_FRAME_COUNT)
        {
            unsigned index = (benchmark_frame - 1) /
                    GPSP_BENCHMARK_SLICE_FRAMES;

            gpsp_benchmark_slice_add(&s_benchmark_slices[index],
                    (uint32_t)(run_end_us - run_start_us),
                    benchmark_wait_us, &core_frame);
            if (benchmark_frame % GPSP_BENCHMARK_SLICE_FRAMES == 0 ||
                    benchmark_frame == GPSP_BENCHMARK_FRAME_COUNT)
                gpsp_benchmark_slice_capture(index);

            if (benchmark_frame == GPSP_BENCHMARK_FRAME_COUNT)
            {
                gpsp_benchmark_report_slices();
                gpsp_benchmark_report_images();
#if defined(GPSP_BENCHMARK_RAM_CODE)
                gpsp_rv32_smc_bench_report();
#endif
                gpsp_benchmark_free();
#if CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT
                struct gpsp_board_output_stats output;

                gpsp_board_output_stats(&output);
                ESP_LOGI(TAG, "BENCH_DONE frames=%u submitted=%u sent=%u dropped=%u",
                        benchmark_frame, output.submitted, output.sent,
                        output.dropped);
                if (frame_timer)
                    esp_timer_delete(frame_timer);
                gpsp_unload_game();
                retro_deinit();
                return ESP_OK;
#else
                memset(&s_benchmark_live_slice, 0, sizeof(s_benchmark_live_slice));
                gpsp_benchmark_get_engine(&s_benchmark_live_engines[0]);
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
                gpsp_benchmark_take_hot_sites(
                        &s_benchmark_live_engines[0].jit_hot);
                gpsp_benchmark_get_bios_probe(
                        &s_benchmark_live_bios_probes[0]);
#endif
#endif
            }
        }
#if !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT
        else
        {
            gpsp_benchmark_slice_add(&s_benchmark_live_slice,
                    (uint32_t)(run_end_us - run_start_us),
                    benchmark_wait_us, &core_frame);
            if ((benchmark_frame - GPSP_BENCHMARK_FRAME_COUNT) %
                    GPSP_BENCHMARK_SLICE_FRAMES == 0)
            {
                unsigned first = benchmark_frame -
                        GPSP_BENCHMARK_SLICE_FRAMES + 1;

                if (s_benchmark_pixels)
                    s_benchmark_live_slice.checksum = gpsp_frame_checksum(
                            s_benchmark_pixels, s_benchmark_width,
                            s_benchmark_height, s_benchmark_pitch);
                gpsp_benchmark_capture_output(&s_benchmark_live_slice);
                gpsp_benchmark_get_engine(&s_benchmark_live_engines[1]);
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
                gpsp_benchmark_take_hot_sites(
                        &s_benchmark_live_engines[1].jit_hot);
                gpsp_benchmark_get_bios_probe(
                        &s_benchmark_live_bios_probes[1]);
#endif
                gpsp_benchmark_report_slice(first, benchmark_frame,
                        &s_benchmark_live_slice
                        , &s_benchmark_live_engines[0],
                        &s_benchmark_live_engines[1]
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
                        , &s_benchmark_live_bios_probes[0],
                        &s_benchmark_live_bios_probes[1]
#endif
                        );
                s_benchmark_live_engines[0] = s_benchmark_live_engines[1];
#if CONFIG_GPSP_BENCHMARK_JIT_DETAIL
                s_benchmark_live_bios_probes[0] =
                        s_benchmark_live_bios_probes[1];
#endif
                memset(&s_benchmark_live_slice, 0,
                        sizeof(s_benchmark_live_slice));
            }
        }
#endif
#endif
    }

    if (quit)
        result = ESP_OK;
    else
        ESP_LOGE(TAG, "Failed to start frame timer: %s",
                esp_err_to_name(result));

    if (frame_timer)
        esp_timer_delete(frame_timer);
#if CONFIG_GPSP_BENCHMARK
    gpsp_benchmark_free();
#endif
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
    gpsp_save_stop();
#endif
    gpsp_unload_game();
    retro_deinit();
    return result;
}

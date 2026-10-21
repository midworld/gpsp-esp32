/* DSI framebuffer and PPA checks on hardware or a peripheral model.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "gpsp_lcd.h"
#include "gpsp_i2c.h"
#include "gpsp_ui.h"
#include "libretro.h"
/* Inspect output buffers without adding a firmware diagnostic API. */
#include "../../../components/gpsp_video/dsi.c"
#include "../../../components/gpsp_video/dsi_pad.c"

#if CONFIG_DSI_CHECK_MODEL
void display_model_fail_init(void);
unsigned display_model_ppa_scale(void);
unsigned display_model_ppa_source_pixels(void);
unsigned display_model_ppa_core(void);
unsigned display_model_ppa_calls(void);
unsigned display_model_blend_calls(void);
void display_model_fail_menu_alloc(unsigned allocation);
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
void display_model_dma_fail_after(unsigned requests);
void display_model_dma_check(void);
#endif
#endif
void display_settings_check(uint16_t *ui);
static uint32_t s_test_touch;
static bool s_test_toggle_touch;
bool gpsp_touch_available(void) { return true; }
uint32_t gpsp_touch_read(void)
{
    uint32_t state = s_test_touch;
    if (s_test_toggle_touch)
        s_test_touch ^= GPSP_VIRTUAL_PAD_VISIBLE;
    return state;
}
void gpsp_touch_set_enabled(bool enabled)
{
    s_test_touch = enabled ? GPSP_VIRTUAL_PAD_VISIBLE : 0;
}

static bool in_pad_region(unsigned x, unsigned y)
{
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
    {
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[i];
        if (x >= r->x && x < r->x + r->width &&
                y >= r->y && y < r->y + r->height)
            return true;
    }
    return false;
}

#if CONFIG_GPSP_LCD_DSI_SIMD
static uint16_t shade_reference(uint16_t p)
{
    unsigned r = ((p >> 11) * 3 + 2) / 4;
    unsigned g = (((p >> 5) & 63) * 3 + 2) / 4;
    unsigned b = ((p & 31) * 3 + 2) / 4;

    return (r << 11) | (g << 5) | b;
}

static void check_simd_shading(void)
{
    uint16_t input[160] __attribute__((aligned(16)));
    uint16_t output[3 * 480 + 8] __attribute__((aligned(16)));

    for (unsigned base = 0; base < 65536; base += 160)
    {
        for (unsigned i = 0; i < 160; i++)
            input[i] = (uint16_t)(base + i);
        for (unsigned gx = 0; gx <= 2; gx += 2)
            for (unsigned gy = 0; gy <= 2; gy += 2)
            {
                for (unsigned i = 0; i < 3 * 480 + 8; i++)
                    output[i] = 0xdead;
                gpsp_display_expand(output, input, 160, 1, 480, 3,
                        gx, gy, NULL);
                for (unsigned y = 0; y < 3; y++)
                    for (unsigned x = 0; x < 480; x++)
                    {
                        uint16_t expected = input[x / 3];
                        if (x % 3 == gx)
                            expected = shade_reference(expected);
                        if (y == gy)
                            expected = shade_reference(expected);
                        assert(output[y * 480 + x] == expected);
                    }
                for (unsigned i = 3 * 480; i < 3 * 480 + 8; i++)
                    assert(output[i] == 0xdead);
            }
    }
    puts("DSI_CHECK fused SIMD grid: all RGB565 colors and edges pass");
}
#endif

static bool ppa_done(ppa_client_handle_t client,
        ppa_event_data_t *event, void *arg)
{
    BaseType_t woken = pdFALSE;
    (void)client;
    (void)event;
    xSemaphoreGiveFromISR(arg, &woken);
    return woken == pdTRUE;
}

static void fill(uint16_t *pixels, unsigned width, unsigned height)
{
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            pixels[y * width + x] = colors[(y >= height / 2) * 2 +
                    (x >= width / 2)];
}

static uint16_t *last_frame(void)
{
    uint16_t *pixels = s_frames[(s_next + DSI_BUFFERS - 1) % DSI_BUFFERS];
    ESP_ERROR_CHECK(esp_cache_msync(pixels, DSI_BYTES,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C));
    return pixels;
}

static void check_frame(unsigned margin)
{
    uint16_t *pixels = last_frame();
    for (unsigned y = 0; y < DSI_HEIGHT; y++)
        if (y < margin || y >= DSI_HEIGHT - margin)
            for (unsigned x = 0; x < DSI_WIDTH; x++)
                assert(pixels[y * DSI_WIDTH + x] == 0);
#if CONFIG_GPSP_LCD_ROTATE_180
    assert(pixels[200 * DSI_WIDTH + 100] == 0x001f);
    assert(pixels[200 * DSI_WIDTH + 380] == 0xf800);
    assert(pixels[600 * DSI_WIDTH + 100] == 0xffff);
    assert(pixels[600 * DSI_WIDTH + 380] == 0x07e0);
#else
    assert(pixels[200 * DSI_WIDTH + 100] == 0x07e0);
    assert(pixels[200 * DSI_WIDTH + 380] == 0xffff);
    assert(pixels[600 * DSI_WIDTH + 100] == 0xf800);
    assert(pixels[600 * DSI_WIDTH + 380] == 0x001f);
#endif
}

static uint16_t landscape_pixel(const uint16_t *pixels, unsigned x, unsigned y)
{
#if CONFIG_GPSP_LCD_ROTATE_180
    return pixels[x * DSI_WIDTH + DSI_WIDTH - 1 - y];
#else
    return pixels[(DSI_HEIGHT - 1 - x) * DSI_WIDTH + y];
#endif
}

static int compare_time(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}

static void pad_background(uint16_t *frame, unsigned border, unsigned seed,
        bool clear)
{
    if (clear)
        memset(frame, 0, DSI_BYTES);
    for (unsigned y = border; y < DSI_HEIGHT - border; y++)
        for (unsigned x = 0; x < DSI_WIDTH; x++)
            frame[y * DSI_WIDTH + x] =
                    (uint16_t)((y * DSI_WIDTH + x) * 25173u + seed * 13849u);
    ESP_ERROR_CHECK(esp_cache_msync(frame, DSI_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
}

static void check_pad_crops(void)
{
    uint16_t *expected = heap_caps_aligned_alloc(64, DSI_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    uint16_t *actual = heap_caps_aligned_alloc(64, DSI_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    const unsigned masks[] = {0, BUTTON(LEFT), BUTTON(RIGHT),
        BUTTON(UP) | BUTTON(LEFT), BUTTON(DOWN) | BUTTON(RIGHT),
        BUTTON(A) | BUTTON(B), BUTTON(L) | BUTTON(R),
        0xffffu | GPSP_VIRTUAL_PAD_HOME};
    unsigned checked = 0;

    assert(expected && actual);
    for (unsigned opacity = 25; opacity <= 100; opacity *= 2)
        for (unsigned border = 40; border <= 80; border += 40)
            for (unsigned state = 0; state < sizeof(masks) / sizeof(masks[0]); state++)
                for (unsigned repeat = 0; repeat < 2; repeat++)
                {
                    unsigned seed = state * 4 + repeat;

                    pad_background(expected, border, seed, true);
                    pad_background(actual, border, seed, !repeat);
                    ESP_ERROR_CHECK(gpsp_dsi_pad_draw(actual,
                            masks[state] | GPSP_VIRTUAL_PAD_VISIBLE, opacity,
                            repeat ? border : 0,
                            repeat ? DSI_HEIGHT - border : DSI_HEIGHT));
                    /* The reference blends the original seven whole rectangles. */
                    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
                    {
                        const struct gpsp_pad_rect *r = &gpsp_pad_rects[i];
#if CONFIG_GPSP_LCD_ROTATE_180
                        unsigned x = DSI_WIDTH - r->y - r->height, y = r->x;
#else
                        unsigned x = r->y, y = DSI_HEIGHT - r->x - r->width;
#endif
                        ESP_ERROR_CHECK(gpsp_dsi_blend_overlay_alpha(expected,
                                s_tiles[i], r->height, r->width, x, y, opacity));
                    }
                    assert(memcmp(expected, actual, DSI_BYTES) == 0);
                    checked++;
                    vTaskDelay(1);
                }
    heap_caps_free(expected);
    heap_caps_free(actual);
    printf("DSI_CHECK pad crops: %u exact frames, all opacities and game/UI borders pass\n",
            checked);
}

static void check_virtual_pad(uint16_t *input, uint16_t *ui)
{
    uint16_t *baseline = heap_caps_malloc(DSI_BYTES, MALLOC_CAP_SPIRAM);
    uint16_t *steady = heap_caps_malloc(DSI_BYTES, MALLOC_CAP_SPIRAM);
    assert(baseline && steady);
    for (unsigned i = 0; i < 320 * 240; i++)
        ui[i] = (uint16_t)(i * 7919);
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_OFF);
    gpsp_dsi_set_options(GPSP_DISPLAY_NEAREST, GPSP_DISPLAY_COLOR_NONE);
    for (unsigned mode = 0; mode < 2; mode++)
    {
        bool is_ui = mode != 0;
        const uint16_t *source = is_ui ? ui : input;
        s_test_touch = 0;
        ESP_ERROR_CHECK(gpsp_dsi_draw(source, is_ui));
        memcpy(baseline, last_frame(), DSI_BYTES);
        s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE;
        const uint32_t masks[] = {0,
            (1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_A),
            (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) | (1u << RETRO_DEVICE_ID_JOYPAD_B)};
        for (unsigned held = 0; held < 3; held++)
        {
            s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE | masks[held];
            ESP_ERROR_CHECK(gpsp_dsi_draw(source, is_ui));
            const uint16_t *shown = last_frame();
            memcpy(steady, shown, DSI_BYTES);
            assert(landscape_pixel(shown, 0, 346) != 0);
            assert(landscape_pixel(shown, 799, 306) != 0);
            for (unsigned y = 0; y < 480; y++)
                for (unsigned x = 0; x < 800; x++)
                    if (!in_pad_region(x, y))
                        assert(landscape_pixel(shown, x, y) ==
                                landscape_pixel(baseline, x, y));
            for (unsigned repeat = 0; repeat < DSI_BUFFERS * 2; repeat++)
            {
                ESP_ERROR_CHECK(gpsp_dsi_draw(source, is_ui));
                assert(memcmp(steady, last_frame(), DSI_BYTES) == 0);
            }
        }
        s_test_touch = 0;
        for (unsigned i = 0; i < DSI_BUFFERS + 1; i++)
        {
            ESP_ERROR_CHECK(gpsp_dsi_draw(source, is_ui));
            assert(memcmp(baseline, last_frame(), DSI_BYTES) == 0);
        }
    }
    /* Direct frame checks run without the UI worker's asynchronous redraws. */
    gpsp_lcd_ui_end();
    const unsigned opacities[] = {25, 50, 100};
    unsigned levels[3];
    s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE;
    for (unsigned i = 0; i < 3; i++)
    {
        gpsp_dsi_set_pad_opacity(opacities[i]);
        ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
        memcpy(steady, last_frame(), DSI_BYTES);
        levels[i] = landscape_pixel(last_frame(), 0, 346);
        assert(landscape_pixel(last_frame(), 0, 0) != 0);
        assert(landscape_pixel(last_frame(), 799, 0) != 0);
        for (unsigned repeat = 0; repeat < DSI_BUFFERS * 2; repeat++)
        {
            ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
            assert(memcmp(steady, last_frame(), DSI_BYTES) == 0);
        }
    }
    assert(levels[0] && levels[0] < levels[1] && levels[1] < levels[2]);
    uint16_t top = landscape_pixel(last_frame(), 100, 0);
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_FULL);
    ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
    assert(landscape_pixel(last_frame(), 100, 0) < top);
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_OFF);
    gpsp_dsi_set_pad_opacity(37);
    assert(gpsp_dsi_pad_opacity() == 100);
    gpsp_dsi_set_pad_opacity(25);
    s_test_touch = 0;
    const uint16_t *held_frame;
    assert(gpsp_lcd_ui_begin(&ui, &held_frame));
    /* A static menu still refreshes controls without editing its canvas. */
    gpsp_lcd_ui_draw(0, 240);
    memcpy(baseline, last_frame(), DSI_BYTES);
    for (unsigned enabled = 1; ; enabled--)
    {
        unsigned next = s_next;
        s_test_touch = enabled ? GPSP_VIRTUAL_PAD_VISIBLE : 0;
        gpsp_lcd_overlay_changed();
        for (unsigned wait = 0; s_next == next && wait < 1000; wait++)
            vTaskDelay(1);
        assert(s_next != next);
        if (!enabled)
        {
            assert(memcmp(baseline, last_frame(), DSI_BYTES) == 0);
            break;
        }
        assert(landscape_pixel(last_frame(), 0, 346) != 0);
    }
    unsigned idle[100], changing[100];
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < 110; i++)
        {
            unsigned state = GPSP_VIRTUAL_PAD_VISIBLE;
            if (pass && (i & 1))
                state |= (1u << RETRO_DEVICE_ID_JOYPAD_UP) |
                        (1u << RETRO_DEVICE_ID_JOYPAD_A);
            int64_t start = esp_timer_get_time();
            ESP_ERROR_CHECK(gpsp_dsi_pad_draw(s_frames[s_next], state,
                    gpsp_dsi_pad_opacity(), 0, DSI_HEIGHT));
            if (i >= 10)
                (pass ? changing : idle)[i - 10] = esp_timer_get_time() - start;
        }
    qsort(idle, 100, sizeof(unsigned), compare_time);
    /* The timing probe also wrote controls into the next scanout buffer. */
    s_pad_borders[s_next] = true;
    qsort(changing, 100, sizeof(unsigned), compare_time);
    printf("DSI_CHECK PAD us idle p50=%u p95=%u p99=%u max=%u "
            "changing p50=%u p95=%u p99=%u max=%u\n", idle[49], idle[94],
            idle[98], idle[99], changing[49], changing[94], changing[98], changing[99]);
    heap_caps_free(baseline);
    heap_caps_free(steady);
    puts("DSI_CHECK virtual pad: game/UI bounds, border cleanup, idle UI refresh pass");
    puts("DSI_CHECK pad opacity: 25/50/100, top edges, Stats on top pass");
}

#if CONFIG_DSI_CHECK_MODEL
static void check_menu_allocations(uint16_t **ui)
{
    const uint16_t *held;
    for (unsigned allocation = 1; allocation <= 4; allocation++)
    {
        size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        display_model_fail_menu_alloc(allocation);
        s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE;
        gpsp_ui_backdrop(*ui, NULL);
        gpsp_ui_panel(*ui);
        gpsp_lcd_ui_draw(0, 240);
        assert(!s_test_touch && !gpsp_lcd_ui_layer(*ui, NULL));
        assert(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == free);
        unsigned next = s_next;
        s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE;
        gpsp_lcd_overlay_changed();
        for (unsigned wait = 0; s_next == next && wait < 1000; wait++)
            vTaskDelay(1);
        assert(s_next != next && !s_test_touch);
        assert(landscape_pixel(last_frame(), 0, 346) == 0);
        gpsp_lcd_ui_end();
        assert(gpsp_lcd_ui_begin(ui, &held));
    }
    puts("DSI_CHECK menu allocation failures: four stages, heap recovery, pad suppression pass");
}
#endif

static void check_menu_layers(uint16_t *ui)
{
    uint16_t *below = heap_caps_malloc(DSI_BYTES, MALLOC_CAP_SPIRAM);
    assert(below);
    gpsp_dsi_set_pad_opacity(100);
    gpsp_ui_backdrop(ui, NULL);
    s_test_touch = GPSP_VIRTUAL_PAD_VISIBLE;
    gpsp_lcd_ui_draw(0, 240);
    memcpy(below, last_frame(), DSI_BYTES);
    gpsp_ui_panel(ui);
    gpsp_ui_tint(ui, 24, 150, 80, 16, GPSP_UI_COLOR_CURSOR);
    gpsp_ui_fill(ui, 270, 190, 12, 12, 0xf800);
    gpsp_ui_text_clip(ui, 26, 160, "Overlay", 3, 80, 0xffff);
    gpsp_lcd_ui_draw(0, 240);
    const uint16_t *shown = last_frame();
    const uint32_t *layer = gpsp_lcd_ui_layer(ui, NULL);
    assert(layer);
    unsigned through = 0, opaque = 0;
    for (unsigned y = 0; y < 480; y++)
        for (unsigned x = 80; x < 720; x++)
        {
            uint16_t b = landscape_pixel(below, x, y);
            uint16_t actual = landscape_pixel(shown, x, y);
            uint32_t fg = layer[(y / 2) * 320 + (x - 80) / 2];
            unsigned alpha = fg >> 24;
            const unsigned shifts[] = {11, 5, 0}, maxima[] = {31, 63, 31};
            for (unsigned channel = 0; channel < 3; channel++)
            {
                unsigned max = maxima[channel], shift = shifts[channel];
                unsigned bg = ((b >> shift) & max) * 255 / max;
                unsigned f = fg >> (16 - channel * 8) & 255;
                unsigned expected = ((f * alpha + bg * (255 - alpha) + 127) / 255 *
                        max + 127) / 255;
                assert(abs((int)((actual >> shift) & max) - (int)expected) <= 1);
            }
            if (alpha == 128 && b)
                through += actual != 0 && actual != b;
            if (alpha == 255)
                opaque++;
        }
    assert(through > 1000 && opaque > 1000);
    assert(landscape_pixel(shown, 626, 396) == 0xf800);
    /* A visibility change during a draw cannot put controls above opaque UI. */
    s_test_touch = 0;
    s_test_toggle_touch = true;
    ESP_ERROR_CHECK(gpsp_dsi_ui_draw(ui, NULL));
    s_test_toggle_touch = false;
    assert(landscape_pixel(last_frame(), 626, 396) == 0xf800);
    assert(landscape_pixel(last_frame(), 0, 346) == 0);
    /* Off/on refreshes reuse both the menu layer and the fixed pad geometry. */
    s_test_touch = 0;
    gpsp_lcd_ui_draw(0, 240);
    memcpy(below, last_frame(), DSI_BYTES);
    memset(ui, 0xff, 320 * 240 * sizeof(uint16_t));
    for (unsigned enabled = 1; ; enabled--)
    {
        unsigned next = s_next;
        s_test_touch = enabled ? GPSP_VIRTUAL_PAD_VISIBLE : 0;
        gpsp_lcd_overlay_changed();
        for (unsigned wait = 0; s_next == next && wait < 1000; wait++)
            vTaskDelay(1);
        assert(s_next != next);
        if (!enabled)
        {
            assert(memcmp(below, last_frame(), DSI_BYTES) == 0);
            break;
        }
        assert(landscape_pixel(last_frame(), 626, 396) == 0xf800);
        assert(landscape_pixel(last_frame(), 0, 346) != 0);
    }
    heap_caps_free(below);
    gpsp_lcd_ui_end();
    const uint16_t *held;
    assert(gpsp_lcd_ui_begin(&ui, &held));
    gpsp_dsi_set_pad_opacity(25);
    puts("DSI_CHECK menu layers: fixed pad, translucent panel, opaque text, cached redraw pass");
}

static void check_stats(uint16_t *input, uint16_t *ui)
{
    const struct lcd_status_value values[LCD_FIELD_COUNT] = {
        [LCD_FIELD_FPS] = {"59.7", 0x07e0},
        [LCD_FIELD_SPEED] = {"100%", 0xf800},
    };
    uint16_t *full = heap_caps_malloc(DSI_BYTES, MALLOC_CAP_SPIRAM);

    assert(full);
    for (unsigned i = 0; i < 240 * 160; i++)
        input[i] = 0xffff;
    gpsp_dsi_set_options(GPSP_DISPLAY_NEAREST, GPSP_DISPLAY_COLOR_NONE);
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_FULL);
    gpsp_dsi_hud_update(values);
#if CONFIG_DSI_CHECK_MODEL
    unsigned calls = display_model_blend_calls();
#endif
    ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
    memcpy(full, last_frame(), DSI_BYTES);
#if CONFIG_DSI_CHECK_MODEL
    assert(display_model_blend_calls() == calls + 2);
#endif
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_SIMPLE);
    /* Change geometry immediately, without waiting for another status update. */
    ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
    const uint16_t *simple = last_frame();
    unsigned fps = 0, speed = 0, background = 0;

    for (unsigned y = 0; y < 480; y++)
        for (unsigned x = 0; x < 800; x++)
        {
            uint16_t base = x >= 40 && x < 760 ? 0xffff : 0;
            bool box = x >= 508 && x < 760 && y < 26;
            uint16_t pixel = landscape_pixel(simple, x, y);

            assert(pixel == (box ? landscape_pixel(full, x, y) : base));
            if (box)
            {
                fps += pixel == 0x07e0;
                speed += pixel == 0xf800;
                background += pixel != base && pixel != 0x8410 &&
                        pixel != 0x07e0 && pixel != 0xf800;
            }
            if (x < 40 || x >= 760 || (y >= 26 && y < 454))
                assert(landscape_pixel(full, x, y) == base);
        }
    assert(fps && speed && background);
#if CONFIG_DSI_CHECK_MODEL
    assert(display_model_blend_calls() == calls + 3);
#endif
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_FULL);
    ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
    assert(!memcmp(full, last_frame(), DSI_BYTES));
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_OFF);
#if CONFIG_DSI_CHECK_MODEL
    calls = display_model_blend_calls();
#endif
    ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
    const uint16_t *off = last_frame();

    for (unsigned y = 0; y < 480; y++)
        for (unsigned x = 0; x < 800; x++)
            assert(landscape_pixel(off, x, y) ==
                    (x >= 40 && x < 760 ? 0xffff : 0));
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_FULL);
    for (unsigned i = 0; i < 320 * 240; i++)
        ui[i] = 0xffff;
    gpsp_lcd_ui_draw(0, 240);
    const uint16_t *menu = last_frame();

    for (unsigned y = 0; y < 480; y++)
        for (unsigned x = 0; x < 800; x++)
            assert(landscape_pixel(menu, x, y) ==
                    (x >= 80 && x < 720 ? 0xffff : 0));
#if CONFIG_DSI_CHECK_MODEL
    assert(display_model_blend_calls() == calls);
#endif
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_OFF);
    gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_COUNT);
    assert(gpsp_dsi_stats_mode() == GPSP_DISPLAY_STATS_OFF);
    heap_caps_free(full);
    puts("DSI_CHECK Stats: Off/Simple/Full pixels, bounds, immediate switch, UI pass");
}

static void check_color_lut(void)
{
    const uint16_t samples[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f,
        0x8410, 0xffe0, 0x07ff, 0xf81f};
    const double matrix[3][3] = {
        {0.905, 0.195, -0.10}, {0.10, 0.65, 0.25}, {0.1575, 0.1425, 0.70},
    };
    const unsigned shifts[] = {11, 5, 0};
    const unsigned maxima[] = {31, 63, 31};

    assert(s_color_lut[0] == 0 && s_color_lut[0xffff] != 0xffff);
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        for (unsigned channel = 0; channel < 3; channel++)
        {
            double linear = 0;
            for (unsigned c = 0; c < 3; c++)
            {
                double value = (samples[i] >> shifts[c]) & maxima[c];
                linear += matrix[channel][c] * pow(value / maxima[c], 2.2);
            }
            linear = fmin(1, fmax(0, linear * 0.91));
            int expected = (int)round(pow(linear, 1.0 / 2.2) * maxima[channel]);
            int actual = (s_color_lut[samples[i]] >> shifts[channel]) & maxima[channel];
            assert(abs(actual - expected) <= 1);
        }
}

static void check_effects(uint16_t *input, uint16_t *ui)
{
    check_color_lut();
    /* Vary every source pixel to catch lane order and replication errors. */
    for (unsigned y = 0; y < 160; y++)
        for (unsigned x = 0; x < 240; x++)
            input[y * 240 + x] = (uint16_t)(x * 577 + y * 19);
    for (unsigned color = 0; color < GPSP_DISPLAY_COLOR_COUNT; color++)
        for (unsigned grid = 0; grid < 2; grid++)
        {
            gpsp_dsi_set_options(grid ? GPSP_DISPLAY_LCD_GRID :
                    GPSP_DISPLAY_NEAREST, color);
#if CONFIG_DSI_CHECK_MODEL && CONFIG_GPSP_LCD_DSI_CPU_ROTATE
            unsigned calls = display_model_ppa_calls();
#endif
            ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
#if CONFIG_DSI_CHECK_MODEL
#if CONFIG_GPSP_LCD_DSI_CPU_ROTATE
            assert(display_model_ppa_calls() == calls);
#else
            assert(display_model_ppa_scale() == 1);
            assert(display_model_ppa_source_pixels() == 240 * 160);
#endif
#endif
            const uint16_t *pixels = last_frame();
            for (unsigned y = 0; y < 480; y++)
                for (unsigned x = 0; x < 800; x++)
                {
                    uint16_t actual = landscape_pixel(pixels, x, y);
                    if (x < 40 || x >= 760)
                    {
                        assert(actual == 0);
                        continue;
                    }
                    unsigned px = x - 40;
                    uint16_t expected = input[(y / 3) * 240 + px / 3];
                    if (color)
                        expected = s_color_lut[expected];
                    if (grid)
                    {
                        unsigned edges = (px % 3 == 2) + (y % 3 == 2);
                        unsigned r = expected >> 11;
                        unsigned g = (expected >> 5) & 63;
                        unsigned b = expected & 31;
                        while (edges--)
                        {
                            r = (3 * r + 2) / 4;
                            g = (3 * g + 2) / 4;
                            b = (3 * b + 2) / 4;
                        }
                        expected = (r << 11) | (g << 5) | b;
                    }
                    assert(actual == expected);
                }
        }

    /* Bilinear keeps the source-sized color pass for PPA to scale. */
    fill(input, 240, 160);
    for (unsigned color = 0; color < GPSP_DISPLAY_COLOR_COUNT; color++)
    {
        gpsp_dsi_set_options(GPSP_DISPLAY_BILINEAR, color);
        ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
#if CONFIG_DSI_CHECK_MODEL
        assert(display_model_ppa_scale() == 3);
#endif
        const uint16_t *pixels = last_frame();
        assert(landscape_pixel(pixels, 100, 100) ==
                (color ? s_color_lut[0xf800] : 0xf800));
    }
    /* UI always uses nearest 2x and unchanged colors in every game mode. */
    for (unsigned i = 0; i < 320 * 240; i++)
        ui[i] = (uint16_t)(i * 37);
    for (unsigned filter = 0; filter < GPSP_DISPLAY_FILTER_COUNT; filter++)
    {
        gpsp_dsi_set_options(filter, GPSP_DISPLAY_COLOR_GBA);
        gpsp_lcd_ui_draw(120, 121);
#if CONFIG_DSI_CHECK_MODEL
        assert(display_model_ppa_scale() == 1);
        assert(display_model_ppa_source_pixels() == 320 * 240);
#if !CONFIG_FREERTOS_UNICORE
        assert(display_model_ppa_core() == 1);
#endif
#endif
        const uint16_t *pixels = last_frame();
        for (unsigned y = 0; y < 480; y++)
            for (unsigned x = 0; x < 800; x++)
                assert(landscape_pixel(pixels, x, y) == (x < 80 || x >= 720 ?
                        0 : ui[(y / 2) * 320 + (x - 80) / 2]));
    }
    gpsp_dsi_set_options(GPSP_DISPLAY_NEAREST, GPSP_DISPLAY_COLOR_NONE);
    puts("DSI_CHECK display effects pass");
}

#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#if !CONFIG_DSI_CHECK_MODEL
static void check_tile_timing(void)
{
    assert(s_copy && s_tile);
    uint16_t *frame = (uint16_t *)s_frames[s_next] + 40 * DSI_WIDTH;

    for (unsigned grid = 0; grid < 2; grid++)
        for (unsigned dma = 0; dma < 2; dma++)
        {
            int64_t total = 0;
            int gx = grid ? 2 : -1, gy = grid ? 0 : -1;

            for (unsigned i = 0; i < 20; i++)
            {
                int64_t start = esp_timer_get_time();
                if (dma)
                    ESP_ERROR_CHECK(dsi_expand_tiles(frame, gx, gy, NULL, NULL, NULL));
                else
                    gpsp_display_expand(frame, s_work, 160, 240, DSI_WIDTH,
                            3, gx, gy, NULL);
                total += esp_timer_get_time() - start;
                vTaskDelay(1);
            }
            printf("DSI_CHECK scaling grid=%u dma=%u avg_us=%u\n",
                    grid, dma, (unsigned)(total / 20));
        }
}
#endif

#if CONFIG_DSI_CHECK_MODEL
static void check_copy_failure(uint16_t *input)
{
    display_model_dma_check();
    for (unsigned requests = 2; requests <= 3; requests++)
    {
        display_model_dma_fail_after(requests);
        ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
        assert(!s_copy && !s_tile);
        check_frame(40);
        dsi_copy_init();
        assert(s_copy && s_tile);
    }
    puts("DSI_CHECK queued DMA ownership and failure drain pass");
}
#endif
#endif

static void check_render_buffer_release(void)
{
    for (unsigned game = 0; game < 8; game++)
    {
        uint16_t *shown = gpsp_lcd_next_buffer();
        uint16_t *unused = gpsp_lcd_next_buffer();
        uint16_t *ui;
        const uint16_t *frame;
        assert(shown && unused && shown != unused);
        fill(shown, 240, 160);
        assert(gpsp_lcd_present(shown, 240, 160, 240 * 2));
        assert(gpsp_lcd_ui_begin(&ui, &frame));
        assert(frame == shown);
        /* Unload returns the next frame that the old game never submitted. */
        gpsp_lcd_release_buffer(unused);
        gpsp_lcd_release_buffer(shown);
        uint16_t *a = gpsp_lcd_next_buffer();
        uint16_t *b = gpsp_lcd_next_buffer();
        assert(a && b && a != b && a != frame && b != frame);
        assert(!gpsp_lcd_next_buffer());
        gpsp_lcd_release_buffer(a);
        gpsp_lcd_release_buffer(b);
        gpsp_lcd_ui_end();
    }
    puts("DSI_CHECK game switch render buffers pass");
}

static void display_check(void *arg)
{
    (void)arg;
    printf("DSI_CHECK CPU%u\n", (unsigned)xPortGetCoreID());
#if CONFIG_GPSP_LCD_DSI_SIMD
    check_simd_shading();
#endif
#if CONFIG_DSI_CHECK_MODEL
    puts("DSI_CHECK test-only peripheral model");
#endif
    /* Check PPA independently, before probing the panel control bus. */
    const ppa_client_config_t client_config = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_handle_t client;
    uint16_t *input = heap_caps_aligned_alloc(64, 240 * 160 * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    void *output = heap_caps_aligned_alloc(64, DSI_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(input && output);
    fill(input, 240, 160);
    memset(output, 0, DSI_BYTES);
    ESP_ERROR_CHECK(esp_cache_msync(output, DSI_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    ESP_ERROR_CHECK(ppa_register_client(&client_config, &client));
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    assert(done);
    const ppa_event_callbacks_t callbacks = {.on_trans_done = ppa_done};
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(client, &callbacks));
    const ppa_srm_oper_config_t operation = {
        .in = {.buffer = input, .pic_w = 240, .pic_h = 160,
            .block_w = 240, .block_h = 160, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = output, .buffer_size = DSI_BYTES,
            .pic_w = DSI_WIDTH, .pic_h = DSI_HEIGHT, .block_offset_y = 40,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 3, .scale_y = 3, .mode = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = done,
    };
    puts("DSI_CHECK PPA begin");
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(client, &operation));
    if (!xSemaphoreTake(done, pdMS_TO_TICKS(1000)))
    {
        puts("DSI_CHECK FAIL: PPA completion timed out");
        abort();
    }
    ESP_ERROR_CHECK(esp_cache_msync(output, DSI_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
    assert(((uint16_t *)output)[200 * DSI_WIDTH + 100] == 0x07e0);
    ESP_ERROR_CHECK(ppa_unregister_client(client));
    vSemaphoreDelete(done);
    heap_caps_free(output);
    puts("DSI_CHECK PPA pass");

    i2c_master_bus_handle_t shared_bus;
    ESP_ERROR_CHECK(gpsp_i2c_acquire(&shared_bus));
    ESP_ERROR_CHECK(gpsp_lcd_init());
    check_render_buffer_release();
    uint16_t *claimed = gpsp_lcd_next_buffer();
    assert(claimed);
    fill(claimed, 240, 160);
    unsigned next = s_next;
    assert(gpsp_lcd_present(claimed, 240, 160, 240 * 2));
    for (unsigned wait = 0; s_next == next && wait < 1000; wait++)
        vTaskDelay(1);
    assert(s_next != next);
#if CONFIG_DSI_CHECK_MODEL && !CONFIG_FREERTOS_UNICORE
    assert(display_model_ppa_core() == 1);
#endif
    /* Stop the worker at the public UI boundary before inspecting buffers. */
    uint16_t *ui;
    const uint16_t *frame;
    assert(gpsp_lcd_ui_begin(&ui, &frame));
    assert(frame == claimed);
    assert(memcmp(frame, input, 240 * 160 * 2) == 0);
    check_frame(40);
    for (unsigned i = 0; i < 9; i++)
    {
        ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
        check_frame(40);
    }
    fill(ui, 320, 240);
    for (unsigned i = 0; i < 9; i++)
    {
        gpsp_lcd_ui_draw(100, 101);
        check_frame(80);
    }
    for (unsigned i = 0; i < 9; i++)
    {
        ESP_ERROR_CHECK(gpsp_dsi_draw(input, false));
        check_frame(40);
    }
    check_effects(input, ui);
    check_stats(input, ui);
    check_pad_crops();
    check_virtual_pad(input, ui);
#if CONFIG_DSI_CHECK_MODEL
    check_menu_allocations(&ui);
#endif
    check_menu_layers(ui);
    fill(input, 240, 160);
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#if CONFIG_DSI_CHECK_MODEL
    check_copy_failure(input);
#else
    check_tile_timing();
#endif
#endif
    display_settings_check(ui);
    gpsp_lcd_ui_end();
    const uint16_t *blank = s_frames[(s_next + DSI_BUFFERS - 1) % DSI_BUFFERS];
    for (unsigned i = 0; i < DSI_WIDTH * DSI_HEIGHT; i++)
        assert(blank[i] == 0);
    gpsp_lcd_shutdown();
#if CONFIG_DSI_CHECK_MODEL
    display_model_fail_init();
    assert(gpsp_lcd_init() == ESP_FAIL);
#endif
    ESP_ERROR_CHECK(gpsp_lcd_init());
    gpsp_lcd_shutdown();
    i2c_master_bus_handle_t same_bus;
    ESP_ERROR_CHECK(gpsp_i2c_acquire(&same_bus));
    assert(same_bus == shared_bus);
    gpsp_i2c_release();
    gpsp_i2c_release();
    heap_caps_free(input);
    puts("DSI_CHECK PASS");
    vTaskDelete(NULL);
}

void app_main(void)
{
    assert(xTaskCreatePinnedToCore(display_check, "dsi_check", 8192, NULL,
            4, NULL, 1) == pdPASS);
}

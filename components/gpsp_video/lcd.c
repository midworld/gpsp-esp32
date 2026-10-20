/* Shared LCD worker and UI output.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_lcd.h"
#include "gpsp_spi.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_LCD

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gpsp_font.h"
#include "gpsp_screen.h"
#include "lcd_status.h"
#if CONFIG_GPSP_LCD_DSI
#include "gpsp_lcd_dsi.h"
#include "dsi_hud.h"
#endif

#if CONFIG_GPSP_LCD_DSI
#define LCD_WIDTH 800
#define LCD_HEIGHT 480
#else
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#endif
#define LCD_FRAME_WIDTH 240
#define LCD_FRAME_HEIGHT 160
#define LCD_FRAME_BYTES (LCD_FRAME_WIDTH * LCD_FRAME_HEIGHT * 2)
/* Three buffers: one rendering, one ready and one on the SPI. The core
   renders straight into them, one row past the visible ones included. */
#define LCD_BUFFERS 3
#define LCD_BUFFER_BYTES ((GPSP_SCREEN_BUFFER_BYTES + 63) & ~63u)
#define LCD_OFFSET_X ((LCD_WIDTH - LCD_FRAME_WIDTH) / 2)
#define LCD_OFFSET_Y ((LCD_HEIGHT - LCD_FRAME_HEIGHT) / 2)

_Static_assert(LCD_WIDTH >= LCD_FRAME_WIDTH &&
        LCD_HEIGHT >= LCD_FRAME_HEIGHT, "The panel must fit a GBA frame");

/* Status uses the shared 7x13 font above and below the game frame.
   Its 280-pixel area is centered like the menu box. */
#define LCD_STATUS_WIDTH 280
#define LCD_STATUS_LEFT ((LCD_WIDTH - LCD_STATUS_WIDTH) / 2 + 4)
#define LCD_STATUS_RIGHT ((LCD_WIDTH + LCD_STATUS_WIDTH) / 2 - 4)
#define LCD_STATUS_BAND (GPSP_FONT_HEIGHT + 4)
#if CONFIG_GPSP_LCD_DSI
#define LCD_STATUS 0
#else
#define LCD_STATUS (LCD_OFFSET_Y >= LCD_STATUS_BAND && \
        LCD_WIDTH >= LCD_STATUS_WIDTH)
#endif
#define LCD_STATUS_TOP (LCD_OFFSET_Y - LCD_STATUS_BAND + 2)
#define LCD_STATUS_BOTTOM (LCD_OFFSET_Y + LCD_FRAME_HEIGHT + 2)
/* Columns from the left edge, and back from the right edge. */
#define LCD_COLUMN(column) (LCD_STATUS_LEFT + (column) * GPSP_FONT_WIDTH)
#define LCD_RIGHT_COLUMN(column) \
    (LCD_STATUS_RIGHT - (column) * GPSP_FONT_WIDTH)

/* Nominal GBA frame rate in tenths. */
#define LCD_FPS_TENTHS 597

/* Little-endian RGB565. Normal values are light gray; warnings and
   critical values stand out. */
#define LCD_COLOR_GOOD 0xd69a
#define LCD_COLOR_WARN 0xffe0
#define LCD_COLOR_BAD 0xf800

/* CPU1 when it exists: CPU0 runs the emulator. */
#if CONFIG_FREERTOS_UNICORE
#define LCD_TASK_CORE tskNO_AFFINITY
#else
#define LCD_TASK_CORE 1
#endif

enum lcd_buffer_state
{
    LCD_BUFFER_FREE,
    LCD_BUFFER_WRITING,
    LCD_BUFFER_READY,
    LCD_BUFFER_SENDING,
    /* The UI's backdrop: kept until the UI ends. */
    LCD_BUFFER_HELD,
};

static const char *TAG = "gpsp_lcd";
static uint16_t *s_buffers[LCD_BUFFERS];
static enum lcd_buffer_state s_state[LCD_BUFFERS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static volatile bool s_stop;
static uint16_t *s_text_pixels;
/* Game frames that reached the panel. */
static volatile uint32_t s_sent;
/* Configured frameskip for FPS colors: 0 is off, negative is auto. */
static volatile int s_frameskip;
/* Serializes display writes and UI ownership changes. */
static SemaphoreHandle_t s_draw_lock;
/* Last buffer sent to the panel, or -1. */
static int s_last = -1;
#if CONFIG_GPSP_LCD_DSI
static atomic_uint s_overlay_revision;
#endif
#if CONFIG_GPSP_MENU
#if CONFIG_GPSP_LCD_DSI
static SemaphoreHandle_t s_ui_done;
/* Protected by s_lock; UI calls wait until the CPU1 worker finishes. */
static bool s_ui_pending;
static uint16_t *s_ui_cached;
static bool s_ui_cached_valid;
static unsigned s_overlay_drawn;
#endif
/* The UI canvas is centered on the panel; a smaller panel crops it. */
#define LCD_UI_WIDTH 320
#define LCD_UI_HEIGHT 240
#define LCD_UI_BYTES (LCD_UI_WIDTH * LCD_UI_HEIGHT * 2)
#define LCD_UI_SHOWN_WIDTH (LCD_WIDTH < LCD_UI_WIDTH ? LCD_WIDTH : LCD_UI_WIDTH)
#define LCD_UI_SHOWN_HEIGHT \
    (LCD_HEIGHT < LCD_UI_HEIGHT ? LCD_HEIGHT : LCD_UI_HEIGHT)
/* Panel position of the shown part, and its place in the canvas. */
#define LCD_UI_X ((LCD_WIDTH - LCD_UI_SHOWN_WIDTH) / 2)
#define LCD_UI_Y ((LCD_HEIGHT - LCD_UI_SHOWN_HEIGHT) / 2)
#define LCD_UI_CROP_X ((LCD_UI_WIDTH - LCD_UI_SHOWN_WIDTH) / 2)
#define LCD_UI_CROP_Y ((LCD_UI_HEIGHT - LCD_UI_SHOWN_HEIGHT) / 2)
/* Rows per transfer: the bus takes at most LCD_FRAME_BYTES. */
#define LCD_UI_ROWS (LCD_FRAME_BYTES / (LCD_UI_WIDTH * 2))
static bool s_ui;
static uint16_t *s_ui_pixels;
#endif

struct lcd_field_state
{
    uint16_t x;
    uint16_t y;
    uint8_t chars;
    bool dirty;
    uint16_t color;
    char text[LCD_TEXT_MAX + 1];
};

/* Top:    CPU  38%  6%  36.5C          FPS 59.7  SPD 100%
   Bottom: RAM FREE 110K MIN  96K          PS 11.9/32M */
static struct lcd_field_state s_fields[LCD_FIELD_COUNT];

/* First buffer in the given state, or -1. Call with s_lock held. */
static int gpsp_lcd_find(enum lcd_buffer_state state)
{
    for (int i = 0; i < LCD_BUFFERS; i++)
        if (s_state[i] == state)
            return i;
    return -1;
}

/* Waits for the transfer only if it was queued. */
static bool gpsp_lcd_draw(int x0, int y0, int x1, int y1, const void *pixels)
{
    return gpsp_spi_draw(x0, y0, x1, y1, pixels) == ESP_OK;
}

/* Clears the whole panel. SPI output uses buffer 0 in row batches. */
static void gpsp_lcd_clear(void)
{
#if CONFIG_GPSP_LCD_DSI
    gpsp_dsi_clear();
#else
    const int rows = LCD_FRAME_BYTES / (LCD_WIDTH * 2);

    memset(s_buffers[0], 0, LCD_FRAME_BYTES);
    for (int y = 0; y < LCD_HEIGHT; y += rows)
        gpsp_lcd_draw(0, y, LCD_WIDTH, y + rows < LCD_HEIGHT ?
                y + rows : LCD_HEIGHT, s_buffers[0]);
#endif
}

/* Draws text at (x, y) in one color; the cells clear what was there. */
static bool gpsp_lcd_draw_text(int x, int y, const char *text, int chars,
        uint16_t color)
{
    int width = chars * GPSP_FONT_WIDTH;

    if (chars > LCD_TEXT_MAX)
        return false;
    memset(s_text_pixels, 0, width * GPSP_FONT_HEIGHT * 2);
    for (int c = 0; c < chars && text[c]; c++)
    {
        const uint8_t *glyph = gpsp_font_glyph(text[c]);

        for (int row = 0; row < GPSP_FONT_HEIGHT; row++)
            for (int column = 0; column < GPSP_FONT_WIDTH; column++)
                if (glyph[row] >> column & 1)
                    s_text_pixels[row * width + c * GPSP_FONT_WIDTH + column] =
                            color;
    }
    return gpsp_lcd_draw(x, y, x + width, y + GPSP_FONT_HEIGHT,
            s_text_pixels);
}

static void gpsp_lcd_set_field(enum lcd_field field, const char *text,
        uint16_t color)
{
    struct lcd_field_state *state = &s_fields[field];

    portENTER_CRITICAL(&s_lock);
    if (strncmp(state->text, text, LCD_TEXT_MAX) || state->color != color)
    {
        strlcpy(state->text, text, sizeof(state->text));
        state->color = color;
        state->dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* Sends one changed field; true only after a successful draw. */
static bool gpsp_lcd_send_field(void)
{
    struct lcd_field_state field;
    int field_index = 0;
    bool found = false;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < LCD_FIELD_COUNT && !found; i++)
        if (s_fields[i].dirty)
        {
            s_fields[i].dirty = false;
            field = s_fields[i];
            field_index = i;
            found = true;
        }
    portEXIT_CRITICAL(&s_lock);

    if (!found || !LCD_STATUS)
        return false;
    /* A failed draw stays pending even if the value does not change. */
    if (!gpsp_lcd_draw_text(field.x, field.y, field.text, field.chars,
            field.color))
    {
        portENTER_CRITICAL(&s_lock);
        s_fields[field_index].dirty = true;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

static uint16_t gpsp_lcd_level(unsigned value, unsigned warn, unsigned bad,
        bool high_is_bad)
{
    if (high_is_bad)
        return value >= bad ? LCD_COLOR_BAD :
                value >= warn ? LCD_COLOR_WARN : LCD_COLOR_GOOD;
    return value < bad ? LCD_COLOR_BAD :
            value < warn ? LCD_COLOR_WARN : LCD_COLOR_GOOD;
}

#if CONFIG_GPSP_STATUS
/* Fixed frameskip lowers the expected FPS and warning thresholds. */
static void gpsp_lcd_set_fps(unsigned tenths)
{
    char text[LCD_TEXT_MAX + 1];
    int frameskip = s_frameskip;
    unsigned expected = frameskip > 0 ?
            LCD_FPS_TENTHS / (unsigned)(frameskip + 1) : LCD_FPS_TENTHS;
    unsigned warn = frameskip > 0 ? expected * 97 / 100 : 580;
    unsigned bad = frameskip > 0 ? expected * 84 / 100 : 500;

    if (tenths > 999)
        tenths = 999;
    snprintf(text, sizeof(text), "%2u.%u", tenths / 10, tenths % 10);
    gpsp_lcd_set_field(LCD_FIELD_FPS, text,
            gpsp_lcd_level(tenths, warn, bad, false));
}
#endif

static void gpsp_lcd_draw_labels(void)
{
    if (!LCD_STATUS)
        return;
#if CONFIG_GPSP_STATUS
    for (unsigned i = 0; i < sizeof(lcd_status_labels) /
            sizeof(lcd_status_labels[0]); i++)
    {
        const struct lcd_status_label *label = &lcd_status_labels[i];
        const struct lcd_status_position *p = &label->position;

        gpsp_lcd_draw_text(p->right ? LCD_RIGHT_COLUMN(p->column) :
                LCD_COLUMN(p->column), p->bottom ? LCD_STATUS_BOTTOM :
                LCD_STATUS_TOP, label->text, p->chars, LCD_COLOR_LABEL);
    }
#endif
}

/* Samples sent frames at intervals of at least one second. */
static void gpsp_lcd_update_fps(int64_t *start_us, uint32_t *start_frames)
{
#if CONFIG_GPSP_STATUS
    int64_t now_us = esp_timer_get_time();
    uint32_t frames = s_sent;

    if (now_us - *start_us < 1000000)
        return;
    gpsp_lcd_set_fps((unsigned)((uint64_t)(frames - *start_frames) *
            10000000u / (uint64_t)(now_us - *start_us)));
    *start_us = now_us;
    *start_frames = frames;
#else
    (void)start_us;
    (void)start_frames;
#endif
}

/* Sends the newest ready buffer; the core never waits for the display. */
static void gpsp_lcd_task(void *arg)
{
    int64_t fps_start_us = esp_timer_get_time();
    uint32_t fps_start_frames = s_sent;

    (void)arg;

    while (!s_stop)
    {
        int index = -1;

        /* Without frames the status still updates, e.g. FPS falls to 0. */
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)))
        {
            gpsp_lcd_update_fps(&fps_start_us, &fps_start_frames);
            xSemaphoreTake(s_draw_lock, portMAX_DELAY);
#if CONFIG_GPSP_MENU
            if (!s_ui)
#endif
                while (!s_stop && gpsp_lcd_send_field())
                    ;
            xSemaphoreGive(s_draw_lock);
            continue;
        }
        for (;;)
        {
#if CONFIG_GPSP_MENU && CONFIG_GPSP_LCD_DSI
            bool ui_frame, ui_refresh;
            unsigned overlay_revision = atomic_load(&s_overlay_revision);
#endif
            xSemaphoreTake(s_draw_lock, portMAX_DELAY);
            portENTER_CRITICAL(&s_lock);
#if CONFIG_GPSP_MENU && CONFIG_GPSP_LCD_DSI
            ui_frame = s_ui && s_ui_pending;
            ui_refresh = s_ui && s_ui_cached_valid &&
                    overlay_revision != s_overlay_drawn;
            s_ui_pending = false;
#endif
#if CONFIG_GPSP_MENU
            index = s_ui ? -1 : gpsp_lcd_find(LCD_BUFFER_READY);
#else
            index = gpsp_lcd_find(LCD_BUFFER_READY);
#endif
            if (index >= 0)
                s_state[index] = LCD_BUFFER_SENDING;
            portEXIT_CRITICAL(&s_lock);
#if CONFIG_GPSP_MENU && CONFIG_GPSP_LCD_DSI
            if (ui_frame || ui_refresh)
            {
                /* The UI caller waits for ui_frame; touch redraws use this copy. */
                if (ui_frame)
                {
                    memcpy(s_ui_cached, s_ui_pixels, LCD_UI_BYTES);
                    s_ui_cached_valid = true;
                }
                if (!s_stop)
                    gpsp_dsi_draw(s_ui_cached, true);
                s_overlay_drawn = overlay_revision;
                if (ui_frame)
                    xSemaphoreGive(s_ui_done);
                xSemaphoreGive(s_draw_lock);
                continue;
            }
#endif
            if (index < 0 || s_stop)
            {
                xSemaphoreGive(s_draw_lock);
                break;
            }

#if CONFIG_GPSP_LCD_DSI
            bool sent = gpsp_dsi_draw(s_buffers[index], false) == ESP_OK;
#else
            bool sent = gpsp_lcd_draw(LCD_OFFSET_X, LCD_OFFSET_Y,
                    LCD_OFFSET_X + LCD_FRAME_WIDTH,
                    LCD_OFFSET_Y + LCD_FRAME_HEIGHT, s_buffers[index]);
#endif

            portENTER_CRITICAL(&s_lock);
            s_state[index] = LCD_BUFFER_FREE;
            s_last = index;
            portEXIT_CRITICAL(&s_lock);
            if (sent)
                s_sent++;
            /* Frames may keep coming without a break out of this loop. */
            gpsp_lcd_update_fps(&fps_start_us, &fps_start_frames);

            /* Send only one status field before the next game frame. */
            gpsp_lcd_send_field();
            xSemaphoreGive(s_draw_lock);
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

#if CONFIG_GPSP_LCD_DSI
void gpsp_lcd_overlay_changed(void)
{
    atomic_fetch_add(&s_overlay_revision, 1);
    if (s_task)
        xTaskNotifyGive(s_task);
}
#endif

esp_err_t gpsp_lcd_init(void)
{

    esp_err_t result;

    for (int i = 0; i < LCD_FIELD_COUNT; i++)
    {
        const struct lcd_status_position *p = &lcd_status_positions[i];

        s_fields[i].x = p->right ? LCD_RIGHT_COLUMN(p->column) :
                LCD_COLUMN(p->column);
        s_fields[i].y = p->bottom ? LCD_STATUS_BOTTOM : LCD_STATUS_TOP;
        s_fields[i].chars = p->chars;
    }
    s_draw_lock = xSemaphoreCreateMutex();
#if CONFIG_GPSP_MENU && CONFIG_GPSP_LCD_DSI
    s_ui_done = xSemaphoreCreateBinary();
    if (!s_ui_done)
    {
        gpsp_lcd_shutdown();
        return ESP_ERR_NO_MEM;
    }
#endif
    for (int i = 0; i < LCD_BUFFERS; i++)
    {
        /* Cache-line alignment lets the display DMA use the buffer directly. */
        s_buffers[i] = heap_caps_aligned_alloc(64, LCD_BUFFER_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        s_state[i] = LCD_BUFFER_FREE;
    }
    s_text_pixels = heap_caps_aligned_alloc(64,
            LCD_TEXT_MAX * GPSP_FONT_WIDTH * GPSP_FONT_HEIGHT * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_draw_lock || !s_buffers[0] || !s_buffers[1] || !s_buffers[2] ||
            !s_text_pixels)
    {
        gpsp_lcd_shutdown();
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_GPSP_LCD_DSI
    result = gpsp_dsi_init();
#else
    result = gpsp_spi_init();
#endif
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(result));
        gpsp_lcd_shutdown();
        return result;
    }

    gpsp_lcd_clear();
    gpsp_lcd_draw_labels();
#if !CONFIG_GPSP_LCD_DSI
    gpsp_spi_backlight(true);
#endif

    s_stop = false;
#if CONFIG_GPSP_LCD_DSI
    /* DSI integer scaling uses aligned row scratch and PIE task context. */
    unsigned stack_size = 5120;
#else
    unsigned stack_size = 3072;
#endif
    if (xTaskCreatePinnedToCore(gpsp_lcd_task, "gpsp_lcd", stack_size, NULL,
            tskIDLE_PRIORITY + 5, &s_task, LCD_TASK_CORE) != pdPASS)
    {
        gpsp_lcd_shutdown();
        return ESP_ERR_NO_MEM;
    }

#if !CONFIG_GPSP_LCD_DSI
    ESP_LOGI(TAG, "ST7789 %ux%u at (%u,%u), SPI %u MHz", LCD_FRAME_WIDTH,
            LCD_FRAME_HEIGHT, LCD_OFFSET_X, LCD_OFFSET_Y,
            CONFIG_GPSP_LCD_SPI_MHZ);
#endif
    return ESP_OK;
}

bool gpsp_lcd_present(const uint16_t *pixels, unsigned width,
        unsigned height, size_t pitch)
{
    int index = -1;

    if (!s_task || width != LCD_FRAME_WIDTH || height != LCD_FRAME_HEIGHT)
        return false;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < LCD_BUFFERS; i++)
        if (s_buffers[i] == pixels && s_state[i] == LCD_BUFFER_WRITING)
            index = i;
    portEXIT_CRITICAL(&s_lock);

    /* A frame from the core's own buffer (the first one) is copied. */
    if (index < 0)
    {
        portENTER_CRITICAL(&s_lock);
        index = gpsp_lcd_find(LCD_BUFFER_FREE);
        if (index >= 0)
            s_state[index] = LCD_BUFFER_WRITING;
        portEXIT_CRITICAL(&s_lock);
        if (index < 0)
            return false;
        for (unsigned y = 0; y < height; y++)
            memcpy(s_buffers[index] + y * LCD_FRAME_WIDTH,
                    (const uint8_t *)pixels + y * pitch, LCD_FRAME_WIDTH * 2);
    }

    /* Keep one ready frame: an older one would be sent after this one. */
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < LCD_BUFFERS; i++)
        if (i != index && s_state[i] == LCD_BUFFER_READY)
            s_state[i] = LCD_BUFFER_FREE;
    s_state[index] = LCD_BUFFER_READY;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

/* With one buffer ready and one sending, the third is always free here. */
uint16_t *gpsp_lcd_next_buffer(void)
{
    int index;

    if (!s_task)
        return NULL;
    portENTER_CRITICAL(&s_lock);
    index = gpsp_lcd_find(LCD_BUFFER_FREE);
    if (index >= 0)
        s_state[index] = LCD_BUFFER_WRITING;
    portEXIT_CRITICAL(&s_lock);
    return index >= 0 ? s_buffers[index] : NULL;
}

void gpsp_lcd_frameskip(int frameskip)
{
    s_frameskip = frameskip;
}

void gpsp_lcd_status(unsigned cpu0, unsigned cpu1, int temp_tenths,
        unsigned speed, unsigned ram_kib, unsigned min_kib,
        unsigned psram_used_tenths, unsigned psram_mib)
{
    char text[LCD_TEXT_MAX + 3];
    unsigned psram_free_tenths;

    if (!s_task)
        return;
    snprintf(text, sizeof(text), "%3u%%", speed > 999 ? 999 : speed);
    gpsp_lcd_set_field(LCD_FIELD_SPEED, text,
            gpsp_lcd_level(speed, 97, 84, false));
    snprintf(text, sizeof(text), "%3u%%", cpu0 > 100 ? 100 : cpu0);
    gpsp_lcd_set_field(LCD_FIELD_CPU0, text, gpsp_lcd_level(cpu0, 70, 90, true));
    snprintf(text, sizeof(text), "%3u%%", cpu1 > 100 ? 100 : cpu1);
    gpsp_lcd_set_field(LCD_FIELD_CPU1, text, gpsp_lcd_level(cpu1, 70, 90, true));
    if (temp_tenths == INT_MIN)
        gpsp_lcd_set_field(LCD_FIELD_TEMP, "--.-C", LCD_COLOR_GOOD);
    else
    {
        if (temp_tenths < 0)
            temp_tenths = 0;
        if (temp_tenths > 999)
            temp_tenths = 999;
        snprintf(text, sizeof(text), "%2d.%dC", temp_tenths / 10,
                temp_tenths % 10);
        gpsp_lcd_set_field(LCD_FIELD_TEMP, text,
                gpsp_lcd_level((unsigned)temp_tenths, 600, 750, true));
    }
    snprintf(text, sizeof(text), "%3uK", ram_kib > 999 ? 999 : ram_kib);
    gpsp_lcd_set_field(LCD_FIELD_RAM, text, gpsp_lcd_level(ram_kib, 64, 32, false));
    snprintf(text, sizeof(text), "%3uK", min_kib > 999 ? 999 : min_kib);
    gpsp_lcd_set_field(LCD_FIELD_MIN, text, gpsp_lcd_level(min_kib, 64, 32, false));
    if (psram_mib > 99)
        psram_mib = 99;
    if (psram_used_tenths > psram_mib * 10)
        psram_used_tenths = psram_mib * 10;
    /* Used against the chip size; colors follow what is left. */
    psram_free_tenths = psram_mib * 10 - psram_used_tenths;
    snprintf(text, sizeof(text), "%2u.%u", psram_used_tenths / 10,
            psram_used_tenths % 10);
    gpsp_lcd_set_field(LCD_FIELD_PSRAM, text,
            gpsp_lcd_level(psram_free_tenths, 40, 10, false));
    snprintf(text, sizeof(text), "/%uM", psram_mib);
    gpsp_lcd_set_field(LCD_FIELD_PSRAM_SIZE, text, LCD_COLOR_LABEL);
#if CONFIG_GPSP_LCD_DSI
    struct lcd_status_value values[LCD_FIELD_COUNT];

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < LCD_FIELD_COUNT; i++)
    {
        memcpy(values[i].text, s_fields[i].text, sizeof(values[i].text));
        values[i].color = s_fields[i].color;
    }
    portEXIT_CRITICAL(&s_lock);
    gpsp_dsi_hud_update(values);
    gpsp_dsi_status();
#endif
}

#if CONFIG_GPSP_MENU
bool gpsp_lcd_ui_begin(uint16_t **pixels, const uint16_t **frame)
{
    int index;

    *pixels = NULL;
    *frame = NULL;
    if (!s_task)
        return false;
    if (!s_ui_pixels)
        s_ui_pixels = heap_caps_aligned_alloc(64, LCD_UI_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_ui_pixels)
        return false;
#if CONFIG_GPSP_LCD_DSI
    if (!s_ui_cached)
        s_ui_cached = heap_caps_aligned_alloc(64, LCD_UI_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_ui_cached)
        return false;
#endif

    portENTER_CRITICAL(&s_lock);
    s_ui = true;
#if CONFIG_GPSP_LCD_DSI
    s_ui_cached_valid = false;
#endif
    portEXIT_CRITICAL(&s_lock);
    /* Waits for a frame on the display; the task then sends no more. */
    xSemaphoreTake(s_draw_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    index = gpsp_lcd_find(LCD_BUFFER_READY);
    if (index < 0 && s_last >= 0 && s_state[s_last] == LCD_BUFFER_FREE)
        index = s_last;
    if (index >= 0)
        s_state[index] = LCD_BUFFER_HELD;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_draw_lock);

    *pixels = s_ui_pixels;
    if (index >= 0)
        *frame = s_buffers[index];
    return true;
}

void gpsp_lcd_ui_draw(unsigned y0, unsigned y1)
{
    if (!s_ui || y0 >= y1 || y1 > LCD_UI_HEIGHT)
        return;
#if LCD_HEIGHT < LCD_UI_HEIGHT
    /* Only the rows the panel shows. */
    if (y0 < LCD_UI_CROP_Y)
        y0 = LCD_UI_CROP_Y;
    if (y1 > LCD_UI_CROP_Y + LCD_UI_SHOWN_HEIGHT)
        y1 = LCD_UI_CROP_Y + LCD_UI_SHOWN_HEIGHT;
    if (y0 >= y1)
        return;
#endif
#if CONFIG_GPSP_LCD_DSI
    /* Keep expansion on CPU1; the caller cannot edit the canvas until done. */
    xSemaphoreTake(s_ui_done, 0);
    portENTER_CRITICAL(&s_lock);
    s_ui_pending = true;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_ui_done, portMAX_DELAY);
#else
    xSemaphoreTake(s_draw_lock, portMAX_DELAY);
#if LCD_WIDTH >= LCD_UI_WIDTH
    for (unsigned y = y0; y < y1; y += LCD_UI_ROWS)
    {
        unsigned end = y + LCD_UI_ROWS < y1 ? y + LCD_UI_ROWS : y1;

        gpsp_lcd_draw(LCD_UI_X, LCD_UI_Y + y - LCD_UI_CROP_Y,
                LCD_UI_X + LCD_UI_WIDTH, LCD_UI_Y + end - LCD_UI_CROP_Y,
                s_ui_pixels + y * LCD_UI_WIDTH);
    }
#else
    /* Cropped rows are not contiguous: send them one by one. */
    for (unsigned y = y0; y < y1; y++)
        gpsp_lcd_draw(0, LCD_UI_Y + y - LCD_UI_CROP_Y, LCD_WIDTH,
                LCD_UI_Y + y + 1 - LCD_UI_CROP_Y,
                s_ui_pixels + y * LCD_UI_WIDTH + LCD_UI_CROP_X);
#endif
    xSemaphoreGive(s_draw_lock);
#endif
}

void gpsp_lcd_ui_end(void)
{
    if (!s_ui)
        return;
    memset(s_ui_pixels, 0, LCD_UI_BYTES);
    gpsp_lcd_ui_draw(0, LCD_UI_HEIGHT);

    xSemaphoreTake(s_draw_lock, portMAX_DELAY);
    gpsp_lcd_draw_labels();
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < LCD_FIELD_COUNT; i++)
        s_fields[i].dirty = s_fields[i].text[0] != '\0';
    for (int i = 0; i < LCD_BUFFERS; i++)
        if (s_state[i] == LCD_BUFFER_HELD)
            s_state[i] = LCD_BUFFER_FREE;
    s_ui = false;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_draw_lock);
}
#endif

void gpsp_lcd_shutdown(void)
{
    if (s_task)
    {
        s_stop = true;
        xTaskNotifyGive(s_task);
        while (s_task)
            vTaskDelay(1);
    }
#if CONFIG_GPSP_LCD_DSI
    gpsp_dsi_shutdown();
#else
    if (s_buffers[0])
        gpsp_lcd_clear();
    gpsp_spi_shutdown();
#endif
    for (int i = 0; i < LCD_BUFFERS; i++)
    {
        heap_caps_free(s_buffers[i]);
        s_buffers[i] = NULL;
    }
    heap_caps_free(s_text_pixels);
    s_text_pixels = NULL;
#if CONFIG_GPSP_MENU
    heap_caps_free(s_ui_pixels);
    s_ui_pixels = NULL;
    s_ui = false;
#if CONFIG_GPSP_LCD_DSI
    s_ui_pending = false;
    heap_caps_free(s_ui_cached);
    s_ui_cached = NULL;
    s_ui_cached_valid = false;
    if (s_ui_done)
    {
        vSemaphoreDelete(s_ui_done);
        s_ui_done = NULL;
    }
#endif
#endif
    s_last = -1;
    if (s_draw_lock)
    {
        vSemaphoreDelete(s_draw_lock);
        s_draw_lock = NULL;
    }
}

#else

esp_err_t gpsp_lcd_init(void)
{
    return ESP_OK;
}

bool gpsp_lcd_present(const uint16_t *pixels, unsigned width,
        unsigned height, size_t pitch)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)pitch;
    return false;
}

uint16_t *gpsp_lcd_next_buffer(void)
{
    return NULL;
}

void gpsp_lcd_frameskip(int frameskip)
{
    (void)frameskip;
}

void gpsp_lcd_status(unsigned cpu0, unsigned cpu1, int temp_tenths,
        unsigned speed, unsigned ram_kib, unsigned min_kib,
        unsigned psram_used_tenths, unsigned psram_mib)
{
    (void)cpu0;
    (void)cpu1;
    (void)temp_tenths;
    (void)speed;
    (void)ram_kib;
    (void)min_kib;
    (void)psram_used_tenths;
    (void)psram_mib;
}

#if CONFIG_GPSP_MENU
bool gpsp_lcd_ui_begin(uint16_t **pixels, const uint16_t **frame)
{
    *pixels = NULL;
    *frame = NULL;
    return false;
}

void gpsp_lcd_ui_draw(unsigned y0, unsigned y1)
{
    (void)y0;
    (void)y1;
}

void gpsp_lcd_ui_end(void)
{
}
#endif

void gpsp_lcd_shutdown(void)
{
}

#endif

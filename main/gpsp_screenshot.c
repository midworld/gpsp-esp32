/* Screen pictures in the console log.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_screenshot.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_timer.h"

#include "gpsp_png.h"

/* 57 bytes make one 76-character base64 line. */
#define SCREENSHOT_LINE_BYTES 57

struct screenshot_out
{
    uint8_t data[SCREENSHOT_LINE_BYTES];
    size_t length;
    size_t total;
};

static void screenshot_line(struct screenshot_out *out)
{
    static const char digits[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char text[SCREENSHOT_LINE_BYTES / 3 * 4 + 1];
    size_t length = 0;

    for (size_t i = 0; i < out->length; i += 3)
    {
        uint32_t group = (uint32_t)out->data[i] << 16;
        size_t left = out->length - i;

        if (left > 1)
            group |= (uint32_t)out->data[i + 1] << 8;
        if (left > 2)
            group |= out->data[i + 2];
        text[length++] = digits[group >> 18 & 63];
        text[length++] = digits[group >> 12 & 63];
        text[length++] = left > 1 ? digits[group >> 6 & 63] : '=';
        text[length++] = left > 2 ? digits[group & 63] : '=';
    }
    text[length] = '\0';
    printf("SS %s\n", text);
    out->length = 0;
}

static bool screenshot_write(const void *data, size_t size, void *user)
{
    struct screenshot_out *out = user;
    const uint8_t *bytes = data;

    out->total += size;
    while (size--)
    {
        out->data[out->length++] = *bytes++;
        if (out->length == SCREENSHOT_LINE_BYTES)
            screenshot_line(out);
    }
    return true;
}

void gpsp_screenshot_print(const uint16_t *pixels, unsigned width,
        unsigned height, size_t pitch)
{
    struct screenshot_out out = {0};
    int64_t start_us = esp_timer_get_time();
    bool ok;

    printf("SCREENSHOT_BEGIN %ux%u\n", width, height);
    ok = gpsp_png_encode(pixels, width, height, pitch, screenshot_write, &out);
    if (out.length)
        screenshot_line(&out);
    printf("SCREENSHOT_END ok=%u bytes=%u ms=%u\n", ok, (unsigned)out.total,
            (unsigned)((esp_timer_get_time() - start_us) / 1000));
}

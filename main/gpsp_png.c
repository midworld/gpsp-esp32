/* PNG files to and from RGB565 pictures, using the ROM's deflate.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_png.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "miniz.h"

/* IDAT chunks are written this large. */
#define PNG_IDAT_BYTES 8192
/* Dictionary probes: fast, and enough for screen pictures. */
#define PNG_PROBES 16

struct png_out
{
    gpsp_png_write_fn write;
    void *user;
    bool ok;
    size_t length;
    uint8_t data[PNG_IDAT_BYTES];
};

static const uint8_t s_signature[8] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
};

static void png_put32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint32_t png_get32(const uint8_t *data)
{
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
            (uint32_t)data[2] << 8 | data[3];
}

static void png_chunk(struct png_out *out, const char *type,
        const uint8_t *data, size_t size)
{
    uint8_t head[8];
    uint8_t tail[4];
    uint32_t crc;

    if (!out->ok)
        return;
    png_put32(head, (uint32_t)size);
    memcpy(head + 4, type, 4);
    crc = esp_rom_crc32_le(0, head + 4, 4);
    crc = esp_rom_crc32_le(crc, data, size);
    png_put32(tail, crc);
    out->ok = out->write(head, sizeof(head), out->user) &&
            (!size || out->write(data, size, out->user)) &&
            out->write(tail, sizeof(tail), out->user);
}

/* Takes deflate output; full buffers go out as IDAT chunks. */
static mz_bool png_deflate_out(const void *data, int size, void *user)
{
    struct png_out *out = user;
    const uint8_t *bytes = data;

    while (size > 0 && out->ok)
    {
        size_t part = PNG_IDAT_BYTES - out->length;

        if (part > (size_t)size)
            part = (size_t)size;
        memcpy(out->data + out->length, bytes, part);
        out->length += part;
        bytes += part;
        size -= (int)part;
        if (out->length == PNG_IDAT_BYTES)
        {
            png_chunk(out, "IDAT", out->data, out->length);
            out->length = 0;
        }
    }
    return out->ok;
}

bool gpsp_png_encode(const uint16_t *pixels, unsigned width, unsigned height,
        size_t pitch, gpsp_png_write_fn write, void *user)
{
    size_t row_bytes = 1 + width * 3;
    tdefl_compressor *deflate = heap_caps_malloc(sizeof(*deflate),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    struct png_out *out = heap_caps_malloc(sizeof(*out) + row_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t header[13];
    bool ok = false;

    if (!deflate || !out)
        goto done;

    out->write = write;
    out->user = user;
    out->ok = write(s_signature, sizeof(s_signature), user);
    out->length = 0;

    /* 8-bit RGB, no interlace. */
    png_put32(header, width);
    png_put32(header + 4, height);
    header[8] = 8;
    header[9] = 2;
    header[10] = 0;
    header[11] = 0;
    header[12] = 0;
    png_chunk(out, "IHDR", header, sizeof(header));

    if (tdefl_init(deflate, png_deflate_out, out,
            PNG_PROBES | TDEFL_WRITE_ZLIB_HEADER) != TDEFL_STATUS_OKAY)
        goto done;

    uint8_t *row = (uint8_t *)(out + 1);

    for (unsigned y = 0; y < height && out->ok; y++)
    {
        const uint16_t *source = (const uint16_t *)
                ((const uint8_t *)pixels + y * pitch);
        uint8_t previous[3] = {0, 0, 0};

        /* The Sub filter: flat areas become runs of zeros. */
        row[0] = 1;
        for (unsigned x = 0; x < width; x++)
        {
            uint16_t pixel = source[x];
            uint8_t rgb[3] = {
                (uint8_t)((pixel >> 8 & 0xf8) | (pixel >> 13)),
                (uint8_t)((pixel >> 3 & 0xfc) | (pixel >> 9 & 0x03)),
                (uint8_t)((pixel << 3 & 0xf8) | (pixel >> 2 & 0x07)),
            };

            for (int c = 0; c < 3; c++)
            {
                row[1 + x * 3 + c] = (uint8_t)(rgb[c] - previous[c]);
                previous[c] = rgb[c];
            }
        }
        if (tdefl_compress_buffer(deflate, row, row_bytes, TDEFL_NO_FLUSH) !=
                TDEFL_STATUS_OKAY)
            goto done;
    }
    if (tdefl_compress_buffer(deflate, NULL, 0, TDEFL_FINISH) !=
            TDEFL_STATUS_DONE)
        goto done;
    if (out->length)
        png_chunk(out, "IDAT", out->data, out->length);
    png_chunk(out, "IEND", NULL, 0);
    ok = out->ok;

done:
    heap_caps_free(out);
    heap_caps_free(deflate);
    return ok;
}

static uint8_t png_paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* Undoes the row filters in place; rows are 1 + width * channels long. */
static bool png_unfilter(uint8_t *data, unsigned width, unsigned height,
        unsigned channels)
{
    size_t stride = (size_t)width * channels;
    const uint8_t *above = NULL;

    for (unsigned y = 0; y < height; y++)
    {
        uint8_t filter = data[y * (stride + 1)];
        uint8_t *row = data + y * (stride + 1) + 1;

        for (size_t i = 0; i < stride; i++)
        {
            uint8_t a = i >= channels ? row[i - channels] : 0;
            uint8_t b = above ? above[i] : 0;
            uint8_t c = above && i >= channels ? above[i - channels] : 0;

            switch (filter)
            {
                case 0: break;
                case 1: row[i] += a; break;
                case 2: row[i] += b; break;
                case 3: row[i] += (uint8_t)((a + b) / 2); break;
                case 4: row[i] += png_paeth(a, b, c); break;
                default: return false;
            }
        }
        above = row;
    }
    return true;
}

bool gpsp_png_decode(const uint8_t *data, size_t size, uint16_t *pixels,
        unsigned width, unsigned height)
{
    unsigned channels = 0;
    size_t offset = sizeof(s_signature);
    size_t packed = 0;
    size_t raw_size;
    size_t in_size;
    size_t out_size;
    uint8_t *idat = NULL;
    uint8_t *raw = NULL;
    tinfl_decompressor *inflate = NULL;
    bool ok = false;

    if (size < offset || memcmp(data, s_signature, offset) != 0)
        return false;

    idat = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!idat)
        return false;

    /* Joins the IDAT chunks into one zlib stream. */
    while (offset + 12 <= size)
    {
        uint32_t length = png_get32(data + offset);
        const uint8_t *type = data + offset + 4;
        const uint8_t *body = data + offset + 8;

        if (length > size - offset - 12)
            goto done;
        if (!memcmp(type, "IHDR", 4))
        {
            if (length != 13 || png_get32(body) != width ||
                    png_get32(body + 4) != height || body[8] != 8 ||
                    body[10] != 0 || body[11] != 0 || body[12] != 0)
                goto done;
            channels = body[9] == 2 ? 3 : body[9] == 6 ? 4 : 0;
            if (!channels)
                goto done;
        }
        else if (!memcmp(type, "IDAT", 4))
        {
            memcpy(idat + packed, body, length);
            packed += length;
        }
        else if (!memcmp(type, "IEND", 4))
            break;
        offset += 12 + length;
    }
    if (!channels || !packed)
        goto done;

    raw_size = (size_t)height * (1 + (size_t)width * channels);
    raw = heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    inflate = heap_caps_malloc(sizeof(*inflate),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || !inflate)
        goto done;

    tinfl_init(inflate);
    in_size = packed;
    out_size = raw_size;
    if (tinfl_decompress(inflate, idat, &in_size, raw, raw, &out_size,
            TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) != TINFL_STATUS_DONE ||
            out_size != raw_size ||
            !png_unfilter(raw, width, height, channels))
        goto done;

    for (unsigned y = 0; y < height; y++)
    {
        const uint8_t *row = raw + y * (1 + (size_t)width * channels) + 1;

        for (unsigned x = 0; x < width; x++, row += channels)
            pixels[y * width + x] = (uint16_t)((row[0] & 0xf8) << 8 |
                    (row[1] & 0xfc) << 3 | row[2] >> 3);
    }
    ok = true;

done:
    heap_caps_free(inflate);
    heap_caps_free(raw);
    heap_caps_free(idat);
    return ok;
}

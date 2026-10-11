/* Host tests for ROM backup signature scanning.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba_rom_scan.h"

#define BLOCK_SIZE (1024 * 1024)
#define ROM_SIZE (16 * BLOCK_SIZE)

struct RFILE
{
    uint8_t *data;
    int64_t size;
    int64_t position;
    int seek_error;
    int read_fail_after;
    int zero_read;
    int64_t max_read;
    unsigned reads;
};

static struct RFILE file;
static size_t alloc_limit;
static unsigned alloc_attempts, allocations;
static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
    } while (0)

void *test_malloc(size_t size)
{
    void *ptr;
    alloc_attempts++;
    if (size > alloc_limit)
        return NULL;
    ptr = malloc(size);
    if (ptr)
        allocations++;
    return ptr;
}

void test_free(void *ptr)
{
    if (ptr)
        allocations--;
    free(ptr);
}

int64_t filestream_get_size(RFILE *stream)
{
    return stream->size;
}

int64_t filestream_seek(RFILE *stream, int64_t offset, int whence)
{
    CHECK(whence == SEEK_SET);
    if (stream->seek_error)
        return -1;
    stream->position = offset;
    return offset;
}

int64_t filestream_read(RFILE *stream, void *data, int64_t size)
{
    stream->reads++;
    if (stream->read_fail_after == 0)
        return stream->zero_read ? 0 : -1;
    if (stream->read_fail_after > 0)
        stream->read_fail_after--;
    if (size > stream->max_read)
        size = stream->max_read;
    if (size > stream->size - stream->position)
        size = stream->size - stream->position;
    memcpy(data, stream->data + stream->position, (size_t)size);
    stream->position += size;
    return size;
}

static void reset(void)
{
    memset(file.data, 0, ROM_SIZE);
    file.size = ROM_SIZE;
    file.position = 0;
    file.seek_error = 0;
    file.read_fail_after = -1;
    file.zero_read = 0;
    file.max_read = ROM_SIZE;
    file.reads = 0;
    alloc_limit = ROM_SIZE;
    alloc_attempts = 0;
    CHECK(allocations == 0);
}

static struct rom_scan_result scan(unsigned count)
{
    uint8_t *blocks[8];
    struct rom_scan_result result;
    unsigned i;

    CHECK(count <= 8);
    for (i = 0; i < count; i++)
    {
        blocks[i] = malloc(BLOCK_SIZE);
        if (!blocks[i])
            exit(1);
        memcpy(blocks[i], file.data + i * BLOCK_SIZE, BLOCK_SIZE);
    }
    result = rom_scan_signatures(&file, blocks, count, BLOCK_SIZE);
    for (i = 0; i < count; i++)
        free(blocks[i]);
    CHECK(allocations == 0);
    return result;
}

int main(void)
{
    static const struct
    {
        const char *text;
        uint32_t flag;
    } signatures[] = {
        {"EEPROM_V", ROM_SIG_EEPROM},
        {"SRAM_V", ROM_SIG_SRAM},
        {"FLASH1M_V", ROM_SIG_FLASH1M},
        {"FLASH512_V", ROM_SIG_FLASH5},
        {"FLASH_V", ROM_SIG_FLASH5},
    };
    static const uint32_t offsets[] = {
        64, BLOCK_SIZE - 4, 8 * BLOCK_SIZE - 4,
        8 * BLOCK_SIZE + 32 * 1024 - 4, 0xFF79CC
    };
    struct rom_scan_result result;
    unsigned i, j;

    file.data = malloc(ROM_SIZE);
    if (!file.data)
        return 1;

    for (i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++)
    {
        for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++)
        {
            reset();
            memcpy(file.data + offsets[j], signatures[i].text,
                    strlen(signatures[i].text));
            result = scan(8);
            CHECK(result.status == ROM_SCAN_OK);
            CHECK(result.signatures == signatures[i].flag);
        }

        /* Exact EOF bounds, without relying on ROM page padding. */
        reset();
        memcpy(file.data + BLOCK_SIZE + 64, signatures[i].text,
                strlen(signatures[i].text));
        file.size = BLOCK_SIZE + 64 + strlen(signatures[i].text);
        result = scan(1);
        CHECK(result.status == ROM_SCAN_OK);
        CHECK(result.signatures == signatures[i].flag);
    }

    /* A complete SDK string can cross a resident block boundary. */
    reset();
    memcpy(file.data + BLOCK_SIZE - 4, "EEPROM_V124", 12);
    file.size = 2 * BLOCK_SIZE;
    result = scan(2);
    CHECK(result.status == ROM_SCAN_OK && result.signatures == ROM_SIG_EEPROM);
    CHECK(alloc_attempts == 0 && file.reads == 0);

    /* Short reads retain both alignment and split signatures. */
    for (i = 1; i <= 17; i += 2)
    {
        reset();
        memcpy(file.data + BLOCK_SIZE + 64, "EEPROM_V124", 12);
        file.size = BLOCK_SIZE + 128;
        file.max_read = i;
        result = scan(1);
        CHECK(result.status == ROM_SCAN_OK && result.signatures == ROM_SIG_EEPROM);
    }

    reset();
    memcpy(file.data + 0xFF79CC, "EEPROM_V124", 12);
    alloc_limit = 512;
    result = scan(8);
    CHECK(result.status == ROM_SCAN_OK && result.signatures == ROM_SIG_EEPROM);
    CHECK(alloc_attempts == 7);

    /* Failed scans preserve known matches and report why they stopped. */
    reset();
    memcpy(file.data + 64, "FLASH1M_V", 9);
    alloc_limit = 0;
    result = scan(1);
    CHECK(result.status == ROM_SCAN_NO_MEMORY);
    CHECK(result.signatures == ROM_SIG_FLASH1M && alloc_attempts == 7);

    reset();
    file.seek_error = 1;
    result = scan(1);
    CHECK(result.status == ROM_SCAN_SEEK_ERROR && file.reads == 0);

    for (i = 0; i < 2; i++)
    {
        reset();
        memcpy(file.data + BLOCK_SIZE, "EEPROM_V", 8);
        file.max_read = 8;
        file.read_fail_after = 1;
        file.zero_read = i;
        result = scan(1);
        CHECK(result.status == ROM_SCAN_READ_ERROR);
        CHECK(result.signatures == ROM_SIG_EEPROM);
    }

    reset();
    file.size = -1;
    result = scan(0);
    CHECK(result.status == ROM_SCAN_SIZE_ERROR);

    reset();
    file.size = 7;
    memcpy(file.data, "EEPROM_", 7);
    result = scan(1);
    CHECK(result.status == ROM_SCAN_OK && result.signatures == 0);
    CHECK(alloc_attempts == 0);

    reset();
    memcpy(file.data + 1, "EEPROM_V", 8);
    file.size = BLOCK_SIZE;
    result = scan(1);
    CHECK(result.status == ROM_SCAN_OK && result.signatures == 0);

    free(file.data);
    printf("ROM signature tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef uint8_t BYTE, DSTATUS;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned UINT;
typedef enum { RES_OK, RES_ERROR } DRESULT;
enum { ESP_OK, ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_CRC };
enum { CTRL_SYNC, GET_SECTOR_COUNT, GET_SECTOR_SIZE };
#define STA_NOINIT 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

typedef struct { struct { uint32_t sector_size, capacity; } csd; } sdmmc_card_t;
static sdmmc_card_t s_card = {{512, 4096}}, *s_sd_card = &s_card;
static bool s_card_failed;
static uint8_t s_buffer[3 * 512];
static struct
{
    bool write;
    const void *buffer;
    uint32_t sector, count;
} s_accesses[16];
static unsigned s_calls, s_recoveries, s_result_count;
static esp_err_t s_results[16], s_recovery_result;

static esp_err_t access_result(sdmmc_card_t *card, bool write, const void *buffer,
        uint32_t sector, uint32_t count)
{
    assert(card == s_sd_card && s_calls < 16);
    s_accesses[s_calls].write = write;
    s_accesses[s_calls].buffer = buffer;
    s_accesses[s_calls].sector = sector;
    s_accesses[s_calls].count = count;
    unsigned call = s_calls++;
    return call < s_result_count ? s_results[call] : ESP_OK;
}

static esp_err_t sdmmc_read_sectors(sdmmc_card_t *card, void *buffer,
        uint32_t sector, uint32_t count)
{
    return access_result(card, false, buffer, sector, count);
}

static esp_err_t sdmmc_write_sectors(sdmmc_card_t *card, const void *buffer,
        uint32_t sector, uint32_t count)
{
    return access_result(card, true, buffer, sector, count);
}

static esp_err_t sd_recover(sdmmc_card_t *card)
{
    assert(card == s_sd_card);
    s_recoveries++;
    return s_recovery_result;
}
#include "sd_access.inc"

static void reset(bool single)
{
    s_card_failed = false;
    s_single_writes = single;
    s_calls = s_recoveries = s_result_count = 0;
    s_recovery_result = ESP_OK;
    memset(s_results, 0, sizeof(s_results));
}

static void check_defaults(void)
{
    assert(s_single_writes == !!CONFIG_GPSP_SD_SINGLE_BLOCK_WRITES);
    assert(sd_write(0, s_buffer, 100, 2) == RES_OK);
    assert(s_calls == (s_single_writes ? 2u : 1u));
    assert(s_accesses[0].count == (s_single_writes ? 1u : 2u));
}

static void check_reads(void)
{
    reset(false);
    assert(sd_read(0, s_buffer, 100, 3) == RES_OK);
    assert(s_calls == 1 && !s_recoveries);
    reset(false);
    s_results[0] = ESP_ERR_TIMEOUT;
    s_result_count = 1;
    assert(sd_read(0, s_buffer, 100, 3) == RES_OK);
    assert(s_calls == 2 && s_recoveries == 1 && !s_single_writes);
    for (unsigned i = 0; i < s_calls; i++)
    {
        assert(!s_accesses[i].write && s_accesses[i].buffer == s_buffer);
        assert(s_accesses[i].sector == 100 && s_accesses[i].count == 3);
    }
    reset(false);
    s_results[0] = ESP_ERR_INVALID_CRC;
    s_result_count = 1;
    assert(sd_access(s_buffer, NULL, 100, 1) == ESP_ERR_INVALID_CRC);
    assert(s_calls == 1 && !s_recoveries && !s_card_failed);
}

static void check_write_fallback(void)
{
    reset(false);
    s_results[0] = ESP_ERR_TIMEOUT;
    s_result_count = 1;
    assert(sd_write(0, s_buffer, 100, 3) == RES_OK);
    assert(s_calls == 4 && s_recoveries == 1 && s_single_writes);
    assert(s_accesses[0].write && s_accesses[0].count == 3);
    for (unsigned i = 0; i < 3; i++)
    {
        assert(s_accesses[i + 1].write && s_accesses[i + 1].count == 1);
        assert(s_accesses[i + 1].sector == 100 + i);
        assert(s_accesses[i + 1].buffer == s_buffer + 512 * i);
    }
    assert(sd_write(0, s_buffer, 200, 2) == RES_OK);
    assert(s_calls == 6 && s_recoveries == 1);
    assert(s_accesses[4].sector == 200 && s_accesses[4].count == 1);
    assert(s_accesses[5].sector == 201 && s_accesses[5].count == 1);

    reset(true);
    s_results[1] = ESP_ERR_TIMEOUT;
    s_result_count = 2;
    assert(sd_write(0, s_buffer, 100, 3) == RES_OK);
    assert(s_calls == 5 && s_recoveries == 1);
    const unsigned sectors[] = {100, 101, 100, 101, 102};
    for (unsigned i = 0; i < s_calls; i++)
    {
        assert(s_accesses[i].sector == sectors[i] && s_accesses[i].count == 1);
        assert(s_accesses[i].buffer == s_buffer + 512 * (sectors[i] - 100));
    }
}

static void check_failures(void)
{
    reset(false);
    s_results[0] = ESP_ERR_TIMEOUT;
    s_result_count = 1;
    s_recovery_result = ESP_FAIL;
    assert(sd_read(0, s_buffer, 100, 1) == RES_ERROR);
    assert(s_card_failed && sd_status(0) == STA_NOINIT);
    assert(sd_access(s_buffer, NULL, 100, 1) == ESP_ERR_INVALID_STATE);
    assert(s_calls == 1 && s_recoveries == 1);

    for (unsigned write = 0; write < 2; write++)
    {
        reset(false);
        s_results[0] = s_results[1] = ESP_ERR_TIMEOUT;
        s_result_count = 2;
        DRESULT result = write ? sd_write(0, s_buffer, 100, 1) :
                sd_read(0, s_buffer, 100, 1);
        assert(result == RES_ERROR && s_card_failed);
        assert(sd_status(0) == STA_NOINIT);
        assert(sd_write(0, s_buffer, 100, 1) == RES_ERROR);
        assert(s_calls == 2 && s_recoveries == 1);
    }
}

int main(void)
{
    check_defaults();
    check_reads();
    check_write_fallback();
    check_failures();
    reset(false);
    DWORD sectors;
    WORD size;
    assert(sd_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
    assert(sd_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK && sectors == 4096);
    assert(sd_ioctl(0, GET_SECTOR_SIZE, &size) == RES_OK && size == 512);
    assert(sd_ioctl(0, 255, NULL) == RES_ERROR);
    printf("SDMMC_ACCESS PASS: single-block default=%u\n",
            CONFIG_GPSP_SD_SINGLE_BLOCK_WRITES);
    return 0;
}

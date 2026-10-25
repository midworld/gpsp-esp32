/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sample_wifi.h"
#include "sd_check.h"

#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "iperf3_tcp.h"
#endif

#define MIB (1024U * 1024U)
#define EXPECTED_PSRAM (32U * MIB)
#define EXPECTED_FLASH (16U * MIB)

#ifdef CONFIG_P4_NANO_BOARD_WT9932P4X_TINY
#define BOARD_NAME "WT9932P4X-TINY"
#else
#define BOARD_NAME "ESP32-P4-NANO-WIFI6-DB"
#endif

static void report(const char *name, int pass)
{
    printf("BOARD CHECK %-12s %s\n", name, pass ? "PASS" : "FAIL");
}

static int check_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    printf("P4 chip: model=%d revision=v%d.%d cores=%d\n", info.model,
            info.revision / 100, info.revision % 100, info.cores);
    printf("P4X generation: %s (revision v3.x; verify X on chip marking)\n",
            info.revision >= 300 ? "yes" : "no");
    int pass = info.model == CHIP_ESP32P4 && info.revision >= 300 &&
            info.cores == 2;
    report("P4X", pass);
    return pass;
}

static unsigned mib_per_s(size_t bytes, int64_t us)
{
    return us > 0 ? (unsigned)((uint64_t)bytes * 1000000U / MIB / us) : 0;
}

/* Address-dependent values catch shorted or stuck address lines. The test
 * block is much larger than the cache, so verify reads come from PSRAM. */
static int psram_pattern(uint32_t *ram, size_t words, uint32_t seed)
{
    int64_t start = esp_timer_get_time();
    for (size_t i = 0; i < words; i++)
        ram[i] = seed ^ ((uint32_t)i * 0x9e3779b1U);
    int64_t mid = esp_timer_get_time();
    size_t bad = 0;
    size_t first = 0;
    for (size_t i = 0; i < words; i++)
    {
        if (ram[i] != (seed ^ ((uint32_t)i * 0x9e3779b1U)))
        {
            if (bad++ == 0)
                first = i;
        }
    }
    int64_t end = esp_timer_get_time();
    size_t bytes = words * sizeof(*ram);
    printf("PSRAM pattern 0x%08lx: write=%u MiB/s verify=%u MiB/s "
            "errors=%u\n", (unsigned long)seed, mib_per_s(bytes, mid - start),
            mib_per_s(bytes, end - mid), (unsigned)bad);
    if (bad != 0)
        printf("PSRAM first mismatch at offset 0x%08x\n",
                (unsigned)(first * sizeof(*ram)));
    return bad == 0;
}

static int check_psram(void)
{
    size_t size = esp_psram_get_size();
    size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    printf("PSRAM: installed=%u MiB free=%u MiB clock=%d MHz\n",
            (unsigned)(size / MIB), (unsigned)(free_size / MIB),
            CONFIG_SPIRAM_SPEED);

    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    block &= ~(MIB - 1);
    uint32_t *ram = block ? heap_caps_malloc(block, MALLOC_CAP_SPIRAM) : NULL;
    int pass = size == EXPECTED_PSRAM && ram != NULL;
    if (ram != NULL)
    {
        printf("PSRAM test: %u MiB write/verify\n", (unsigned)(block / MIB));
        size_t words = block / sizeof(*ram);
        pass &= psram_pattern(ram, words, 0xa5a5a5a5U);
        pass &= psram_pattern(ram, words, 0x5a5a5a5aU);
        heap_caps_free(ram);
    }
    report("PSRAM", pass);
    return pass;
}

static int check_flash(void)
{
    uint32_t size = 0;
    uint32_t id = 0;
    esp_err_t size_err = esp_flash_get_physical_size(NULL, &size);
    esp_err_t id_err = esp_flash_read_id(NULL, &id);
    printf("Flash: physical=%u MiB JEDEC ID=0x%06lx (size: %s, ID: %s)\n",
            (unsigned)(size / MIB), (unsigned long)id,
            esp_err_to_name(size_err), esp_err_to_name(id_err));
    int pass = size_err == ESP_OK && id_err == ESP_OK &&
            size == EXPECTED_FLASH;
    report("FLASH", pass);
    return pass;
}

#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
#define WIFI_GOT_IP BIT0
#define WIFI_DISCONNECTED BIT1

static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
        void *data)
{
    EventGroupHandle_t events = arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = data;
        printf("Wi-Fi IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(events, WIFI_GOT_IP);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupSetBits(events, WIFI_DISCONNECTED);
    }
}

static int check_wifi_speed(void)
{
    const char *ssid = CONFIG_P4_NANO_WIFI_SSID;
    const char *password = CONFIG_P4_NANO_WIFI_PASSWORD;
    if (!CONFIG_P4_NANO_IPERF3_HOST[0])
    {
        printf("Wi-Fi speed: set the iperf3 server address in menuconfig\n");
        report("WIFI SPEED", 0);
        return 0;
    }
    if (ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 63)
    {
        printf("Wi-Fi speed: set a valid SSID/password in menuconfig\n");
        report("WIFI SPEED", 0);
        return 0;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (events == NULL)
    {
        report("WIFI SPEED", 0);
        return 0;
    }
    esp_event_handler_instance_t ip_handler;
    esp_event_handler_instance_t wifi_handler;
    esp_err_t err = esp_event_handler_instance_register(IP_EVENT,
            IP_EVENT_STA_GOT_IP, wifi_event, events, &ip_handler);
    if (err != ESP_OK)
    {
        vEventGroupDelete(events);
        report("WIFI SPEED", 0);
        return 0;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, wifi_event, events, &wifi_handler);
    if (err != ESP_OK)
    {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                ip_handler);
        vEventGroupDelete(events);
        report("WIFI SPEED", 0);
        return 0;
    }

    err = esp_wifi_connect();

    EventBits_t bits = 0;
    if (err == ESP_OK)
        bits = xEventGroupWaitBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED,
                pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    int pass = (bits & WIFI_GOT_IP) != 0;
    if (!pass)
    {
        printf("Wi-Fi connect: %s, event bits=0x%lx\n", esp_err_to_name(err),
                (unsigned long)bits);
    }
    else
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            printf("Wi-Fi AP: channel=%u RSSI=%d dBm\n", ap.primary, ap.rssi);
        printf("iperf3 server: %s:%d, duration=%d s per direction\n",
                CONFIG_P4_NANO_IPERF3_HOST, CONFIG_P4_NANO_IPERF3_PORT,
                CONFIG_P4_NANO_IPERF3_SECONDS);
        int rx = iperf3_tcp_run(CONFIG_P4_NANO_IPERF3_HOST,
                CONFIG_P4_NANO_IPERF3_PORT, CONFIG_P4_NANO_IPERF3_SECONDS, true);
        vTaskDelay(pdMS_TO_TICKS(1000));
        int tx = iperf3_tcp_run(CONFIG_P4_NANO_IPERF3_HOST,
                CONFIG_P4_NANO_IPERF3_PORT, CONFIG_P4_NANO_IPERF3_SECONDS, false);
        pass = tx && rx;
    }
    report("WIFI SPEED", pass);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
            ip_handler);
    esp_event_handler_instance_unregister(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, wifi_handler);
    vEventGroupDelete(events);
    return pass;
}
#endif

#ifdef CONFIG_P4_NANO_BOARD_NANO_WIFI6
static int check_wifi(void)
{
    printf("Expected radio module: ESP32-C5-MINI-1U (check shield marking)\n");
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        printf("NVS init: %s\n", esp_err_to_name(err));
        report("C5", 0);
#ifdef CONFIG_P4_NANO_WIFI_SCAN
        report("WIFI SCAN", 0);
#else
        printf("BOARD CHECK WIFI SCAN    SKIP\n");
#endif
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
        report("WIFI SPEED", 0);
#endif
        return 0;
    }

    err = esp_netif_init();
    if (err == ESP_OK)
        err = esp_event_loop_create_default();
    if (err != ESP_OK)
    {
        printf("Network init: %s\n", esp_err_to_name(err));
        report("C5", 0);
#ifdef CONFIG_P4_NANO_WIFI_SCAN
        report("WIFI SCAN", 0);
#else
        printf("BOARD CHECK WIFI SCAN    SKIP\n");
#endif
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
        report("WIFI SPEED", 0);
#endif
        return 0;
    }

    err = esp_hosted_init();
    if (err == ESP_OK)
        err = esp_hosted_connect_to_slave();
    if (err != ESP_OK)
    {
        printf("ESP-Hosted connection: %s\n", esp_err_to_name(err));
        report("C5", 0);
#ifdef CONFIG_P4_NANO_WIFI_SCAN
        report("WIFI SCAN", 0);
#else
        printf("BOARD CHECK WIFI SCAN    SKIP\n");
#endif
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
        report("WIFI SPEED", 0);
#endif
        return 0;
    }

    uint32_t chip_id = 0;
    char target[32] = {0};
    err = esp_hosted_get_cp_info(&chip_id, target, sizeof(target));
    printf("Wireless coprocessor: target=%s chip_id=0x%08lx (%s)\n",
            target[0] ? target : "unknown", (unsigned long)chip_id,
            esp_err_to_name(err));
    int chip_pass = err == ESP_OK && strcmp(target, "esp32c5") == 0;
    report("C5", chip_pass);

#if defined(CONFIG_P4_NANO_WIFI_SCAN) || \
        defined(CONFIG_P4_NANO_WIFI_SPEED_TEST)
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
    wifi_config_t wifi_config = {0};
    size_t ssid_len = strlen(CONFIG_P4_NANO_WIFI_SSID);
    if (ssid_len > sizeof(wifi_config.sta.ssid))
        ssid_len = sizeof(wifi_config.sta.ssid);
    size_t password_len = strlen(CONFIG_P4_NANO_WIFI_PASSWORD);
    if (password_len > sizeof(wifi_config.sta.password))
        password_len = sizeof(wifi_config.sta.password);
    memcpy(wifi_config.sta.ssid, CONFIG_P4_NANO_WIFI_SSID, ssid_len);
    memcpy(wifi_config.sta.password, CONFIG_P4_NANO_WIFI_PASSWORD,
            password_len);
    err = sample_wifi_start(&wifi_config);
#else
    err = sample_wifi_start(NULL);
#endif
    if (err != ESP_OK)
    {
        printf("Wi-Fi start: %s\n", esp_err_to_name(err));
#ifdef CONFIG_P4_NANO_WIFI_SCAN
        report("WIFI SCAN", 0);
#endif
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
        report("WIFI SPEED", 0);
#endif
        return 0;
    }

    int wifi_pass = 1;
#ifdef CONFIG_P4_NANO_WIFI_SCAN
    if (err == ESP_OK)
        err = esp_wifi_scan_start(NULL, true);

    uint16_t ap_count = 0;
    if (err == ESP_OK)
        err = esp_wifi_scan_get_ap_num(&ap_count);
    printf("Wi-Fi scan: %s, AP count=%u\n", esp_err_to_name(err), ap_count);
    if (err == ESP_OK && ap_count != 0)
    {
        static wifi_ap_record_t records[16];
        uint16_t listed = ap_count < 16 ? ap_count : 16;
        err = esp_wifi_scan_get_ap_records(&listed, records);
        if (err == ESP_OK)
        {
            for (uint16_t i = 0; i < listed; i++)
                printf("AP %u: SSID=%.*s channel=%u RSSI=%d dBm\n", i + 1,
                        32, records[i].ssid, records[i].primary, records[i].rssi);
        }
        else
        {
            printf("Wi-Fi scan records: %s\n", esp_err_to_name(err));
        }
    }
    int scan_pass = err == ESP_OK;
    report("WIFI SCAN", scan_pass);
    wifi_pass &= scan_pass;
#else
    printf("Wi-Fi scan: skipped\n");
    printf("BOARD CHECK WIFI SCAN    SKIP\n");
#endif
#ifdef CONFIG_P4_NANO_WIFI_SPEED_TEST
    wifi_pass &= check_wifi_speed();
#endif
    return chip_pass && wifi_pass;
#else
    printf("Wi-Fi scan: skipped (antenna not assumed attached)\n");
    printf("BOARD CHECK WIFI SCAN    SKIP\n");
    return chip_pass;
#endif
}
#endif

void app_main(void)
{
    int pass = 1;
    printf("\n=== %s board check ===\n", BOARD_NAME);
    pass &= check_chip();
    pass &= check_psram();
    pass &= check_flash();
#ifdef CONFIG_P4_NANO_SD_TEST
    /* Before ESP-Hosted, which uses SDMMC slot 1 on the Nano. */
    pass &= sd_check();
#else
    printf("BOARD CHECK SD           SKIP\n");
#endif
#ifdef CONFIG_P4_NANO_BOARD_NANO_WIFI6
    pass &= check_wifi();
#endif
#ifdef CONFIG_P4_NANO_BOARD_WT9932P4X_TINY
    printf("BOARD CHECK RESULT: %s\n", pass ? "PASS (NO RADIO)" : "FAIL");
#elif defined(CONFIG_P4_NANO_WIFI_SCAN) && \
        defined(CONFIG_P4_NANO_WIFI_SPEED_TEST)
    printf("BOARD CHECK RESULT: %s\n", pass ? "PASS" : "FAIL");
#elif defined(CONFIG_P4_NANO_WIFI_SCAN)
    printf("BOARD CHECK RESULT: %s\n", pass ? "PASS" : "FAIL");
#elif defined(CONFIG_P4_NANO_WIFI_SPEED_TEST)
    printf("BOARD CHECK RESULT: %s\n",
            pass ? "PASS (SCAN SKIPPED)" : "FAIL");
#else
    printf("BOARD CHECK RESULT: %s\n",
            pass ? "PASS (RF SCAN SKIPPED)" : "FAIL");
#endif
    printf("BOARD CHECK COMPLETE\n");
}

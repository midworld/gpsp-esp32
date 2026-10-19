/* Ethernet-first networking for ESP32-P4-NANO-WIFI6-DB.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"

#include "gpsp_network.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

enum gpsp_route
{
    GPSP_ROUTE_NONE,
    GPSP_ROUTE_ETHERNET,
    GPSP_ROUTE_WIFI,
};

static const char *TAG = "gpsp_p4_network";
static EventGroupHandle_t s_events;
static EventBits_t s_ready_bit;
static esp_eth_handle_t s_eth_handle;
static esp_eth_mac_t *s_mac;
static esp_eth_phy_t *s_phy;
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t *s_eth_netif;
static esp_netif_t *s_wifi_netif;
/* The menu's access point, made on first use and kept like the driver. */
static esp_netif_t *s_ap_netif;
static bool s_eth_ready;
static bool s_wifi_ready;
static bool s_wifi_started;
static bool s_wifi_initialized;
static bool s_hosted_started;
static enum gpsp_route s_route;
static atomic_uint_fast32_t s_generation;

static void gpsp_update_route(void)
{
    enum gpsp_route route = s_eth_ready ? GPSP_ROUTE_ETHERNET :
            s_wifi_ready ? GPSP_ROUTE_WIFI : GPSP_ROUTE_NONE;

    bool changed = route != s_route;

    s_route = route;

    if (route == GPSP_ROUTE_NONE)
        xEventGroupClearBits(s_events, s_ready_bit);
    else
    {
        esp_netif_set_default_netif(
                route == GPSP_ROUTE_ETHERNET ? s_eth_netif : s_wifi_netif);
        xEventGroupSetBits(s_events, s_ready_bit);
    }

    if (changed)
    {
        atomic_fetch_add_explicit(&s_generation, 1, memory_order_release);
        ESP_LOGI(TAG, "Active network: %s",
                route == GPSP_ROUTE_ETHERNET ? "Ethernet" :
                route == GPSP_ROUTE_WIFI ? "Wi-Fi" : "none");
    }
}

static void gpsp_ethernet_event(
        void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == ETHERNET_EVENT_DISCONNECTED)
    {
        s_eth_ready = false;
        gpsp_update_route();
    }
}

static void gpsp_ip_event(
        void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;

    (void)arg;
    (void)base;

    if (id == IP_EVENT_ETH_GOT_IP)
    {
        s_eth_ready = true;
        ESP_LOGI(TAG, "Ethernet address: " IPSTR, IP2STR(&event->ip_info.ip));
    }
    else if (id == IP_EVENT_STA_GOT_IP)
    {
        wifi_ap_record_t ap;

        s_wifi_ready = true;
        ESP_LOGI(TAG, "Wi-Fi address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "Wi-Fi link: %s channel=%u RSSI=%d dBm",
                    ap.primary > 14 ? "5 GHz" : "2.4 GHz", ap.primary, ap.rssi);
    }

    gpsp_update_route();
}

static void gpsp_wifi_event(
        void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            s_wifi_ready = false;
            gpsp_update_route();
        }

        esp_wifi_connect();
    }
}

static esp_err_t gpsp_ethernet_init(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_config_t eth_config;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_err_t result;

    if (!gpsp_hardware.ethernet.enabled)
        return ESP_ERR_NOT_SUPPORTED;

    phy_config.phy_addr = gpsp_hardware.ethernet.phy;
    phy_config.reset_gpio_num = gpsp_hardware.ethernet.reset;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = gpsp_hardware.ethernet.clock;
    emac_config.smi_gpio.mdc_num = gpsp_hardware.ethernet.mdc;
    emac_config.smi_gpio.mdio_num = gpsp_hardware.ethernet.mdio;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = gpsp_hardware.ethernet.crs_dv;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = gpsp_hardware.ethernet.rxd0;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = gpsp_hardware.ethernet.rxd1;
    emac_config.emac_dataif_gpio.rmii.txd0_num = gpsp_hardware.ethernet.txd0;
    emac_config.emac_dataif_gpio.rmii.txd1_num = gpsp_hardware.ethernet.txd1;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = gpsp_hardware.ethernet.tx_en;

    s_mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!s_mac)
        return ESP_ERR_NO_MEM;

    s_phy = esp_eth_phy_new_generic(&phy_config);
    if (!s_phy)
        return ESP_ERR_NO_MEM;

    eth_config = (esp_eth_config_t)ETH_DEFAULT_CONFIG(s_mac, s_phy);
    result = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (result != ESP_OK)
        return result;

    s_eth_netif = esp_netif_new(&netif_config);
    if (!s_eth_netif)
        return ESP_ERR_NO_MEM;

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue)
        return ESP_ERR_NO_MEM;

    result = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (result != ESP_OK)
        return result;

    return esp_eth_start(s_eth_handle);
}

static esp_err_t gpsp_wifi_init(void)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_err_t result;
    size_t ssid_length = strlen(CONFIG_GPSP_WIFI_SSID);
    size_t password_length = strlen(CONFIG_GPSP_WIFI_PASSWORD);

    if (ssid_length == 0)
        return ESP_ERR_INVALID_ARG;

    result = nvs_flash_init();
    if (result != ESP_OK)
        return result;

    result = esp_hosted_init();
    if (result == ESP_OK)
        s_hosted_started = true;
    if (result == ESP_OK)
        result = esp_hosted_connect_to_slave();
    if (result != ESP_OK)
        return result;

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi_netif)
        return ESP_ERR_NO_MEM;

    result = esp_wifi_init(&init_config);
    if (result == ESP_OK)
        s_wifi_initialized = true;
    if (result == ESP_OK)
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK)
        result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK)
        return result;

    if (ssid_length > sizeof(wifi_config.sta.ssid))
        ssid_length = sizeof(wifi_config.sta.ssid);
    if (password_length > sizeof(wifi_config.sta.password))
        password_length = sizeof(wifi_config.sta.password);
    memcpy(wifi_config.sta.ssid, CONFIG_GPSP_WIFI_SSID, ssid_length);
    memcpy(wifi_config.sta.password, CONFIG_GPSP_WIFI_PASSWORD,
            password_length);
#if CONFIG_GPSP_WIFI_PREFER_5GHZ
    /* Scan every channel and take a 5 GHz AP unless it is much weaker. */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi_5g_adjustment =
            CONFIG_GPSP_WIFI_5GHZ_RSSI_BONUS;
#endif

    result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (result == ESP_OK)
        result = esp_wifi_start();
    if (result == ESP_OK)
    {
        s_wifi_started = true;
        result = esp_wifi_set_ps(WIFI_PS_NONE);
    }

    return result;
}

esp_err_t gpsp_network_init(
        EventGroupHandle_t events, EventBits_t ready_bit)
{
    esp_err_t ethernet_result;
    esp_err_t wifi_result = ESP_ERR_INVALID_ARG;
    esp_err_t result;

    s_events = events;
    s_ready_bit = ready_bit;
    s_route = GPSP_ROUTE_NONE;
    atomic_init(&s_generation, 0);

    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return result;

    result = esp_event_handler_register(
            ETH_EVENT, ESP_EVENT_ANY_ID, gpsp_ethernet_event, NULL);
    if (result == ESP_OK)
        result = esp_event_handler_register(
                IP_EVENT, IP_EVENT_ETH_GOT_IP, gpsp_ip_event, NULL);
    if (result == ESP_OK)
        result = esp_event_handler_register(
                IP_EVENT, IP_EVENT_STA_GOT_IP, gpsp_ip_event, NULL);
    if (result == ESP_OK)
        result = esp_event_handler_register(
                WIFI_EVENT, ESP_EVENT_ANY_ID, gpsp_wifi_event, NULL);
    if (result != ESP_OK)
        goto error;

    ethernet_result = gpsp_ethernet_init();
    if (ethernet_result != ESP_OK && gpsp_hardware.ethernet.enabled)
        ESP_LOGW(TAG, "Ethernet init failed: %s",
                esp_err_to_name(ethernet_result));

    if (CONFIG_GPSP_WIFI_SSID[0])
    {
        wifi_result = gpsp_wifi_init();
        if (wifi_result != ESP_OK)
            ESP_LOGW(TAG, "Wi-Fi init failed: %s",
                    esp_err_to_name(wifi_result));
    }

    if (ethernet_result == ESP_OK || wifi_result == ESP_OK)
        return ESP_OK;
    result = ethernet_result;

error:
    gpsp_network_shutdown();
    return result;
}

uint32_t gpsp_network_generation(void)
{
    return (uint32_t)atomic_load_explicit(
            &s_generation, memory_order_acquire);
}

void gpsp_network_shutdown(void)
{
    esp_event_handler_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, gpsp_wifi_event);
    esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, gpsp_ip_event);
    esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, gpsp_ip_event);
    esp_event_handler_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, gpsp_ethernet_event);

    if (s_wifi_started)
    {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized)
    {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_hosted_started)
    {
        esp_hosted_deinit();
        s_hosted_started = false;
    }
    if (s_eth_handle)
        esp_eth_stop(s_eth_handle);

    if (s_eth_glue)
        esp_eth_del_netif_glue(s_eth_glue);
    if (s_eth_netif)
        esp_netif_destroy(s_eth_netif);
    if (s_eth_handle)
        esp_eth_driver_uninstall(s_eth_handle);
    if (s_mac)
        s_mac->del(s_mac);
    if (s_phy)
        s_phy->del(s_phy);
    if (s_wifi_netif)
        esp_netif_destroy_default_wifi(s_wifi_netif);

    s_eth_glue = NULL;
    s_eth_netif = NULL;
    s_eth_handle = NULL;
    s_mac = NULL;
    s_phy = NULL;
    s_wifi_netif = NULL;
    s_eth_ready = false;
    s_wifi_ready = false;
    s_events = NULL;
    s_ready_bit = 0;
    s_route = GPSP_ROUTE_NONE;
}

/* Menu Wi-Fi. The network above runs only for the host viewer; the two
   never run together. */
#define GPSP_WIFI_CONNECTED_BIT BIT0
#define GPSP_WIFI_FAILED_BIT BIT1
#define GPSP_WIFI_RETRY_BIT BIT2
#define GPSP_WIFI_CONNECT_TRIES 3
#define GPSP_WIFI_CONNECT_TIMEOUT_MS 15000

enum gpsp_wifi_join
{
    GPSP_WIFI_IDLE,
    GPSP_WIFI_PREPARING,
    GPSP_WIFI_JOINING,
    GPSP_WIFI_CANCELLED,
};

static EventGroupHandle_t s_menu_events;
/* Published before event handlers start; cancel also runs during open. */
static _Atomic(SemaphoreHandle_t) s_menu_lock;
static bool s_menu_wifi;
static enum gpsp_wifi_join s_menu_join;
static uint8_t s_menu_reason;
static esp_ip4_addr_t s_menu_address;

static void gpsp_menu_wifi_event(
        void *arg, esp_event_base_t base, int32_t id, void *data)
{
    SemaphoreHandle_t lock = atomic_load(&s_menu_lock);

    (void)arg;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP &&
            s_menu_join == GPSP_WIFI_JOINING)
    {
        s_menu_address = ((const ip_event_got_ip_t *)data)->ip_info.ip;
        xEventGroupClearBits(s_menu_events, GPSP_WIFI_RETRY_BIT);
        xEventGroupSetBits(s_menu_events, GPSP_WIFI_CONNECTED_BIT);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED &&
            s_menu_join == GPSP_WIFI_JOINING)
    {
        const wifi_event_sta_disconnected_t *event = data;

        /* The disconnect before a new join can arrive late. */
        if (event->reason != WIFI_REASON_ASSOC_LEAVE)
        {
            s_menu_reason = event->reason;
            xEventGroupClearBits(s_menu_events, GPSP_WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_menu_events, GPSP_WIFI_RETRY_BIT);
        }
    }
    xSemaphoreGive(lock);
}

/* Releases the menu's Wi-Fi driver and interfaces. */
static void gpsp_wifi_teardown(void)
{
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
            gpsp_menu_wifi_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
            gpsp_menu_wifi_event);
    if (s_wifi_started)
    {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized)
    {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_wifi_netif)
    {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }
    if (s_ap_netif)
    {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_hosted_started)
    {
        /* The SD card keeps the shared SDMMC controller; IDF logs the
           refused delete as an error. */
        esp_log_level_t level = esp_log_level_get("SD_HOST");

        esp_log_level_set("SD_HOST", ESP_LOG_NONE);
        esp_hosted_deinit();
        esp_log_level_set("SD_HOST", level);
        s_hosted_started = false;
    }
    s_menu_wifi = false;
}

esp_err_t gpsp_wifi_open(void)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result;

    /* After the first use only the radio stops and starts again, as a
       station even if it was an access point last time. */
    if (s_menu_wifi)
    {
        if (s_wifi_started)
            return ESP_OK;
        result = esp_wifi_set_mode(WIFI_MODE_STA);
        if (result == ESP_OK)
            result = esp_wifi_start();
        s_wifi_started = result == ESP_OK;
        return result;
    }
    if (s_wifi_initialized || s_hosted_started)
        return ESP_ERR_INVALID_STATE;

    if (!atomic_load(&s_menu_lock))
    {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();

        if (!lock)
            return ESP_ERR_NO_MEM;
        atomic_store(&s_menu_lock, lock);
    }
    if (!s_menu_events)
        s_menu_events = xEventGroupCreate();
    if (!s_menu_events)
        return ESP_ERR_NO_MEM;
    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return result;
    result = nvs_flash_init();
    if (result != ESP_OK)
        return result;

    s_menu_wifi = true;
    result = esp_hosted_init();
    if (result == ESP_OK)
    {
        s_hosted_started = true;
        result = esp_hosted_connect_to_slave();
    }
    if (result == ESP_OK)
    {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        if (!s_wifi_netif)
            result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK)
        result = esp_wifi_init(&init_config);
    if (result == ESP_OK)
    {
        s_wifi_initialized = true;
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (result == ESP_OK)
        result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK)
        result = esp_event_handler_register(WIFI_EVENT,
                WIFI_EVENT_STA_DISCONNECTED, gpsp_menu_wifi_event, NULL);
    if (result == ESP_OK)
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                gpsp_menu_wifi_event, NULL);
    if (result == ESP_OK)
        result = esp_wifi_start();
    if (result == ESP_OK)
        s_wifi_started = true;

    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(result));
        gpsp_wifi_teardown();
    }
    return result;
}

size_t gpsp_wifi_scan(struct gpsp_wifi_ap *aps, size_t max)
{
    wifi_ap_record_t *records;
    uint16_t count = (uint16_t)max;
    size_t found = 0;

    if (!s_wifi_started || !max ||
            esp_wifi_scan_start(NULL, true) != ESP_OK)
        return 0;
    records = calloc(count, sizeof(*records));
    if (!records)
    {
        esp_wifi_clear_ap_list();
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK)
        for (uint16_t i = 0; i < count; i++)
        {
            const char *ssid = (const char *)records[i].ssid;
            bool seen = !ssid[0];

            /* One entry per name: the strongest comes first. */
            for (size_t j = 0; j < found && !seen; j++)
                seen = !strcmp(aps[j].ssid, ssid);
            if (seen)
                continue;
            snprintf(aps[found].ssid, sizeof(aps[found].ssid), "%s", ssid);
            aps[found].rssi = records[i].rssi;
            aps[found].secure = records[i].authmode != WIFI_AUTH_OPEN;
            found++;
        }
    free(records);
    return found;
}

/* The WPA2 key of a passphrase: PBKDF2-HMAC-SHA1 over 4096 rounds with
   the SSID as salt, as 64 hex digits. */
static bool gpsp_wifi_psk(const char *ssid, const char *password,
        char *hex, size_t size)
{
    psa_key_derivation_operation_t operation =
            PSA_KEY_DERIVATION_OPERATION_INIT;
    uint8_t key[32];
    bool ok = size > 2 * sizeof(key) &&
            psa_crypto_init() == PSA_SUCCESS &&
            psa_key_derivation_setup(&operation,
                    PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_1)) == PSA_SUCCESS &&
            psa_key_derivation_input_integer(&operation,
                    PSA_KEY_DERIVATION_INPUT_COST, 4096) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&operation,
                    PSA_KEY_DERIVATION_INPUT_SALT, (const uint8_t *)ssid,
                    strlen(ssid)) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&operation,
                    PSA_KEY_DERIVATION_INPUT_PASSWORD, (const uint8_t *)password,
                    strlen(password)) == PSA_SUCCESS &&
            psa_key_derivation_output_bytes(&operation, key, sizeof(key)) ==
                    PSA_SUCCESS;

    psa_key_derivation_abort(&operation);
    if (ok)
        for (size_t i = 0; i < sizeof(key); i++)
            snprintf(hex + 2 * i, 3, "%02x", key[i]);
    memset(key, 0, sizeof(key));
    return ok;
}

esp_err_t gpsp_wifi_connect(const char *ssid, const char *password,
        char *address, size_t size, char *psk, size_t psk_size)
{
    size_t password_length = strlen(password);
    SemaphoreHandle_t lock = atomic_load(&s_menu_lock);
    wifi_config_t wifi_config = {0};
    esp_ip4_addr_t ip;
    EventBits_t bits;
    TickType_t started;
    const TickType_t timeout = pdMS_TO_TICKS(GPSP_WIFI_CONNECT_TIMEOUT_MS);
    esp_err_t result;
    uint8_t reason;
    bool cancelled;

    if (!s_wifi_started || !lock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    s_menu_join = GPSP_WIFI_PREPARING;
    s_menu_reason = 0;
    xEventGroupClearBits(s_menu_events, GPSP_WIFI_CONNECTED_BIT |
            GPSP_WIFI_FAILED_BIT | GPSP_WIFI_RETRY_BIT);
    xSemaphoreGive(lock);
    esp_wifi_disconnect();

    /* The SDK name field needs no terminating zero: 32 characters fit. */
    memcpy(wifi_config.sta.ssid, ssid,
            strnlen(ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, password,
            strnlen(password, sizeof(wifi_config.sta.password)));
#if CONFIG_GPSP_WIFI_PREFER_5GHZ
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi_5g_adjustment =
            CONFIG_GPSP_WIFI_5GHZ_RSSI_BONUS;
#endif
    result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_menu_join == GPSP_WIFI_CANCELLED)
        result = ESP_ERR_INVALID_STATE;
    else if (result == ESP_OK)
        s_menu_join = GPSP_WIFI_JOINING;
    xSemaphoreGive(lock);
    started = xTaskGetTickCount();
    /* This task owns every join RPC; callbacks only wake it. */
    for (unsigned tries = 0; result == ESP_OK; tries++)
    {
        TickType_t elapsed = xTaskGetTickCount() - started;

        xSemaphoreTake(lock, portMAX_DELAY);
        cancelled = s_menu_join == GPSP_WIFI_CANCELLED;
        xSemaphoreGive(lock);
        if (cancelled)
        {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (elapsed >= timeout)
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        result = esp_wifi_connect();
        if (result != ESP_OK)
            break;
        elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout)
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        bits = xEventGroupWaitBits(s_menu_events,
                GPSP_WIFI_CONNECTED_BIT | GPSP_WIFI_FAILED_BIT |
                GPSP_WIFI_RETRY_BIT, pdTRUE, pdFALSE, timeout - elapsed);
        if (bits & GPSP_WIFI_FAILED_BIT)
            result = ESP_ERR_INVALID_STATE;
        else if (bits & GPSP_WIFI_CONNECTED_BIT)
            break;
        else if (bits & GPSP_WIFI_RETRY_BIT)
        {
            if (tries + 1 == GPSP_WIFI_CONNECT_TRIES)
                result = ESP_FAIL;
        }
        else
            result = ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    cancelled = s_menu_join == GPSP_WIFI_CANCELLED;
    s_menu_join = GPSP_WIFI_IDLE;
    reason = s_menu_reason;
    ip = s_menu_address;
    xSemaphoreGive(lock);
    /* The last refusal tells a missing network from a wrong password. */
    if (result != ESP_OK && reason == WIFI_REASON_NO_AP_FOUND)
        result = ESP_ERR_NOT_FOUND;
    else if (result != ESP_OK && (reason == WIFI_REASON_AUTH_FAIL ||
            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_AUTH_EXPIRE))
        result = ESP_ERR_INVALID_ARG;
    psk[0] = '\0';
    if (cancelled)
        result = ESP_ERR_INVALID_STATE;
    if (result != ESP_OK)
    {
        esp_wifi_disconnect();
        return result;
    }
    snprintf(address, size, IPSTR, IP2STR(&ip));

    /* A WPA2 passphrase can be kept as its key instead. WPA3 needs the
       passphrase itself, and 64 hex digits already are a key. */
    wifi_ap_record_t ap;

    if (password_length >= 8 && password_length <= 63 &&
            esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            (ap.authmode == WIFI_AUTH_WPA2_PSK ||
                    ap.authmode == WIFI_AUTH_WPA_WPA2_PSK) &&
            !gpsp_wifi_psk(ssid, password, psk, psk_size))
        psk[0] = '\0';
    return ESP_OK;
}

void gpsp_wifi_cancel(void)
{
    SemaphoreHandle_t lock = atomic_load(&s_menu_lock);

    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_menu_join != GPSP_WIFI_IDLE)
    {
        /* The worker finishes any RPC in flight, then disconnects. */
        s_menu_join = GPSP_WIFI_CANCELLED;
        xEventGroupSetBits(s_menu_events, GPSP_WIFI_FAILED_BIT);
    }
    xSemaphoreGive(lock);
}

esp_err_t gpsp_wifi_start_ap(const char *ssid, const char *password,
        char *address, size_t size)
{
    wifi_config_t config = {0};
    esp_netif_ip_info_t ip;
    esp_err_t result;

    if (!s_menu_wifi)
        return ESP_ERR_INVALID_STATE;
    if (!s_ap_netif)
        s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif)
        return ESP_ERR_NO_MEM;

    memcpy(config.ap.ssid, ssid, strnlen(ssid, sizeof(config.ap.ssid)));
    config.ap.ssid_len = (uint8_t)strnlen(ssid, sizeof(config.ap.ssid));
    memcpy(config.ap.password, password,
            strnlen(password, sizeof(config.ap.password)));
    config.ap.channel = 6;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;

    /* The mode changes only while the radio is off. */
    gpsp_wifi_close();
    result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK)
        result = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (result == ESP_OK)
        result = esp_wifi_start();
    s_wifi_started = result == ESP_OK;
    if (result == ESP_OK)
        result = esp_netif_get_ip_info(s_ap_netif, &ip);
    if (result == ESP_OK)
        snprintf(address, size, IPSTR, IP2STR(&ip.ip));
    return result;
}

/* Stops the radio. The co-processor link and the driver stay: making
   them again takes a large block of internal memory each time, and the
   heap soon has none left. */
void gpsp_wifi_close(void)
{
    if (!s_wifi_started)
        return;
    gpsp_wifi_cancel();
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_started = false;
}

void gpsp_wifi_shutdown(void)
{
    /* The caller has joined the menu and transfer workers. */
    if (s_menu_wifi)
        gpsp_wifi_teardown();
    if (s_menu_events)
    {
        vEventGroupDelete(s_menu_events);
        s_menu_events = NULL;
    }
    SemaphoreHandle_t lock = atomic_exchange(&s_menu_lock, NULL);

    if (lock)
        vSemaphoreDelete(lock);
    s_menu_join = GPSP_WIFI_IDLE;
    s_menu_reason = 0;
    s_menu_address.addr = 0;
}

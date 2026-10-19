/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_virtual_network.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_log.h"

static const char *TAG = "gpsp_espemu";
static EventGroupHandle_t s_network_events;
static EventBits_t s_ready_bit;
static esp_eth_handle_t s_eth_handle;
static esp_eth_mac_t *s_eth_mac;
static esp_eth_phy_t *s_eth_phy;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;

/* Mark the virtual network unavailable after link loss. The transport task
 * must wait for a new DHCP lease before opening another host connection. */
static void gpsp_espemu_eth_event(
        void *arg, esp_event_base_t event_base, int32_t event_id,
        void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ETHERNET_EVENT_DISCONNECTED && s_network_events)
        xEventGroupClearBits(s_network_events, s_ready_bit);
}

static void gpsp_espemu_got_ip(
        void *arg, esp_event_base_t event_base, int32_t event_id,
        void *event_data)
{
    const ip_event_got_ip_t *event = event_data;

    (void)arg;
    (void)event_base;
    (void)event_id;

    ESP_LOGI(TAG, "Ethernet address: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_network_events, s_ready_bit);
}

/* Create the ESP-IDF Ethernet driver used by ESP-EMU user networking. */
esp_err_t gpsp_virtual_network_init(EventGroupHandle_t events, EventBits_t bit)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_config_t eth_config;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_err_t result;

    s_network_events = events;
    s_ready_bit = bit;

    /* ESP-EMU models the on-chip EMAC and an IEEE 802.3 PHY. It does not need
     * a physical board's PHY reset pin or vendor-specific initialization. */
    phy_config.reset_gpio_num = -1;

#if CONFIG_IDF_TARGET_ESP32S31
    /* The S31 default describes a physical RGMII reference board and requests
     * a 125 MHz clock that ESP-EMU does not need. Select a valid S31 RMII pin
     * set so the emulated on-chip MAC can start with an external clock input. */
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 13;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = 12;
    emac_config.emac_dataif_gpio.rmii.txd0_num = 8;
    emac_config.emac_dataif_gpio.rmii.txd1_num = 9;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = 15;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = 19;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = 18;
#endif

    s_eth_mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    if (!s_eth_mac)
        return ESP_FAIL;

    s_eth_phy = esp_eth_phy_new_generic(&phy_config);

    if (!s_eth_phy)
    {
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        return ESP_FAIL;
    }

    eth_config = (esp_eth_config_t)ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    result = esp_eth_driver_install(&eth_config, &s_eth_handle);

    if (result != ESP_OK)
        return result;

    result = esp_netif_init();

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        return result;

    result = esp_event_loop_create_default();

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
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

    result = esp_event_handler_register(
            ETH_EVENT, ESP_EVENT_ANY_ID, gpsp_espemu_eth_event, NULL);

    if (result != ESP_OK)
        return result;

    result = esp_event_handler_register(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, gpsp_espemu_got_ip, NULL);

    if (result != ESP_OK)
    {
        esp_event_handler_unregister(
                ETH_EVENT, ESP_EVENT_ANY_ID, gpsp_espemu_eth_event);
        return result;
    }

    return esp_eth_start(s_eth_handle);
}

void gpsp_virtual_network_shutdown(void)
{
    if (s_eth_handle)
        esp_eth_stop(s_eth_handle);

    esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, gpsp_espemu_got_ip);
    esp_event_handler_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, gpsp_espemu_eth_event);

    if (s_eth_glue)
    {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_eth_netif)
    {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    if (s_eth_handle)
    {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }

    if (s_eth_mac)
    {
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
    }

    if (s_eth_phy)
    {
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
    }
}

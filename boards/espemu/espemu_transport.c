/* TCP transport between gpSP and the host viewer.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "espemu_transport.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_ESPEMU
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#else
#include "p4_nano_network.h"
#endif
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "espemu_protocol.h"
#include "gpsp_profile.h"

#if CONFIG_GPSP_BENCHMARK
/* Absorb host scheduling bursts without changing per-frame copy work. */
#define GPSP_ESPEMU_FRAME_BUFFER_COUNT 8
#else
#define GPSP_ESPEMU_FRAME_BUFFER_COUNT 2
#endif
#define GPSP_ESPEMU_IP_READY_BIT BIT0
#define GPSP_ESPEMU_TASK_STOPPED_BIT BIT1
#define GPSP_ESPEMU_INPUT_TIMEOUT_US 500000
#define GPSP_ESPEMU_REPORT_INTERVAL_US 1000000
#define GPSP_ESPEMU_RX_BUFFER_SIZE 128
#define GPSP_ESPEMU_COMMANDS 16
/* Leave room for ordered failures without stopping buttons and PING. */
#define GPSP_ESPEMU_COMMAND_RESULTS (GPSP_ESPEMU_COMMANDS * 2)

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define GPSP_ESPEMU_TRANSPORT_CORE 1
#else
#define GPSP_ESPEMU_TRANSPORT_CORE 0
#endif

struct gpsp_espemu_frame
{
    /* Index identifies a PSRAM buffer owned by exactly one queue or task. */
    uint8_t index;
    uint64_t timestamp_us;
};

static const char *TAG = "gpsp_espemu";
static uint8_t *s_frame_buffers[GPSP_ESPEMU_FRAME_BUFFER_COUNT];
static QueueHandle_t s_free_frames;
static QueueHandle_t s_ready_frames;
static EventGroupHandle_t s_network_events;
static TaskHandle_t s_transport_task;
#if CONFIG_ESPEMU
static esp_eth_handle_t s_eth_handle;
static esp_eth_mac_t *s_eth_mac;
static esp_eth_phy_t *s_eth_phy;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;
#endif
static uint32_t s_frame_number;
static atomic_bool s_running;
static atomic_bool s_connected;
static atomic_uint_fast32_t s_input_bits;
/* A slot stays until its result is sent, including time in a child screen. */
static struct
{
    uint32_t id;
    uint16_t command;
    bool dispatched;
    /* Zero is pending, one is success, two is failure. */
    uint8_t result;
} s_commands[GPSP_ESPEMU_COMMAND_RESULTS];
static unsigned s_command_head;
static unsigned s_command_count;
static uint32_t s_command_session;
static uint32_t s_command_id;
static portMUX_TYPE s_command_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint_fast32_t s_frames_submitted;
static atomic_uint_fast32_t s_frames_sent;
static atomic_uint_fast32_t s_frames_dropped;
static atomic_uint_fast32_t s_input_messages;

#if CONFIG_GPSP_BENCHMARK
bool gpsp_espemu_transport_connected(void)
{
#if !CONFIG_ESPEMU
    if (!CONFIG_GPSP_SERVER_HOST[0])
        return true;
#endif
    return atomic_load_explicit(&s_connected, memory_order_acquire);
}

void gpsp_espemu_transport_get_output_stats(
        uint32_t *submitted, uint32_t *sent, uint32_t *dropped)
{
    *submitted = (uint32_t)atomic_load_explicit(
            &s_frames_submitted, memory_order_relaxed);
    *sent = (uint32_t)atomic_load_explicit(
            &s_frames_sent, memory_order_relaxed);
    *dropped = (uint32_t)atomic_load_explicit(
            &s_frames_dropped, memory_order_relaxed);
}
#endif

/* Mark the virtual network unavailable after link loss. The transport task
 * must wait for a new DHCP lease before opening another host connection. */
#if CONFIG_ESPEMU
static void gpsp_espemu_eth_event(
        void *arg, esp_event_base_t event_base, int32_t event_id,
        void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ETHERNET_EVENT_DISCONNECTED && s_network_events)
        xEventGroupClearBits(s_network_events, GPSP_ESPEMU_IP_READY_BIT);
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
    xEventGroupSetBits(s_network_events, GPSP_ESPEMU_IP_READY_BIT);
}

/* Create the ESP-IDF Ethernet driver used by ESP-EMU user networking. */
static esp_err_t gpsp_espemu_ethernet_init(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_config_t eth_config;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_err_t result;

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

static void gpsp_espemu_ethernet_shutdown(void)
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
#endif

static esp_err_t gpsp_espemu_network_init(void)
{
#if CONFIG_ESPEMU
    return gpsp_espemu_ethernet_init();
#else
    return gpsp_p4_nano_network_init(
            s_network_events, GPSP_ESPEMU_IP_READY_BIT);
#endif
}

static void gpsp_espemu_network_shutdown(void)
{
#if CONFIG_ESPEMU
    gpsp_espemu_ethernet_shutdown();
#else
    gpsp_p4_nano_network_shutdown();
#endif
}

static uint32_t gpsp_espemu_network_generation(void)
{
#if CONFIG_ESPEMU
    return 0;
#else
    return gpsp_p4_nano_network_generation();
#endif
}

/* Open one outbound TCP connection to the viewer.
 *
 * @return A connected socket, or -1 when the host is unavailable.
 */
static int gpsp_espemu_open_host_socket(void)
{
    struct sockaddr_in address = {0};
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 250000,
    };
    int no_delay = 1;
    int socket_fd;
    const char *host;
    int port;

#if CONFIG_ESPEMU
    host = CONFIG_GPSP_ESPEMU_HOST;
    port = CONFIG_GPSP_ESPEMU_PORT;
    /* In ESP-EMU user networking, 192.168.4.1 routes back to the host. An
     * outbound guest connection avoids platform-specific port forwarding. */
#else
    host = CONFIG_GPSP_SERVER_HOST;
    port = CONFIG_GPSP_SERVER_PORT;
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        ESP_LOGE(TAG, "Invalid host address: %s", host);
        return -1;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (socket_fd < 0)
        return -1;

    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
            &no_delay, sizeof(no_delay));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
            &timeout, sizeof(timeout));

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static bool gpsp_espemu_send_all(
        int socket_fd, const void *data, size_t size)
{
    const uint8_t *source = data;

    /* TCP may accept only part of a frame. This loop is confined to the
     * transport task and can never stall the emulation task. */
    while (size > 0 && atomic_load_explicit(&s_running, memory_order_relaxed))
    {
        int sent = send(socket_fd, source, size, 0);

        if (sent <= 0)
            return false;

        source += sent;
        size -= (size_t)sent;
    }

    return size == 0;
}

/* Describe the fixed video stream before sending any frame data. */
static bool gpsp_espemu_send_hello(int socket_fd, uint32_t sequence)
{
    uint8_t message[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_HELLO_SIZE];
    uint8_t *payload = message + GPSP_ESPEMU_HEADER_SIZE;

    gpsp_espemu_write_header(message, GPSP_ESPEMU_MESSAGE_HELLO,
            GPSP_ESPEMU_HELLO_SIZE, sequence, esp_timer_get_time());
    gpsp_espemu_write_u16(payload, GPSP_ESPEMU_SCREEN_WIDTH);
    gpsp_espemu_write_u16(payload + 2, GPSP_ESPEMU_SCREEN_HEIGHT);
    gpsp_espemu_write_u16(payload + 4, GPSP_ESPEMU_PIXEL_RGB565_LE);
    gpsp_espemu_write_u16(payload + 6, 0);
    gpsp_espemu_write_u32(payload + 8, 59728);

    return gpsp_espemu_send_all(socket_fd, message, sizeof(message));
}

static bool gpsp_espemu_send_frame(
        int socket_fd, const struct gpsp_espemu_frame *frame,
        uint32_t sequence)
{
    uint8_t prefix[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_FRAME_INFO_SIZE];
    uint8_t *frame_info = prefix + GPSP_ESPEMU_HEADER_SIZE;

    gpsp_espemu_write_header(prefix, GPSP_ESPEMU_MESSAGE_FRAME,
            GPSP_ESPEMU_FRAME_INFO_SIZE + GPSP_ESPEMU_FRAME_BYTES,
            sequence, frame->timestamp_us);
    gpsp_espemu_write_u16(frame_info, GPSP_ESPEMU_SCREEN_WIDTH);
    gpsp_espemu_write_u16(frame_info + 2, GPSP_ESPEMU_SCREEN_HEIGHT);
    gpsp_espemu_write_u16(frame_info + 4, GPSP_ESPEMU_SCREEN_PITCH);
    gpsp_espemu_write_u16(frame_info + 6, GPSP_ESPEMU_PIXEL_RGB565_LE);

    return gpsp_espemu_send_all(socket_fd, prefix, sizeof(prefix)) &&
            gpsp_espemu_send_all(socket_fd,
                    s_frame_buffers[frame->index], GPSP_ESPEMU_FRAME_BYTES);
}

static uint32_t gpsp_espemu_profile_average(
        const struct gpsp_profile_phase *phase)
{
    return phase->count == 0 ? 0 :
            (uint32_t)(phase->total_us / phase->count);
}

static bool gpsp_espemu_send_profile(
        int socket_fd, const struct gpsp_profile_window *window,
        uint32_t submitted, uint32_t sent, uint32_t dropped,
        uint32_t sequence)
{
    uint8_t message[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_PROFILE_SIZE];
    uint8_t *payload = message + GPSP_ESPEMU_HEADER_SIZE;

    gpsp_espemu_write_header(message, GPSP_ESPEMU_MESSAGE_PROFILE,
            GPSP_ESPEMU_PROFILE_SIZE, sequence, window->end_us);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_INTERVAL_US,
            (uint32_t)(window->end_us - window->start_us));
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_FRAMES,
            window->run.count);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_RUN_AVG_US,
            gpsp_espemu_profile_average(&window->run));
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_RUN_MAX_US,
            window->run.max_us);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_WAIT_AVG_US,
            gpsp_espemu_profile_average(&window->wait));
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_WAIT_MAX_US,
            window->wait.max_us);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_COPY_COUNT,
            window->copy.count);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_COPY_AVG_US,
            gpsp_espemu_profile_average(&window->copy));
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_COPY_MAX_US,
            window->copy.max_us);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_SEND_COUNT,
            window->send.count);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_SEND_AVG_US,
            gpsp_espemu_profile_average(&window->send));
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_SEND_MAX_US,
            window->send.max_us);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_SUBMITTED,
            submitted);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_SENT, sent);
    gpsp_espemu_write_u32(payload + GPSP_ESPEMU_PROFILE_DROPPED,
            dropped);
    return gpsp_espemu_send_all(socket_fd, message, sizeof(message));
}

static bool gpsp_espemu_send_pong(int socket_fd, uint32_t sequence)
{
    uint8_t header[GPSP_ESPEMU_HEADER_SIZE];

    gpsp_espemu_write_header(header, GPSP_ESPEMU_MESSAGE_PONG,
            0, sequence, esp_timer_get_time());
    return gpsp_espemu_send_all(socket_fd, header, sizeof(header));
}

static bool gpsp_espemu_send_state_status(
        int socket_fd, uint32_t sequence, uint32_t status)
{
    uint8_t message[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_STATE_STATUS_SIZE];

    gpsp_espemu_write_header(message, GPSP_ESPEMU_MESSAGE_STATE_STATUS,
            GPSP_ESPEMU_STATE_STATUS_SIZE, sequence, esp_timer_get_time());
    gpsp_espemu_write_u16(message + GPSP_ESPEMU_HEADER_SIZE,
            (uint16_t)(status >> 16));
    gpsp_espemu_write_u16(message + GPSP_ESPEMU_HEADER_SIZE + 2,
            (uint16_t)status);
    return gpsp_espemu_send_all(socket_fd, message, sizeof(message));
}

static void gpsp_espemu_command_session(bool connected)
{
    portENTER_CRITICAL(&s_command_lock);
    if (!++s_command_session)
        s_command_session++;
    s_command_head = 0;
    s_command_count = 0;
    atomic_store_explicit(&s_connected, connected, memory_order_release);
    portEXIT_CRITICAL(&s_command_lock);
}

static bool gpsp_espemu_queue_command(uint16_t command)
{
    bool queued = false;

    portENTER_CRITICAL(&s_command_lock);
    if (s_command_count < GPSP_ESPEMU_COMMAND_RESULTS)
    {
        unsigned index = (s_command_head + s_command_count) %
                GPSP_ESPEMU_COMMAND_RESULTS;
        bool rejected = s_command_count >= GPSP_ESPEMU_COMMANDS;

        s_commands[index].id = ++s_command_id;
        s_commands[index].command = command;
        s_commands[index].dispatched = rejected;
        s_commands[index].result = rejected ? 2 : 0;
        s_command_count++;
        queued = true;
    }
    portEXIT_CRITICAL(&s_command_lock);
    return queued;
}

static bool gpsp_espemu_send_command_results(int socket_fd, uint32_t *sequence)
{
    /* Bound the work so reporting cannot starve input and frame delivery. */
    for (unsigned i = 0; i < GPSP_ESPEMU_COMMANDS; i++)
    {
        uint32_t status = 0;

        portENTER_CRITICAL(&s_command_lock);
        if (s_command_count && s_commands[s_command_head].result)
            status = ((uint32_t)s_commands[s_command_head].command << 16) |
                    (s_commands[s_command_head].result - 1u);
        portEXIT_CRITICAL(&s_command_lock);
        if (!status)
            break;
        if (!gpsp_espemu_send_state_status(socket_fd, (*sequence)++, status))
            return false;
        portENTER_CRITICAL(&s_command_lock);
        s_command_head = (s_command_head + 1) % GPSP_ESPEMU_COMMAND_RESULTS;
        s_command_count--;
        portEXIT_CRITICAL(&s_command_lock);
    }
    return true;
}

static bool gpsp_espemu_receive_messages(
        int socket_fd, uint8_t *buffer, size_t *buffer_size,
        int64_t *last_input_us, uint32_t *sequence)
{
    int received;

    /* Keep incomplete TCP messages between calls. The host currently sends
     * only the fixed-size INPUT message and an optional empty PING. */
    received = recv(socket_fd, buffer + *buffer_size,
            GPSP_ESPEMU_RX_BUFFER_SIZE - *buffer_size, MSG_DONTWAIT);

    if (received == 0)
        return false;

    if (received < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
        received = 0;
    }

    *buffer_size += (size_t)received;

    while (*buffer_size >= GPSP_ESPEMU_HEADER_SIZE)
    {
        uint32_t magic = gpsp_espemu_read_u32(buffer);
        uint16_t version = gpsp_espemu_read_u16(buffer + 4);
        uint16_t type = gpsp_espemu_read_u16(buffer + 6);
        uint32_t payload_size = gpsp_espemu_read_u32(buffer + 8);
        size_t message_size = GPSP_ESPEMU_HEADER_SIZE + payload_size;

        if (magic != GPSP_ESPEMU_PROTOCOL_MAGIC ||
                version != GPSP_ESPEMU_PROTOCOL_VERSION ||
                payload_size > GPSP_ESPEMU_INPUT_SIZE)
            return false;

        if (*buffer_size < message_size)
            break;

        if (type == GPSP_ESPEMU_MESSAGE_INPUT &&
                payload_size == GPSP_ESPEMU_INPUT_SIZE)
        {
            uint16_t input = gpsp_espemu_read_u16(
                    buffer + GPSP_ESPEMU_HEADER_SIZE);
            uint16_t command = gpsp_espemu_read_u16(
                    buffer + GPSP_ESPEMU_HEADER_SIZE + 2);
            uint32_t previous = atomic_exchange_explicit(
                    &s_input_bits, input, memory_order_relaxed);

            atomic_fetch_add_explicit(
                    &s_input_messages, 1, memory_order_relaxed);
            *last_input_us = esp_timer_get_time();

            if (command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
                    command == GPSP_ESPEMU_COMMAND_LOAD_STATE)
            {
                if (!gpsp_espemu_queue_command(command))
                {
                    /* The viewer waits for each result; a flooding peer must
                     * reconnect rather than lose replies or block input. */
                    ESP_LOGW(TAG, "Command replies full; closing connection");
                    return false;
                }
            }

            if (previous != input)
                ESP_LOGI(TAG, "Input changed: bits=0x%03x", input);
        }
        else if (type == GPSP_ESPEMU_MESSAGE_PING && payload_size == 0)
        {
            if (!gpsp_espemu_send_pong(socket_fd, (*sequence)++))
                return false;
        }

        *buffer_size -= message_size;
        memmove(buffer, buffer + message_size, *buffer_size);
    }

    return *buffer_size < GPSP_ESPEMU_RX_BUFFER_SIZE;
}

/* Return every frame still waiting for transmission to the free queue. */
static void gpsp_espemu_drain_frames(void)
{
    struct gpsp_espemu_frame frame;

    while (xQueueReceive(s_ready_frames, &frame, 0) == pdTRUE)
        xQueueSend(s_free_frames, &frame.index, 0);
}

/* Exchange frames and input until the peer or virtual link disconnects. */
static void gpsp_espemu_service_client(int socket_fd)
{
    uint8_t receive_buffer[GPSP_ESPEMU_RX_BUFFER_SIZE];
    size_t receive_size = 0;
    int64_t last_input_us = esp_timer_get_time();
    int64_t last_report_us = last_input_us;
    uint32_t sequence = 0;
    uint32_t last_submitted;
    uint32_t last_sent;
    uint32_t last_dropped;
    uint32_t network_generation = gpsp_espemu_network_generation();

    if (!gpsp_espemu_send_hello(socket_fd, sequence++))
        return;

    /* Frames are accepted only after HELLO reaches the host. */
    gpsp_profile_reset(last_input_us);
    last_submitted = atomic_load_explicit(
            &s_frames_submitted, memory_order_relaxed);
    last_sent = atomic_load_explicit(&s_frames_sent, memory_order_relaxed);
    last_dropped = atomic_load_explicit(
            &s_frames_dropped, memory_order_relaxed);
    gpsp_espemu_command_session(true);
    ESP_LOGI(TAG, "Host viewer connected");

    while (atomic_load_explicit(&s_running, memory_order_relaxed) &&
            (xEventGroupGetBits(s_network_events) &
                    GPSP_ESPEMU_IP_READY_BIT) != 0 &&
            gpsp_espemu_network_generation() == network_generation)
    {
        struct gpsp_espemu_frame frame;
        int64_t now_us;

        if (!gpsp_espemu_receive_messages(socket_fd, receive_buffer,
                &receive_size, &last_input_us, &sequence))
            break;

        if (!gpsp_espemu_send_command_results(socket_fd, &sequence))
            break;

        now_us = esp_timer_get_time();

        if (now_us - last_input_us > GPSP_ESPEMU_INPUT_TIMEOUT_US)
        {
            uint32_t previous = atomic_exchange_explicit(
                    &s_input_bits, 0, memory_order_relaxed);

            /* Never leave a button held after the host stops responding. */
            if (previous != 0)
                ESP_LOGW(TAG, "Input timed out; released all buttons");
        }

        if (xQueueReceive(s_ready_frames, &frame, 0) == pdTRUE)
        {
            int64_t send_start_us = esp_timer_get_time();
            bool sent = gpsp_espemu_send_frame(socket_fd, &frame, sequence++);
            int64_t send_end_us = esp_timer_get_time();

            xQueueSend(s_free_frames, &frame.index, portMAX_DELAY);

            if (!sent)
                break;

            gpsp_profile_record_send((uint32_t)(send_end_us - send_start_us));
            atomic_fetch_add_explicit(
                    &s_frames_sent, 1, memory_order_relaxed);
        }

        now_us = esp_timer_get_time();

        if (now_us - last_report_us >= GPSP_ESPEMU_REPORT_INTERVAL_US)
        {
            struct gpsp_profile_window window;
            uint32_t submitted = atomic_load_explicit(
                    &s_frames_submitted, memory_order_relaxed);
            uint32_t sent = atomic_load_explicit(
                    &s_frames_sent, memory_order_relaxed);
            uint32_t dropped = atomic_load_explicit(
                    &s_frames_dropped, memory_order_relaxed);

            gpsp_profile_take(now_us, &window);

            if (!gpsp_espemu_send_profile(socket_fd, &window,
                    submitted - last_submitted, sent - last_sent,
                    dropped - last_dropped, sequence++))
                break;

            ESP_LOGI(TAG,
                    "Profile: run=%lu/%lu wait=%lu/%lu copy=%lu/%lu "
                    "send=%lu/%lu us avg/max frames=%lu submitted=%lu "
                    "sent=%lu dropped=%lu input_messages=%lu input=0x%03lx",
                    (unsigned long)gpsp_espemu_profile_average(&window.run),
                    (unsigned long)window.run.max_us,
                    (unsigned long)gpsp_espemu_profile_average(&window.wait),
                    (unsigned long)window.wait.max_us,
                    (unsigned long)gpsp_espemu_profile_average(&window.copy),
                    (unsigned long)window.copy.max_us,
                    (unsigned long)gpsp_espemu_profile_average(&window.send),
                    (unsigned long)window.send.max_us,
                    (unsigned long)window.run.count,
                    (unsigned long)(submitted - last_submitted),
                    (unsigned long)(sent - last_sent),
                    (unsigned long)(dropped - last_dropped),
                    (unsigned long)atomic_load_explicit(
                            &s_input_messages, memory_order_relaxed),
                    (unsigned long)atomic_load_explicit(
                            &s_input_bits, memory_order_relaxed));
            last_submitted = submitted;
            last_sent = sent;
            last_dropped = dropped;
            last_report_us = now_us;
        }

        /* pdMS_TO_TICKS(2) is zero with a 100 Hz FreeRTOS tick. One explicit
         * tick guarantees that this task cannot starve its core's idle task. */
        vTaskDelay(1);
    }

    gpsp_espemu_command_session(false);
    atomic_store_explicit(&s_input_bits, 0, memory_order_relaxed);
    gpsp_espemu_drain_frames();
    ESP_LOGI(TAG, "Host viewer disconnected");
}

/* Reconnect for the lifetime of the frontend without blocking emulation. */
static void gpsp_espemu_transport_task(void *arg)
{
    const char *host;
    int port;

    (void)arg;

#if CONFIG_ESPEMU
    host = CONFIG_GPSP_ESPEMU_HOST;
    port = CONFIG_GPSP_ESPEMU_PORT;
#else
    host = CONFIG_GPSP_SERVER_HOST;
    port = CONFIG_GPSP_SERVER_PORT;
#endif

    ESP_LOGI(TAG,
            "Transport task: core=%d priority=%u tick_rate=%u Hz",
            xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL),
            (unsigned)CONFIG_FREERTOS_HZ);
    ESP_LOGI(TAG, "Waiting for host viewer at %s:%d",
            host, port);

    while (atomic_load_explicit(&s_running, memory_order_relaxed))
    {
        EventBits_t bits = xEventGroupWaitBits(s_network_events,
                GPSP_ESPEMU_IP_READY_BIT, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(100));

        if ((bits & GPSP_ESPEMU_IP_READY_BIT) != 0)
        {
            int socket_fd = gpsp_espemu_open_host_socket();

            if (socket_fd >= 0)
            {
                gpsp_espemu_service_client(socket_fd);
                close(socket_fd);
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }

    xEventGroupSetBits(s_network_events, GPSP_ESPEMU_TASK_STOPPED_BIT);
    vTaskDelete(NULL);
}

esp_err_t gpsp_espemu_transport_init(void)
{
    esp_err_t result;
    uint8_t index;

    atomic_init(&s_running, true);
    atomic_init(&s_connected, false);
    atomic_init(&s_input_bits, 0);
    s_command_head = s_command_count = 0;
    s_command_session = s_command_id = 0;
    atomic_init(&s_frames_submitted, 0);
    atomic_init(&s_frames_sent, 0);
    atomic_init(&s_frames_dropped, 0);
    atomic_init(&s_input_messages, 0);
    s_transport_task = NULL;
    s_frame_number = 0;

#if !CONFIG_ESPEMU
    if (!CONFIG_GPSP_SERVER_HOST[0])
    {
        ESP_LOGI(TAG, "Frame receiver address is empty; transport disabled");
        return ESP_OK;
    }
#endif

    s_free_frames = xQueueCreate(
            GPSP_ESPEMU_FRAME_BUFFER_COUNT, sizeof(uint8_t));
    s_ready_frames = xQueueCreate(
            GPSP_ESPEMU_FRAME_BUFFER_COUNT - 1,
            sizeof(struct gpsp_espemu_frame));
    s_network_events = xEventGroupCreate();

    if (!s_free_frames || !s_ready_frames || !s_network_events)
    {
        result = ESP_ERR_NO_MEM;
        goto error;
    }

    /* One buffer can be sent while the rest wait in the queue. */
    for (index = 0; index < GPSP_ESPEMU_FRAME_BUFFER_COUNT; index++)
    {
        s_frame_buffers[index] = heap_caps_malloc(
                GPSP_ESPEMU_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!s_frame_buffers[index])
        {
            result = ESP_ERR_NO_MEM;
            goto error;
        }

        xQueueSend(s_free_frames, &index, 0);
    }

    result = gpsp_espemu_network_init();

    if (result != ESP_OK)
        goto error;

    /* Keep host I/O away from the CPU0 emulation task on dual-core targets. */
    if (xTaskCreatePinnedToCore(
            gpsp_espemu_transport_task, "gpsp_transport", 6144, NULL,
            tskIDLE_PRIORITY + 1, &s_transport_task,
            GPSP_ESPEMU_TRANSPORT_CORE) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto error;
    }

    return ESP_OK;

error:
    atomic_store_explicit(&s_running, false, memory_order_relaxed);
    gpsp_espemu_network_shutdown();

    for (index = 0; index < GPSP_ESPEMU_FRAME_BUFFER_COUNT; index++)
    {
        heap_caps_free(s_frame_buffers[index]);
        s_frame_buffers[index] = NULL;
    }

    if (s_free_frames)
    {
        vQueueDelete(s_free_frames);
        s_free_frames = NULL;
    }

    if (s_ready_frames)
    {
        vQueueDelete(s_ready_frames);
        s_ready_frames = NULL;
    }

    if (s_network_events)
    {
        vEventGroupDelete(s_network_events);
        s_network_events = NULL;
    }

    return result;
}

bool gpsp_espemu_transport_submit_frame(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch)
{
    struct gpsp_espemu_frame frame;
    struct gpsp_espemu_frame waiting_frame;
    uint8_t index;
    unsigned y;
    int64_t copy_start_us;

#if !CONFIG_ESPEMU
    s_frame_number++;
#if CONFIG_GPSP_BENCHMARK
    if (s_frame_number <= CONFIG_GPSP_BENCHMARK_FRAMES)
    {
        if (s_frame_number != 64 &&
                s_frame_number != CONFIG_GPSP_BENCHMARK_FRAMES)
            return false;
    }
#if CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT
    else
        return false;
#else
    else if (s_frame_number % 6 != 0)
        return false;
#endif
#else
    if (s_frame_number % 6 != 0)
        return false;
#endif
#endif

    if (!pixels || width != GPSP_ESPEMU_SCREEN_WIDTH ||
            height != GPSP_ESPEMU_SCREEN_HEIGHT ||
            pitch < GPSP_ESPEMU_SCREEN_PITCH ||
            !atomic_load_explicit(&s_connected, memory_order_acquire))
        return false;

    atomic_fetch_add_explicit(
            &s_frames_submitted, 1, memory_order_relaxed);

    if (xQueueReceive(s_free_frames, &index, 0) != pdTRUE)
    {
        /* Replace the oldest waiting frame. The TCP task owns its buffer. */
        if (xQueueReceive(s_ready_frames, &frame, 0) != pdTRUE)
        {
            atomic_fetch_add_explicit(
                    &s_frames_dropped, 1, memory_order_relaxed);
            return false;
        }

        index = frame.index;
        atomic_fetch_add_explicit(
                &s_frames_dropped, 1, memory_order_relaxed);
    }

    copy_start_us = esp_timer_get_time();

    for (y = 0; y < GPSP_ESPEMU_SCREEN_HEIGHT; y++)
    {
        memcpy(s_frame_buffers[index] + y * GPSP_ESPEMU_SCREEN_PITCH,
                (const uint8_t *)pixels + y * pitch,
                GPSP_ESPEMU_SCREEN_PITCH);
    }

    gpsp_profile_record_copy(
            (uint32_t)(esp_timer_get_time() - copy_start_us));

    frame.index = index;
    frame.timestamp_us = esp_timer_get_time();

    if (xQueueSend(s_ready_frames, &frame, 0) != pdTRUE)
    {
        /* A free buffer and a queued frame can coexist while the transport is
         * idle. Remove that older frame and retry with the new one. */
        if (xQueueReceive(s_ready_frames, &waiting_frame, 0) == pdTRUE)
        {
            xQueueSend(s_free_frames, &waiting_frame.index, 0);
            atomic_fetch_add_explicit(
                    &s_frames_dropped, 1, memory_order_relaxed);
        }

        if (xQueueSend(s_ready_frames, &frame, 0) != pdTRUE)
        {
            xQueueSend(s_free_frames, &index, 0);
            atomic_fetch_add_explicit(
                    &s_frames_dropped, 1, memory_order_relaxed);
            return false;
        }
    }

    return true;
}

uint16_t gpsp_espemu_transport_read_input(void)
{
    return (uint16_t)atomic_load_explicit(
            &s_input_bits, memory_order_relaxed);
}

struct gpsp_board_request gpsp_espemu_transport_read_command(void)
{
    struct gpsp_board_request request = {0};

    portENTER_CRITICAL(&s_command_lock);
    if (atomic_load_explicit(&s_connected, memory_order_acquire))
        for (unsigned i = 0; i < s_command_count; i++)
        {
            unsigned index = (s_command_head + i) % GPSP_ESPEMU_COMMAND_RESULTS;

            if (!s_commands[index].dispatched)
            {
                s_commands[index].dispatched = true;
                request.command = (enum gpsp_board_command)s_commands[index].command;
                request.session = s_command_session;
                request.id = s_commands[index].id;
                break;
            }
        }
    portEXIT_CRITICAL(&s_command_lock);
    return request;
}

bool gpsp_espemu_transport_command_valid(struct gpsp_board_request request)
{
    bool valid;

    portENTER_CRITICAL(&s_command_lock);
    valid = atomic_load_explicit(&s_connected, memory_order_acquire) &&
            request.session == s_command_session;
    portEXIT_CRITICAL(&s_command_lock);
    return valid;
}

void gpsp_espemu_transport_report_command(
        struct gpsp_board_request request, bool success)
{
    portENTER_CRITICAL(&s_command_lock);
    if (atomic_load_explicit(&s_connected, memory_order_acquire) &&
            request.session == s_command_session)
        for (unsigned i = 0; i < s_command_count; i++)
        {
            unsigned index = (s_command_head + i) % GPSP_ESPEMU_COMMAND_RESULTS;

            if (s_commands[index].id == request.id &&
                    s_commands[index].command == request.command &&
                    s_commands[index].dispatched && !s_commands[index].result)
            {
                s_commands[index].result = success ? 1 : 2;
                break;
            }
        }
    portEXIT_CRITICAL(&s_command_lock);
}

void gpsp_espemu_transport_shutdown(void)
{
    unsigned index;

    atomic_store_explicit(&s_running, false, memory_order_relaxed);

#if !CONFIG_ESPEMU
    if (!CONFIG_GPSP_SERVER_HOST[0])
        return;
#endif

    if (s_transport_task)
    {
        xEventGroupWaitBits(s_network_events,
                GPSP_ESPEMU_TASK_STOPPED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        s_transport_task = NULL;
    }

    gpsp_espemu_network_shutdown();

    for (index = 0; index < GPSP_ESPEMU_FRAME_BUFFER_COUNT; index++)
    {
        heap_caps_free(s_frame_buffers[index]);
        s_frame_buffers[index] = NULL;
    }

    if (s_free_frames)
    {
        vQueueDelete(s_free_frames);
        s_free_frames = NULL;
    }

    if (s_ready_frames)
    {
        vQueueDelete(s_ready_frames);
        s_ready_frames = NULL;
    }

    if (s_network_events)
    {
        vEventGroupDelete(s_network_events);
        s_network_events = NULL;
    }
}

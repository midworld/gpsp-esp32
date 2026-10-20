/* USB HCI transport and Bluedroid host loaded when an adapter is attached.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usb_bt.h"

#if CONFIG_GPSP_USB_BT

#include <stdatomic.h>
#include <string.h>
#include "esp_bluedroid_hci.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "usb_bt_packet.h"

#define BT_PRIORITY (tskIDLE_PRIORITY + 7)
#define BT_START_TIMEOUT_US 15000000
#define BT_COMMAND_TIMEOUT_US 4000000
#define BT_RELEASE_TIMEOUT_US 30000000
#define BT_SLOTS 8
#define BT_PENDING 4
#define BT_LINKS 4
#define BT_PROBES 8
#define BT_RESET_TRIES 3
#define BT_RESET_RETRY_US 200000
#define BT_EVENT_SIZE 72
#define BT_HW_FAILURE 0x03

struct bt_endpoints
{
    int interface;
    uint8_t event, acl_in, acl_out;
    unsigned event_mps, acl_mps;
};

struct bt_slot
{
    unsigned size;
    uint8_t data[GPSP_HCI_ACL_MAX + 1];
};

/* Controller replies whose failure aborts Bluedroid start-up. */
struct bt_probe
{
    uint16_t opcode;
    uint8_t page, size;
    uint8_t event[BT_EVENT_SIZE];
};

struct bt_pending
{
    uint16_t opcode;
    int page;
    int64_t sent_us;
};

struct bt_adapter
{
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    uint8_t address;
    struct bt_endpoints endpoints;
    usb_transfer_t *event, *acl_in, *acl_out, *command;
    usb_transfer_t *tx_pending;
    bool event_busy, acl_busy;
    bool claimed, barrot, restart, stack_started, links_closed, rx_open;
    atomic_bool removed, closed, stopping, working, disabled, drained, stack_done,
            stack_failed;
    atomic_int stack_error;
    struct gpsp_hci_packet events, acl;
    struct bt_slot *slots;
    QueueHandle_t free_slots, ready_slots;
    SemaphoreHandle_t rx_lock;
    _Atomic(const esp_bluedroid_hci_driver_callbacks_t *) callbacks;
    uint16_t probe_opcode;
    unsigned probe_size, probe_count, pending_count, link_count;
    uint8_t probe_event[BT_EVENT_SIZE];
    struct bt_probe probes[BT_PROBES];
    struct bt_pending pending[BT_PENDING];
    uint16_t links[BT_LINKS];
    int64_t start_us;
    unsigned errors;
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    /* ACL packets received and completions reported to the controller. */
    unsigned acl_received, acl_done, completed_replies;
#endif
    esp_err_t error;
};

static const char *TAG = "gpsp_usb_bt";
static struct bt_adapter *s_adapter;
static const struct gpsp_usb_bt_profile *s_profile;
static atomic_bool s_active, s_stop, s_stuck;
static atomic_uint s_pending, s_received;
static portMUX_TYPE s_info_lock = portMUX_INITIALIZER_UNLOCKED;
static struct gpsp_usb_bt_info s_info;

static void bt_publish(enum gpsp_usb_bt_state state, esp_err_t error)
{
    portENTER_CRITICAL(&s_info_lock);
    s_info.state = state;
    s_info.error = error;
    portEXIT_CRITICAL(&s_info_lock);
}

void gpsp_usb_bt_get_info(struct gpsp_usb_bt_info *info)
{
    portENTER_CRITICAL(&s_info_lock);
    *info = s_info;
    portEXIT_CRITICAL(&s_info_lock);
    info->received = atomic_load(&s_received);
    info->active = atomic_load(&s_active);
}

void gpsp_usb_bt_set_profile(const struct gpsp_usb_bt_profile *profile)
{
    configASSERT(!atomic_load(&s_active));
    s_profile = profile;
}

static bool bt_find_endpoints(const usb_config_desc_t *config,
        struct bt_endpoints *result)
{
    const usb_standard_desc_t *desc = (const void *)config;
    struct bt_endpoints found = {.interface = -1};
    int offset = 0;

    for (;;)
    {
        desc = usb_parse_next_descriptor(desc, config->wTotalLength, &offset);
        if (!desc || desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
        {
            if (found.event && found.acl_in && found.acl_out &&
                    found.event_mps && found.acl_mps)
            {
                *result = found;
                return true;
            }
            found = (struct bt_endpoints){.interface = -1};
            if (!desc)
                return false;
            const usb_intf_desc_t *intf = (const void *)desc;

            if (intf->bAlternateSetting == 0 &&
                    intf->bInterfaceClass == 0xe0 &&
                    intf->bInterfaceSubClass == 1 &&
                    intf->bInterfaceProtocol == 1)
                found.interface = intf->bInterfaceNumber;
        }
        else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                found.interface >= 0)
        {
            const usb_ep_desc_t *ep = (const void *)desc;
            unsigned type = USB_EP_DESC_GET_XFERTYPE(ep);
            bool input = USB_EP_DESC_GET_EP_DIR(ep);

            if (type == USB_TRANSFER_TYPE_INTR && input)
            {
                found.event = ep->bEndpointAddress;
                found.event_mps = USB_EP_DESC_GET_MPS(ep);
            }
            else if (type == USB_TRANSFER_TYPE_BULK && input)
            {
                found.acl_in = ep->bEndpointAddress;
                found.acl_mps = USB_EP_DESC_GET_MPS(ep);
            }
            else if (type == USB_TRANSFER_TYPE_BULK)
                found.acl_out = ep->bEndpointAddress;
        }
    }
}

bool gpsp_usb_bt_match(const usb_device_desc_t *device,
        const usb_config_desc_t *config)
{
    struct bt_endpoints endpoints;

    (void)device;
    return bt_find_endpoints(config, &endpoints);
}

static void bt_close(struct bt_adapter *bt, esp_err_t error)
{
    if (error && !bt->error)
        bt->error = error;
    atomic_store(&bt->closed, true);
}

/* Bluedroid frees its receive queues during deinit; stop calls first. */
static void bt_deliver(struct bt_adapter *bt, uint8_t *packet, unsigned size)
{
    const esp_bluedroid_hci_driver_callbacks_t *callbacks;

    xSemaphoreTake(bt->rx_lock, portMAX_DELAY);
    callbacks = atomic_load(&bt->callbacks);
    if (bt->rx_open && callbacks && packet)
    {
        atomic_fetch_add(&s_received, 1);
        callbacks->notify_host_recv(packet, size);
    }
    else if (bt->rx_open && callbacks)
        callbacks->notify_host_send_available();
    xSemaphoreGive(bt->rx_lock);
}

#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
/* L2CAP signals on CID 1: code, and the PSM of a connection request or the
   result and status of its response. */
static void bt_log_signal(const char *direction, const uint8_t *packet, unsigned size)
{
    if (packet[0] != 2 || size < 13 || (packet[7] | (packet[8] << 8)) != 1)
        return;
    unsigned code = packet[9], length = packet[11] | (packet[12] << 8);

    if (code == 0x02 && size >= 15)
        ESP_LOGI(TAG, "L2CAP %s connect request psm=%04x", direction,
                packet[13] | (packet[14] << 8));
    else if (code == 0x03 && size >= 21)
        ESP_LOGI(TAG, "L2CAP %s connect response result=%04x status=%04x", direction,
                packet[17] | (packet[18] << 8), packet[19] | (packet[20] << 8));
    else
        ESP_LOGI(TAG, "L2CAP %s signal code=%02x length=%u", direction, code, length);
}
#endif

static void bt_track(struct bt_adapter *bt, const uint8_t *packet, unsigned size)
{
    unsigned opcode = 0;

    if (packet[0] != 4 || size < 3)
        return;
    if (packet[1] == 0x0e && size >= 6)
        opcode = packet[4] | (packet[5] << 8);
    else if (packet[1] == 0x0f && size >= 7)
        opcode = packet[5] | (packet[6] << 8);
    else if (packet[1] == 0x03 && size >= 6 && !packet[3] &&
            bt->link_count < BT_LINKS)
        bt->links[bt->link_count++] = (packet[4] | (packet[5] << 8)) & 0x0fff;
    else if (packet[1] == 0x05 && size >= 6 && !packet[3])
    {
        unsigned handle = (packet[4] | (packet[5] << 8)) & 0x0fff;

        for (unsigned i = 0; i < bt->link_count; i++)
            if (bt->links[i] == handle)
                bt->links[i] = bt->links[--bt->link_count];
    }
    for (unsigned i = 0; opcode && i < bt->pending_count; i++)
        if (bt->pending[i].opcode == opcode)
        {
            memmove(bt->pending + i, bt->pending + i + 1,
                    (--bt->pending_count - i) * sizeof(*bt->pending));
            break;
        }
}

static const struct bt_probe *bt_find_probe(const struct bt_adapter *bt,
        unsigned opcode, int page)
{
    for (unsigned i = 0; i < bt->probe_count; i++)
    {
        const struct bt_probe *probe = &bt->probes[i];

        if (probe->opcode == opcode && (opcode != 0x1004 || probe->page == page))
            return probe;
    }
    return NULL;
}

/* Bluedroid asserts on a failed or short start-up reply. One that differs
   from the probe means a faulty adapter; close it before the host sees it. */
static bool bt_contradicts_probe(const struct bt_adapter *bt,
        const uint8_t *packet, unsigned size)
{
    if (packet[0] != 4 || packet[1] != 0x0e || size < 7)
        return false;
    unsigned opcode = packet[4] | (packet[5] << 8);
    int page = -1;

    /* A failed reply may omit the page; use the one that was sent. */
    for (unsigned i = 0; i < bt->pending_count; i++)
        if (bt->pending[i].opcode == opcode)
        {
            page = bt->pending[i].page;
            break;
        }
    const struct bt_probe *probe = bt_find_probe(bt, opcode, page);

    return probe && (packet[6] || size < probe->size);
}

static void bt_receive(uint8_t *packet, unsigned size, void *arg)
{
    struct bt_adapter *bt = arg;

#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    bt_log_signal("in", packet, size);
    if (packet[0] == 2)
        bt->acl_received++;
    /* Inquiry results and completed-packet counts are frequent. */
    /* Some adapters answer every Host Number Of Completed Packets; sample those. */
    if (packet[0] == 4 && size >= 7 && packet[1] == 0x0e &&
            (packet[4] | (packet[5] << 8)) == 0x0c35)
    {
        if (bt->completed_replies++ % 64 < 4)
            ESP_LOGI(TAG, "HCI complete 0c35 status=%02x ncmd=%u replies=%u acl_in=%u completed=%u",
                    packet[6], packet[3], bt->completed_replies, bt->acl_received,
                    bt->acl_done);
    }
    /* ncmd is how many commands the controller now accepts. */
    else if (packet[0] == 4 && size >= 7 && packet[1] == 0x0e)
        ESP_LOGI(TAG, "HCI complete %04x status=%02x ncmd=%u acl_in=%u completed=%u",
                packet[4] | (packet[5] << 8), packet[6], packet[3], bt->acl_received,
                bt->acl_done);
    else if (packet[0] == 4 && size >= 7 && packet[1] == 0x0f)
        ESP_LOGI(TAG, "HCI status %04x status=%02x ncmd=%u", packet[5] | (packet[6] << 8),
                packet[3], packet[4]);
    else if (packet[0] == 4 && size >= 3 && packet[1] != 0x02 && packet[1] != 0x13 &&
            packet[1] != 0x22 && packet[1] != 0x2f)
        ESP_LOGI(TAG, "HCI event %02x length=%u data=%02x %02x %02x", packet[1],
                packet[2], size > 3 ? packet[3] : 0, size > 4 ? packet[4] : 0,
                size > 5 ? packet[5] : 0);
#endif
    if (bt->probe_opcode)
    {
        if (packet[0] == 4 && packet[1] == 0x0e && size >= 7 &&
                size <= BT_EVENT_SIZE &&
                (packet[4] | (packet[5] << 8)) == bt->probe_opcode)
        {
            memcpy(bt->probe_event, packet, size);
            bt->probe_size = size;
        }
        return;
    }
    if (atomic_load(&bt->closed))
        return;
    /* Host Number Of Completed Packets is not part of the normal command
       flow and the host waits for no reply. Bluedroid takes the credits of
       any Command Complete before matching it, so a reply with none would
       stop every later command. */
    if (packet[0] == 4 && packet[1] == 0x0e && size >= 6 && packet[2] >= 3 &&
            (packet[4] | (packet[5] << 8)) == 0x0c35)
        return;
    if (bt_contradicts_probe(bt, packet, size))
    {
        ESP_LOGW(TAG, "HCI command %04x reply differs from the probe",
                packet[4] | (packet[5] << 8));
        bt_close(bt, ESP_FAIL);
        return;
    }
    bt_track(bt, packet, size);
    bt_deliver(bt, packet, size);
}

static void bt_in_done(usb_transfer_t *transfer)
{
    struct bt_adapter *bt = transfer->context;
    bool event = transfer == bt->event;
    bool *busy = event ? &bt->event_busy : &bt->acl_busy;

    *busy = false;
    if (atomic_load(&bt->closed))
        return;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED)
    {
        ESP_LOGW(TAG, "IN ep=%02x status=%d", transfer->bEndpointAddress,
                transfer->status);
        bt->errors++;
        bt_close(bt, atomic_load(&bt->removed) ? ESP_OK : ESP_FAIL);
        return;
    }
    if (!gpsp_hci_receive(event ? &bt->events : &bt->acl, event ? 4 : 2,
            transfer->data_buffer, transfer->actual_num_bytes, bt->barrot,
            bt_receive, bt))
    {
        ESP_LOGW(TAG, "Invalid HCI packet ep=%02x", transfer->bEndpointAddress);
        bt->errors++;
        bt_close(bt, ESP_FAIL);
        return;
    }
    if (usb_host_transfer_submit(transfer) == ESP_OK)
        *busy = true;
    else
        bt_close(bt, ESP_FAIL);
}

static void bt_out_done(usb_transfer_t *transfer)
{
    struct bt_adapter *bt = transfer->context;

    bt->tx_pending = NULL;
    if (atomic_load(&bt->closed))
        return;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED)
    {
        ESP_LOGW(TAG, "OUT ep=%02x status=%d", transfer->bEndpointAddress,
                transfer->status);
        bt->errors++;
        bt_close(bt, atomic_load(&bt->removed) ? ESP_OK : ESP_FAIL);
    }
}

static esp_err_t bt_submit(struct bt_adapter *bt, const uint8_t *packet,
        unsigned size)
{
    usb_transfer_t *transfer = packet[0] == 1 ? bt->command : bt->acl_out;
    esp_err_t result;

    size--;
    if (packet[0] == 1)
    {
        usb_setup_packet_t *setup = (void *)transfer->data_buffer;

        *setup = (usb_setup_packet_t){.bmRequestType = 0x20, .wLength = size};
        memcpy(transfer->data_buffer + sizeof(*setup), packet + 1, size);
        transfer->num_bytes = sizeof(*setup) + size;
        bt->tx_pending = transfer;
        result = usb_host_transfer_submit_control(bt->client, transfer);
    }
    else
    {
        memcpy(transfer->data_buffer, packet + 1, size);
        transfer->num_bytes = size;
        bt->tx_pending = transfer;
        result = usb_host_transfer_submit(transfer);
    }
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "HCI submit type=%u: %s", packet[0], esp_err_to_name(result));
        bt->tx_pending = NULL;
        bt_close(bt, result);
    }
    return result;
}

/* Bluedroid stops when a start-up reply is missing or fails, so check them
   with a timeout before the host owns the adapter. */
static esp_err_t bt_probe(struct bt_adapter *bt, uint16_t opcode, int page,
        unsigned minimum)
{
    uint8_t command[] = {1, opcode, opcode >> 8, page >= 0, page};
    int64_t end = esp_timer_get_time() + BT_COMMAND_TIMEOUT_US;

    bt->probe_opcode = opcode;
    bt->probe_size = 0;
    if (bt_submit(bt, command, page >= 0 ? 5 : 4) != ESP_OK)
        return bt->error;
    while ((bt->tx_pending || !bt->probe_size) && !atomic_load(&bt->closed) &&
            !atomic_load(&s_stop))
    {
        usb_host_client_handle_events(bt->client, pdMS_TO_TICKS(5));
        if (esp_timer_get_time() >= end)
        {
            ESP_LOGW(TAG, "HCI command %04x timed out", opcode);
            bt_close(bt, ESP_ERR_TIMEOUT);
        }
    }
    bt->probe_opcode = 0;
    if (atomic_load(&bt->closed) || atomic_load(&s_stop))
        return bt->error ? bt->error : ESP_ERR_INVALID_STATE;
    if (bt->probe_size < 7 || bt->probe_event[2] < 4)
    {
        ESP_LOGW(TAG, "HCI command %04x reply is short", opcode);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (bt->probe_event[6])
    {
        ESP_LOGW(TAG, "HCI command %04x failed status=%02x reply=%02x %02x %02x %02x %02x %02x",
                opcode, bt->probe_event[6], bt->probe_event[1], bt->probe_event[2],
                bt->probe_event[3], bt->probe_event[4], bt->probe_event[5],
                bt->probe_event[6]);
        return ESP_FAIL;
    }
    if (bt->probe_size < 7 + minimum || bt->probe_event[2] < 4 + minimum)
    {
        ESP_LOGW(TAG, "HCI command %04x reply is short", opcode);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (opcode != 0x0c03 && bt->probe_count < BT_PROBES)
    {
        struct bt_probe *probe = &bt->probes[bt->probe_count++];

        probe->opcode = opcode;
        probe->page = page;
        probe->size = bt->probe_size;
        memcpy(probe->event, bt->probe_event, bt->probe_size);
    }
    return ESP_OK;
}

static esp_err_t bt_check_controller(struct bt_adapter *bt)
{
    esp_err_t result = bt_probe(bt, 0x0c03, -1, 0);

    /* A controller may refuse a reset just after a session ends; only a
       well-formed failure status is retried. */
    for (unsigned i = 1; result == ESP_FAIL && i < BT_RESET_TRIES; i++)
    {
        int64_t end = esp_timer_get_time() + BT_RESET_RETRY_US;

        while (esp_timer_get_time() < end && !atomic_load(&bt->closed) &&
                !atomic_load(&s_stop))
            usb_host_client_handle_events(bt->client, pdMS_TO_TICKS(5));
        if (atomic_load(&bt->closed) || atomic_load(&s_stop))
            return bt->error ? bt->error : ESP_ERR_INVALID_STATE;
        result = bt_probe(bt, 0x0c03, -1, 0);
    }

    if (result == ESP_OK)
        result = bt_probe(bt, 0x1001, -1, 8);
    if (result == ESP_OK)
        result = bt_probe(bt, 0x1002, -1, 64);
    if (result == ESP_OK)
        result = bt_probe(bt, 0x1004, 0, 10);
    if (result != ESP_OK)
        return result;
    /* Page 0 bit 37 marks a controller without BR/EDR. */
    if (bt->probe_event[13] & 0x20)
    {
        ESP_LOGW(TAG, "Adapter does not support Bluetooth Classic");
        return ESP_ERR_NOT_SUPPORTED;
    }
    unsigned pages = bt->probe_event[8];

    for (unsigned page = 1; result == ESP_OK && page <= pages && page < 3; page++)
        result = bt_probe(bt, 0x1004, page, 10);
    if (result == ESP_OK)
        result = bt_probe(bt, 0x1005, -1, 7);
    if (result == ESP_OK && (bt->probe_event[7] | (bt->probe_event[8] << 8)) >
            GPSP_HCI_ACL_MAX - 4)
    {
        ESP_LOGW(TAG, "Adapter ACL length is too large");
        result = ESP_ERR_NOT_SUPPORTED;
    }
    if (result == ESP_OK)
        result = bt_probe(bt, 0x1009, -1, 6);
    return result;
}

/* A closed adapter answers like failed hardware so Bluedroid can run its own
   disable path: links drop, commands fail, and start-up reads reuse the
   replies checked before the host started. */
static void bt_answer(struct bt_adapter *bt, uint16_t opcode, int page)
{
    static const uint16_t status_commands[] = {
        0x0401, 0x0405, 0x0406, 0x0409, 0x040a, 0x040f, 0x0411, 0x0413,
        0x0419, 0x041b, 0x041c, 0x041d, 0x041f, 0x0428, 0x0429, 0x0803,
        0x0804, 0x080b,
    };
    uint8_t event[7] = {4, 0x0e, 4, 1, opcode, opcode >> 8, BT_HW_FAILURE};

    /* Host Number Of Completed Packets has no reply. */
    if (opcode == 0x0c35)
        return;
    const struct bt_probe *probe = bt_find_probe(bt, opcode, page);

    if (probe)
    {
        uint8_t reply[BT_EVENT_SIZE];

        memcpy(reply, probe->event, probe->size);
        bt_deliver(bt, reply, probe->size);
        return;
    }
    for (unsigned i = 0; i < sizeof(status_commands) / sizeof(*status_commands); i++)
        if (status_commands[i] == opcode)
        {
            event[1] = 0x0f;
            event[3] = BT_HW_FAILURE;
            event[4] = 1;
            event[5] = opcode;
            event[6] = opcode >> 8;
        }
    bt_deliver(bt, event, sizeof(event));
}

static void bt_close_links(struct bt_adapter *bt)
{
    bt->links_closed = true;
    for (unsigned i = 0; i < bt->link_count; i++)
    {
        uint8_t event[] = {4, 0x05, 4, 0, bt->links[i], bt->links[i] >> 8, 0x08};

        bt_deliver(bt, event, sizeof(event));
    }
    bt->link_count = 0;
    for (unsigned i = 0; i < bt->pending_count; i++)
        bt_answer(bt, bt->pending[i].opcode, bt->pending[i].page);
    bt->pending_count = 0;
}

/* Runs on the USB task between event pumps. */
static void bt_service(struct bt_adapter *bt)
{
    unsigned index;

    /* Bluedroid frees no command left waiting at deinit; finish them. */
    if (atomic_load(&bt->disabled) && !atomic_load(&bt->drained))
    {
        bt_close(bt, ESP_OK);
        if (!bt->links_closed)
            bt_close_links(bt);
        atomic_store(&bt->drained, true);
    }
    if (atomic_load(&bt->closed) && !bt->links_closed)
        bt_close_links(bt);
    if (!atomic_load(&bt->closed) && bt->pending_count &&
            esp_timer_get_time() - bt->pending[0].sent_us >= BT_COMMAND_TIMEOUT_US)
    {
        ESP_LOGW(TAG, "HCI command %04x timed out", bt->pending[0].opcode);
        bt_close(bt, ESP_ERR_TIMEOUT);
        bt_close_links(bt);
    }
    while ((atomic_load(&bt->closed) || !bt->tx_pending) &&
            xQueueReceive(bt->ready_slots, &index, 0) == pdTRUE)
    {
        struct bt_slot *slot = &bt->slots[index];
        uint16_t opcode = slot->data[1] | (slot->data[2] << 8);
        int page = slot->size > 4 ? slot->data[4] : -1;

#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
        if (!atomic_load(&bt->closed))
            bt_log_signal("out", slot->data, slot->size);
#endif
        /* Barrot adapters accept controller-to-host flow control but then
           refuse commands with status 0x07. Keep it off there; the host's
           completed-packet reports then have no use and are not sent. */
        bool barrot_flow = bt->barrot && slot->data[0] == 1 && slot->size >= 5 &&
                (opcode == 0x0c31 || opcode == 0x0c35);

        if (barrot_flow && opcode == 0x0c31)
            slot->data[4] = 0;
        if (atomic_load(&bt->closed))
        {
            if (slot->data[0] == 1)
                bt_answer(bt, opcode, page);
        }
        else if (barrot_flow && opcode == 0x0c35)
            ;
        else if (bt_submit(bt, slot->data, slot->size) == ESP_OK &&
                slot->data[0] == 1)
        {
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
            if (opcode == 0x0c35 && slot->size >= 9)
            {
                if (bt->acl_done < 4)
                    ESP_LOGI(TAG, "HCI command 0c35 handles=%u handle=%03x count=%u",
                            slot->data[4], (slot->data[5] | (slot->data[6] << 8)) & 0x0fff,
                            slot->data[7] | (slot->data[8] << 8));
                bt->acl_done += slot->data[7] | (slot->data[8] << 8);
            }
            else if (opcode != 0x0c35)
                ESP_LOGI(TAG, "HCI command %04x", opcode);
#endif
            if (opcode != 0x0c35 && bt->pending_count < BT_PENDING)
                bt->pending[bt->pending_count++] = (struct bt_pending){
                    opcode, page, esp_timer_get_time(),
                };
        }
        else if (atomic_load(&bt->closed) && slot->data[0] == 1)
            bt_answer(bt, opcode, page);
        xQueueSend(bt->free_slots, &index, 0);
        bt_deliver(bt, NULL, 0);
    }
}

/* Bluedroid restores its H4 prefix as soon as send returns. */
static void bt_send(uint8_t *packet, uint16_t size)
{
    struct bt_adapter *bt = s_adapter;
    unsigned index;

    if (size < 4 || (packet[0] != 1 && packet[0] != 2) ||
            size > (packet[0] == 1 ? 259 : GPSP_HCI_ACL_MAX + 1) ||
            xQueueReceive(bt->free_slots, &index, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Dropped HCI packet type=%u size=%u", packet[0], size);
        atomic_store(&bt->stack_error, ESP_ERR_INVALID_SIZE);
        return;
    }
    bt->slots[index].size = size;
    memcpy(bt->slots[index].data, packet, size);
    xQueueSend(bt->ready_slots, &index, 0);
}

static bool bt_can_send(void)
{
    return uxQueueMessagesWaiting(s_adapter->free_slots) != 0;
}

static esp_err_t bt_register_host(const esp_bluedroid_hci_driver_callbacks_t *callbacks)
{
    atomic_store(&s_adapter->callbacks, callbacks);
    return ESP_OK;
}

static void bt_stack_task(void *arg)
{
    struct bt_adapter *bt = arg;
    const esp_bluedroid_hci_driver_operations_t operations = {
        .send = bt_send, .check_send_available = bt_can_send,
        .register_host_callback = bt_register_host,
    };
    esp_bluedroid_config_t config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bool initialized = false, enabled = false, profile = false;
    esp_err_t result = nvs_flash_init();

    if (result == ESP_OK)
        result = esp_bluedroid_attach_hci_driver(&operations);
    if (result == ESP_OK)
    {
        result = esp_bluedroid_init_with_cfg(&config);
        initialized = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        result = esp_bluedroid_enable();
        enabled = result == ESP_OK;
    }
    if (result == ESP_OK && s_profile && !atomic_load(&bt->stopping))
    {
        profile = true;
        result = s_profile->init();
    }
    if (result == ESP_OK)
        atomic_store(&bt->working, true);
    else
        atomic_store(&bt->stack_error, result);
    while (!atomic_load(&bt->stopping))
        vTaskDelay(pdMS_TO_TICKS(5));
    if (profile)
    {
        s_profile->stop(atomic_load(&bt->removed));
        s_profile->close();
    }
    if (enabled)
        result = esp_bluedroid_disable();
    else
        result = ESP_OK;
    atomic_store(&bt->disabled, true);
    while (!atomic_load(&bt->drained))
        vTaskDelay(1);
    xSemaphoreTake(bt->rx_lock, portMAX_DELAY);
    bt->rx_open = false;
    xSemaphoreGive(bt->rx_lock);
    if (initialized && result == ESP_OK)
        result = esp_bluedroid_deinit();
    /* A host left running may still use the driver and profile. */
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluetooth host release failed: %s", esp_err_to_name(result));
        atomic_store(&bt->stack_failed, true);
        vTaskDelete(NULL);
    }
    esp_bluedroid_detach_hci_driver();
    if (profile)
        s_profile->deinit();
    atomic_store(&bt->stack_done, true);
    vTaskDelete(NULL);
}

static void bt_client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    struct bt_adapter *bt = arg;

    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
            event->dev_gone.dev_hdl == bt->device)
    {
        atomic_store(&bt->removed, true);
        bt_close(bt, ESP_OK);
        ESP_LOGI(TAG, "USB Bluetooth adapter removed");
    }
}

static esp_err_t bt_transfer(struct bt_adapter *bt, usb_transfer_t **result,
        unsigned size, unsigned endpoint, usb_transfer_cb_t callback)
{
    esp_err_t error = usb_host_transfer_alloc(size, 0, result);

    if (error == ESP_OK)
    {
        (*result)->device_handle = bt->device;
        (*result)->bEndpointAddress = endpoint;
        (*result)->num_bytes = size;
        (*result)->context = bt;
        (*result)->callback = callback;
    }
    return error;
}

static void bt_cancel(struct bt_adapter *bt, usb_transfer_t *transfer, bool busy)
{
    if (busy && transfer)
    {
        usb_host_endpoint_halt(bt->device, transfer->bEndpointAddress);
        usb_host_endpoint_flush(bt->device, transfer->bEndpointAddress);
    }
}

static esp_err_t bt_alloc_slots(struct bt_adapter *bt)
{
    bt->slots = heap_caps_calloc(BT_SLOTS, sizeof(*bt->slots),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bt->free_slots = xQueueCreate(BT_SLOTS, sizeof(unsigned));
    bt->ready_slots = xQueueCreate(BT_SLOTS, sizeof(unsigned));
    bt->rx_lock = xSemaphoreCreateMutex();
    if (!bt->slots || !bt->free_slots || !bt->ready_slots || !bt->rx_lock)
        return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < BT_SLOTS; i++)
        xQueueSend(bt->free_slots, &i, 0);
    bt->rx_open = true;
    return ESP_OK;
}

static void bt_free_slots(struct bt_adapter *bt)
{
    if (bt->rx_lock)
        vSemaphoreDelete(bt->rx_lock);
    if (bt->ready_slots)
        vQueueDelete(bt->ready_slots);
    if (bt->free_slots)
        vQueueDelete(bt->free_slots);
    heap_caps_free(bt->slots);
}

#define BT_TRY(call) do { result = (call); if (result != ESP_OK) goto done; } while (0)

static void bt_task(void *arg)
{
    struct bt_adapter *bt = arg;
    const usb_device_desc_t *device;
    const usb_config_desc_t *config;
    const usb_host_client_config_t client = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = bt_client_event, .callback_arg = bt},
    };
    esp_err_t result;

    s_adapter = bt;
    BT_TRY(bt_alloc_slots(bt));
    BT_TRY(usb_host_client_register(&client, &bt->client));
    BT_TRY(usb_host_device_open(bt->client, bt->address, &bt->device));
    BT_TRY(usb_host_get_device_descriptor(bt->device, &device));
    BT_TRY(usb_host_get_active_config_descriptor(bt->device, &config));
    if (!bt_find_endpoints(config, &bt->endpoints))
    {
        result = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    bt->barrot = device->idVendor == 0x33fa;
    BT_TRY(usb_host_interface_claim(bt->client, bt->device,
            bt->endpoints.interface, 0));
    bt->claimed = true;
    /* One interrupt packet per URB avoids DWC short-packet list handling. */
    unsigned event_bytes = bt->endpoints.event_mps;
    unsigned acl_bytes = ((GPSP_HCI_ACL_MAX + bt->endpoints.acl_mps - 1) /
            bt->endpoints.acl_mps) * bt->endpoints.acl_mps;

    BT_TRY(bt_transfer(bt, &bt->event, event_bytes, bt->endpoints.event, bt_in_done));
    BT_TRY(bt_transfer(bt, &bt->acl_in, acl_bytes, bt->endpoints.acl_in, bt_in_done));
    BT_TRY(bt_transfer(bt, &bt->acl_out, GPSP_HCI_ACL_MAX,
            bt->endpoints.acl_out, bt_out_done));
    BT_TRY(bt_transfer(bt, &bt->command, 8 + 258, 0, bt_out_done));
    BT_TRY(usb_host_transfer_submit(bt->event));
    bt->event_busy = true;
    BT_TRY(usb_host_transfer_submit(bt->acl_in));
    bt->acl_busy = true;
    ESP_LOGI(TAG, "Loading USB Bluetooth %04x:%04x core=%d", device->idVendor,
            device->idProduct, xPortGetCoreID());
    ESP_LOGI(TAG, "HCI endpoints event=%02x/%u acl=%02x,%02x/%u",
            bt->endpoints.event, bt->endpoints.event_mps, bt->endpoints.acl_in,
            bt->endpoints.acl_out, bt->endpoints.acl_mps);
    bt->start_us = esp_timer_get_time();
    BT_TRY(bt_check_controller(bt));
    if (xTaskCreatePinnedToCore(bt_stack_task, "gpsp_bt_stack", 4096, bt,
            BT_PRIORITY, NULL, 1) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    bt->stack_started = true;

    bool ready = false;

    while (!atomic_load(&bt->closed) && !atomic_load(&s_stop))
    {
        usb_host_client_handle_events(bt->client, pdMS_TO_TICKS(5));
        bt_service(bt);
        if (atomic_load(&bt->closed))
            break;
        if (atomic_load(&bt->stack_error))
        {
            result = atomic_load(&bt->stack_error);
            break;
        }
        if (atomic_load(&bt->working))
        {
            if (!ready)
            {
                ready = true;
                bt_publish(GPSP_USB_BT_READY, ESP_OK);
                ESP_LOGI(TAG, "Bluetooth host ready core=%d", xPortGetCoreID());
            }
            esp_err_t polled = s_profile ? s_profile->poll() : ESP_OK;

            if (polled == ESP_ERR_INVALID_STATE)
            {
                bt->restart = true;
                break;
            }
            if (polled != ESP_OK)
            {
                result = polled;
                break;
            }
        }
        else if (esp_timer_get_time() - bt->start_us >= BT_START_TIMEOUT_US)
        {
            ESP_LOGW(TAG, "Bluetooth initialization timed out");
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }

done:
    if (!atomic_load(&bt->removed) && !atomic_load(&s_stop) && !bt->error)
        bt->error = result != ESP_OK ? result : bt->errors ? ESP_FAIL : ESP_OK;
    atomic_store(&bt->stopping, true);
    int64_t release = esp_timer_get_time() + BT_RELEASE_TIMEOUT_US;

    /* The host disables over a live adapter; a fault closes it. */
    while (bt->stack_started && !atomic_load(&bt->stack_done) &&
            !atomic_load(&bt->stack_failed) && esp_timer_get_time() < release)
    {
        usb_host_client_handle_events(bt->client, pdMS_TO_TICKS(5));
        bt_service(bt);
    }
    bool stuck = bt->stack_started && !atomic_load(&bt->stack_done);

    if (stuck)
    {
        ESP_LOGE(TAG, "Bluetooth host did not stop; restart to use the adapter");
        bt->error = ESP_ERR_TIMEOUT;
        atomic_store(&s_stuck, true);
    }
    bt_close(bt, ESP_OK);
    bt_cancel(bt, bt->event, bt->event_busy);
    bt_cancel(bt, bt->acl_in, bt->acl_busy);
    bt_cancel(bt, bt->acl_out, bt->tx_pending == bt->acl_out);
    /* USBH owns EP0; keep its buffer until completion or device removal. */
    if (bt->tx_pending == bt->command && bt->command)
        ESP_LOGW(TAG, "Waiting for USB control completion; removal also releases it");
    while (bt->event_busy || bt->acl_busy || bt->tx_pending)
        usb_host_client_handle_events(bt->client, pdMS_TO_TICKS(10));
    if (bt->event)
        usb_host_transfer_free(bt->event);
    if (bt->acl_in)
        usb_host_transfer_free(bt->acl_in);
    if (bt->acl_out)
        usb_host_transfer_free(bt->acl_out);
    if (bt->command)
        usb_host_transfer_free(bt->command);
    if (bt->claimed)
        usb_host_interface_release(bt->client, bt->device, bt->endpoints.interface);
    if (bt->device)
        usb_host_device_close(bt->client, bt->device);
    if (bt->client)
        usb_host_client_deregister(bt->client);
    ESP_LOGI(TAG, "USB Bluetooth unloaded: %s rx=%u errors=%u",
            esp_err_to_name(bt->error), atomic_load(&s_received), bt->errors);
    bool restart = bt->restart && !atomic_load(&s_stop) && !stuck;
    uint8_t address = bt->address;
    esp_err_t error = bt->error;

    /* A stuck host may still call the driver; keep its state. */
    if (!stuck)
    {
        s_adapter = NULL;
        bt_free_slots(bt);
        heap_caps_free(bt);
    }
    if (restart)
        atomic_store(&s_pending, address);
    bt_publish(restart ? GPSP_USB_BT_STARTING : error ? GPSP_USB_BT_ERROR :
            GPSP_USB_BT_ABSENT, error);
    atomic_store(&s_active, false);
    vTaskDelete(NULL);
}

void gpsp_usb_bt_attach(uint8_t address)
{
    if (s_profile)
        s_profile->attached();
    bt_publish(GPSP_USB_BT_STARTING, ESP_OK);
    atomic_store(&s_pending, address);
}

void gpsp_usb_bt_poll(void)
{
    if (atomic_load(&s_active))
        return;
    unsigned address = atomic_exchange(&s_pending, 0);

    if (!address)
        return;
    if (atomic_load(&s_stuck))
    {
        bt_publish(GPSP_USB_BT_ERROR, ESP_ERR_INVALID_STATE);
        return;
    }
    struct bt_adapter *bt = heap_caps_calloc(1, sizeof(*bt),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!bt)
    {
        bt_publish(GPSP_USB_BT_ERROR, ESP_ERR_NO_MEM);
        ESP_LOGW(TAG, "No memory for USB Bluetooth");
        return;
    }
    atomic_store(&s_received, 0);
    bt->address = address;
    atomic_store(&s_stop, false);
    atomic_store(&s_active, true);
    if (xTaskCreatePinnedToCore(bt_task, "gpsp_usb_bt", 6144, bt,
            BT_PRIORITY, NULL, 1) != pdPASS)
    {
        heap_caps_free(bt);
        atomic_store(&s_active, false);
        bt_publish(GPSP_USB_BT_ERROR, ESP_ERR_NO_MEM);
        ESP_LOGW(TAG, "No task memory for USB Bluetooth");
    }
}

void gpsp_usb_bt_shutdown(void)
{
    atomic_store(&s_pending, 0);
    atomic_store(&s_stop, true);
    while (atomic_load(&s_active))
        vTaskDelay(pdMS_TO_TICKS(5));
    struct gpsp_usb_bt_info info;

    gpsp_usb_bt_get_info(&info);
    if (info.state == GPSP_USB_BT_STARTING)
        bt_publish(GPSP_USB_BT_ABSENT, ESP_OK);
}

#endif

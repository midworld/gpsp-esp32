/* Classic HID gamepad profile for the USB Bluetooth host.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_bt.h"
#include "usb_bt.h"

#if CONFIG_GPSP_USB_BT

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_hidh_api.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hid_gamepad.h"
#include "nvs.h"
#include "playstation_pad.h"

#define BT_INQUIRY_UNITS 8
#define BT_EVENTS 32
#define BT_CONNECT_TIMEOUT_US 30000000
#define BT_CLOSE_TIMEOUT_MS 2000
#define BT_PEERS 4
#define BT_PEER_SIZE (7 + GPSP_BT_NAME_SIZE)
#define BT_DESCRIPTOR_MAX 1024

enum bt_event_type
{
    BT_FOUND, BT_SCAN_DONE, BT_PIN, BT_CONFIRM, BT_PASSKEY, BT_AUTH,
    BT_BOND_REMOVED, BT_HID_READY,
    BT_OPEN, BT_CLOSE, BT_REPORT, BT_DESCRIPTOR, BT_SDP_READY, BT_SDP_RECORD,
};

/* Callbacks run on Bluedroid tasks and copy what the USB task needs. */
struct bt_event
{
    enum bt_event_type type;
    int status;
    uint8_t address[6], handle, pad;
    bool candidate, incoming;
    uint16_t vendor, product;
    unsigned size;
    /* A copy of the HID descriptor, freed by whoever takes the event. */
    uint8_t *descriptor;
    /* Input report, its leading bytes when longer, or a found name. */
    uint8_t data[1 + GPSP_HID_GAMEPAD_REPORT_BYTES];
};

struct bt_pad
{
    bool working, discovering, connecting, target, connected, describing, sony;
    bool disconnecting, connect_after_search, restart, failed, auto_connect;
    bool command_pending, hid_ready, sdp_ready;
    atomic_bool stopping, overflow;
    atomic_int removing;
    enum gpsp_bt_action command_action;
    enum gpsp_bt_pad pad;
    uint8_t command_address[6], peer[6], handle;
    struct gpsp_bt_info view, published;
    QueueHandle_t events;
    SemaphoreHandle_t closed;
    nvs_handle_t nvs;
    int64_t connect_us;
    unsigned reports, decoded, report_id;
    struct gpsp_hid_gamepad hid;
};

static const char *TAG = "gpsp_bt_pad";
/* Public calls use atomics; profile state belongs to the USB task. */
static struct bt_pad *s_pad;
static atomic_uint s_buttons;
static atomic_bool s_manual_hold;
static atomic_int s_failure;
static portMUX_TYPE s_view_lock = portMUX_INITIALIZER_UNLOCKED;

static void bt_publish(struct bt_pad *bt)
{
    bt->view.state = !bt->working ? GPSP_BT_STARTING :
            bt->disconnecting ? GPSP_BT_DISCONNECTING :
            bt->connecting || bt->describing ? GPSP_BT_CONNECTING :
            bt->connected ? GPSP_BT_CONNECTED :
            bt->discovering ? GPSP_BT_SEARCHING : GPSP_BT_IDLE;
    bt->view.error = atomic_load(&s_failure);
    bt->view.auto_connect = bt->auto_connect;
    bt->view.persistent = bt->nvs != 0;
    memcpy(bt->view.active, bt->peer, 6);
    portENTER_CRITICAL(&s_view_lock);
    bt->published = bt->view;
    portEXIT_CRITICAL(&s_view_lock);
}

void gpsp_bt_get_info(struct gpsp_bt_info *info)
{
    struct gpsp_usb_bt_info transport;

    gpsp_usb_bt_get_info(&transport);
    portENTER_CRITICAL(&s_view_lock);
    if (s_pad)
        *info = s_pad->published;
    else
    {
        enum gpsp_bt_error error = atomic_load(&s_failure);

        if (!error && transport.error)
            error = transport.error == ESP_ERR_TIMEOUT ? GPSP_BT_INIT_TIMEOUT :
                    transport.error == ESP_ERR_NOT_SUPPORTED ? GPSP_BT_NO_CLASSIC :
                    GPSP_BT_INIT_FAILED;
        *info = (struct gpsp_bt_info){
            .state = error ? GPSP_BT_ERROR : transport.state == GPSP_USB_BT_STARTING ?
                    GPSP_BT_STARTING : GPSP_BT_ABSENT,
            .error = error,
        };
    }
    portEXIT_CRITICAL(&s_view_lock);
}

bool gpsp_bt_request(enum gpsp_bt_action action, const uint8_t address[6])
{
    bool accepted = false;

    if ((unsigned)action > GPSP_BT_AUTO_OFF ||
            ((action == GPSP_BT_CONNECT || action == GPSP_BT_FORGET) && !address))
        return false;
    portENTER_CRITICAL(&s_view_lock);
    if (s_pad && !s_pad->command_pending &&
            s_pad->published.state >= GPSP_BT_IDLE &&
            s_pad->published.state <= GPSP_BT_CONNECTED &&
            ((action != GPSP_BT_SEARCH && action != GPSP_BT_CONNECT) ||
            s_pad->published.state == GPSP_BT_IDLE ||
            s_pad->published.state == GPSP_BT_SEARCHING))
    {
        s_pad->command_action = action;
        if (address)
            memcpy(s_pad->command_address, address, 6);
        s_pad->command_pending = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_view_lock);
    return accepted;
}

static int bt_find_peer(struct bt_pad *bt, const uint8_t address[6])
{
    for (unsigned i = 0; i < bt->view.count; i++)
        if (!memcmp(bt->view.devices[i].address, address, 6))
            return i;
    return -1;
}

/* An unknown kind or an empty name keeps what is already known. */
static struct gpsp_bt_device *bt_add_peer(struct bt_pad *bt,
        const uint8_t address[6], enum gpsp_bt_pad pad, const char *name)
{
    int index = bt_find_peer(bt, address);

    if (index < 0)
    {
        if (bt->view.count == GPSP_BT_DEVICES)
            return NULL;
        index = bt->view.count++;
        bt->view.devices[index] = (struct gpsp_bt_device){0};
        memcpy(bt->view.devices[index].address, address, 6);
    }
    struct gpsp_bt_device *device = &bt->view.devices[index];

    if (pad != GPSP_BT_PAD_UNKNOWN)
        device->pad = pad;
    if (name && name[0])
        snprintf(device->name, sizeof(device->name), "%s", name);
    return device;
}

static bool bt_has_key(const uint8_t address[6])
{
    esp_bd_addr_t peers[CONFIG_BT_SMP_MAX_BONDS];
    int count = CONFIG_BT_SMP_MAX_BONDS;

    if (esp_bt_gap_get_bond_device_list(&count, peers) != ESP_OK)
        return false;
    for (int i = 0; i < count; i++)
        if (!memcmp(peers[i], address, 6))
            return true;
    return false;
}

/* Removal finishes on the Bluedroid task; close waits for it. */
static void bt_remove_bond(struct bt_pad *bt, const uint8_t address[6])
{
    uint8_t peer[6];

    memcpy(peer, address, 6);
    atomic_fetch_add(&bt->removing, 1);
    if (esp_bt_gap_remove_bond_device(peer) != ESP_OK)
    {
        atomic_fetch_sub(&bt->removing, 1);
        atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
    }
}

static void bt_forget_all(struct bt_pad *bt)
{
    esp_bd_addr_t peers[CONFIG_BT_SMP_MAX_BONDS];
    int count = CONFIG_BT_SMP_MAX_BONDS;

    if (esp_bt_gap_get_bond_device_list(&count, peers) == ESP_OK)
        for (int i = 0; i < count; i++)
            bt_remove_bond(bt, peers[i]);
}

static void bt_storage(esp_err_t result)
{
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
        atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
}

static unsigned bt_paired_count(const struct bt_pad *bt)
{
    unsigned count = 0;

    for (unsigned i = 0; i < bt->view.count; i++)
        count += bt->view.devices[i].paired;
    return count;
}

static void bt_save_peers(struct bt_pad *bt)
{
    uint8_t peers[BT_PEERS * BT_PEER_SIZE];
    unsigned size = 0;

    if (!bt->nvs)
        return;
    for (unsigned i = 0; i < bt->view.count && size < sizeof(peers); i++)
    {
        struct gpsp_bt_device *device = &bt->view.devices[i];

        if (!device->paired)
            continue;
        memcpy(peers + size, device->address, 6);
        peers[size + 6] = device->pad;
        memcpy(peers + size + 7, device->name, GPSP_BT_NAME_SIZE);
        size += BT_PEER_SIZE;
    }
    bt_storage(nvs_set_blob(bt->nvs, "peers", peers, size));
    bt_storage(nvs_commit(bt->nvs));
}

static void bt_save_peer(struct bt_pad *bt)
{
    uint8_t peer[7];

    memcpy(peer, bt->peer, 6);
    peer[6] = bt->pad;
    if (bt->nvs)
    {
        bt_storage(nvs_set_blob(bt->nvs, "peer", peer, sizeof(peer)));
        bt_storage(nvs_commit(bt->nvs));
    }
}

static void bt_scan_mode(struct bt_pad *bt)
{
    bool incoming = bt->target && bt->auto_connect && !atomic_load(&s_manual_hold);

    esp_bt_gap_set_scan_mode(incoming || bt->connecting ? ESP_BT_CONNECTABLE :
            ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

static void bt_discover(struct bt_pad *bt)
{
    if (bt->connected || bt->discovering || bt->connecting || bt->disconnecting)
        return;
    /* Keep registrations; drop stale search results before a new scan. */
    unsigned count = 0;
    for (unsigned i = 0; i < bt->view.count; i++)
        if (bt->view.devices[i].paired)
        {
            bt->view.devices[count] = bt->view.devices[i];
            bt->view.devices[count++].found = false;
        }
    bt->view.count = count;
    atomic_store(&s_failure, GPSP_BT_OK);
    esp_err_t status = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
            BT_INQUIRY_UNITS, 0);

    bt->discovering = status == ESP_OK;
    ESP_LOGI(TAG, "Inquiry start status=%s duration=%ums", esp_err_to_name(status),
            BT_INQUIRY_UNITS * 1280);
}

static void bt_connect(struct bt_pad *bt)
{
    if (!bt->target)
        return;
    esp_err_t result = esp_bt_hid_host_connect(bt->peer);

    bt->connecting = result == ESP_OK;
    bt->connect_us = esp_timer_get_time();
    if (!bt->connecting)
        atomic_store(&s_failure, GPSP_BT_CONNECT_FAILED);
    ESP_LOGI(TAG, "Connect " ESP_BD_ADDR_STR " %s", ESP_BD_ADDR_HEX(bt->peer),
            esp_err_to_name(result));
    bt_scan_mode(bt);
}

static void bt_disconnect(struct bt_pad *bt)
{
    atomic_store(&s_manual_hold, true);
    atomic_store(&s_buttons, 0);
    bt->connect_after_search = false;
    bt_scan_mode(bt);
    if (bt->discovering)
        esp_bt_gap_cancel_discovery();
    /* A pending connect finishes first; its open event is then closed. */
    if (bt->connecting)
        bt->disconnecting = true;
    else if (bt->connected)
    {
        bt->disconnecting = true;
        if (esp_bt_hid_host_disconnect(bt->peer) != ESP_OK)
            bt->restart = true;
    }
}

static void bt_command(struct bt_pad *bt)
{
    enum gpsp_bt_action action;
    uint8_t address[6];

    portENTER_CRITICAL(&s_view_lock);
    bool pending = bt->command_pending;
    action = bt->command_action;
    memcpy(address, bt->command_address, 6);
    bt->command_pending = false;
    portEXIT_CRITICAL(&s_view_lock);
    if (!pending || !bt->working)
        return;
    int index = bt_find_peer(bt, address);

    switch (action)
    {
        case GPSP_BT_SEARCH:
            bt_discover(bt);
            break;
        case GPSP_BT_STOP_SEARCH:
            bt->connect_after_search = false;
            if (bt->discovering)
                esp_bt_gap_cancel_discovery();
            break;
        case GPSP_BT_CONNECT:
            if (index < 0 || bt->connected || bt->connecting || bt->disconnecting)
            {
                atomic_store(&s_failure, GPSP_BT_CONNECT_FAILED);
                break;
            }
            /* NVS keeps BT_PEERS registrations; keep them rather than drop one. */
            if (!bt->view.devices[index].paired && bt_paired_count(bt) >= BT_PEERS)
            {
                atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
                break;
            }
            memcpy(bt->peer, address, 6);
            bt->pad = bt->view.devices[index].pad;
            bt->target = true;
            atomic_store(&s_manual_hold, false);
            atomic_store(&s_failure, GPSP_BT_OK);
            if (bt->discovering)
            {
                bt->connect_after_search = true;
                esp_bt_gap_cancel_discovery();
            }
            else
                bt_connect(bt);
            break;
        case GPSP_BT_DISCONNECT:
            bt_disconnect(bt);
            break;
        case GPSP_BT_FORGET:
        case GPSP_BT_FORGET_ALL:
            if (action == GPSP_BT_FORGET && index < 0)
                break;
            if (action == GPSP_BT_FORGET_ALL)
            {
                bt_forget_all(bt);
                for (unsigned i = 0; i < bt->view.count; i++)
                    bt->view.devices[i].paired = false;
            }
            else
            {
                bt_remove_bond(bt, address);
                bt->view.devices[index].paired = false;
            }
            if (action == GPSP_BT_FORGET_ALL || !memcmp(address, bt->peer, 6))
            {
                if (bt->connected || bt->connecting)
                    bt_disconnect(bt);
                bt->target = false;
                memset(bt->peer, 0, 6);
                if (bt->nvs)
                    bt_storage(nvs_erase_key(bt->nvs, "peer"));
                bt_scan_mode(bt);
            }
            bt_save_peers(bt);
            break;
        case GPSP_BT_AUTO_ON:
        case GPSP_BT_AUTO_OFF:
            bt->auto_connect = action == GPSP_BT_AUTO_ON;
            if (bt->nvs)
            {
                bt_storage(nvs_set_u8(bt->nvs, "auto", bt->auto_connect));
                bt_storage(nvs_commit(bt->nvs));
            }
            if (bt->auto_connect)
            {
                atomic_store(&s_manual_hold, false);
                atomic_store(&s_failure, GPSP_BT_OK);
            }
            bt_scan_mode(bt);
            break;
    }
    bt->view.completed++;
    bt_publish(bt);
}

static bool bt_post(const struct bt_event *event)
{
    struct bt_pad *bt = s_pad;

    if (!bt || atomic_load(&bt->stopping))
        return false;
    if (xQueueSend(bt->events, event, 0) == pdTRUE)
        return true;
    atomic_store(&s_buttons, 0);
    atomic_store(&bt->overflow, true);
    return false;
}

/* Candidates are peripherals by class, HID services, or Sony pads by
   Device ID. Names are shown only; the HID descriptor decides after
   connecting. */
static void bt_parse_found(struct bt_event *event, const esp_bt_gap_cb_param_t *param)
{
    char *name = (char *)event->data;
    unsigned cod = 0;

    for (int i = 0; i < param->disc_res.num_prop; i++)
    {
        const esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        const uint8_t *value = prop->val;
        unsigned length = prop->len;

        if (prop->type == ESP_BT_GAP_DEV_PROP_COD && length >= 4)
            memcpy(&cod, value, 4);
        else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR)
            for (unsigned n = 0; n < length && value[n];)
            {
                unsigned size = value[n++];

                if (size > length - n)
                    break;
                /* Device ID: source, vendor, product, version. */
                if (value[n] == 0x10 && size >= 9)
                {
                    event->vendor = value[n + 3] | (value[n + 4] << 8);
                    event->product = value[n + 5] | (value[n + 6] << 8);
                }
                /* 16-bit service UUID lists with Human Interface Device. */
                if (value[n] == ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID ||
                        value[n] == ESP_BT_EIR_TYPE_CMPL_16BITS_UUID)
                    for (unsigned k = 1; k + 1 < size; k += 2)
                        event->candidate |= (value[n + k] | (value[n + k + 1] << 8)) == 0x1124;
                if ((value[n] == ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME ||
                        value[n] == ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME) && size > 1)
                {
                    unsigned count = size - 1 < GPSP_BT_NAME_SIZE - 1 ?
                            size - 1 : GPSP_BT_NAME_SIZE - 1;

                    memcpy(name, value + n + 1, count);
                    name[count] = 0;
                }
                n += size;
            }
        else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME)
        {
            if (length > GPSP_BT_NAME_SIZE - 1)
                length = GPSP_BT_NAME_SIZE - 1;
            memcpy(name, value, length);
            name[length] = 0;
        }
    }
    if (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL)
        event->candidate = true;
    if (event->vendor == 0x054c)
        event->pad = event->product == 0x0ce6 || event->product == 0x0df2 ?
                GPSP_BT_PAD_DS5 : event->product == 0x05c4 || event->product == 0x09cc ?
                GPSP_BT_PAD_DS4 : GPSP_BT_PAD_UNKNOWN;
    event->candidate |= event->pad != GPSP_BT_PAD_UNKNOWN;
    ESP_LOGI(TAG, "Inquiry result " ESP_BD_ADDR_STR " name=%s cod=%06x id=%04x:%04x candidate=%d",
            ESP_BD_ADDR_HEX(param->disc_res.bda), name, cod, event->vendor,
            event->product, event->candidate);
}

static void bt_gap_event(esp_bt_gap_cb_event_t type, esp_bt_gap_cb_param_t *param)
{
    struct bt_event event = {0};

    switch (type)
    {
        case ESP_BT_GAP_DISC_RES_EVT:
            event.type = BT_FOUND;
            memcpy(event.address, param->disc_res.bda, 6);
            bt_parse_found(&event, param);
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state != ESP_BT_GAP_DISCOVERY_STOPPED)
                return;
            event.type = BT_SCAN_DONE;
            break;
        case ESP_BT_GAP_PIN_REQ_EVT:
            event.type = BT_PIN;
            event.status = param->pin_req.min_16_digit;
            memcpy(event.address, param->pin_req.bda, 6);
            break;
        /* Just Works never reaches here; these need a display or keys. */
        case ESP_BT_GAP_CFM_REQ_EVT:
            event.type = BT_CONFIRM;
            memcpy(event.address, param->cfm_req.bda, 6);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            event.type = BT_PASSKEY;
            event.status = type == ESP_BT_GAP_KEY_NOTIF_EVT;
            memcpy(event.address, type == ESP_BT_GAP_KEY_REQ_EVT ?
                    param->key_req.bda : param->key_notif.bda, 6);
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            event.type = BT_AUTH;
            event.status = param->auth_cmpl.stat;
            memcpy(event.address, param->auth_cmpl.bda, 6);
            break;
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            if (s_pad && atomic_load(&s_pad->removing) > 0)
                atomic_fetch_sub(&s_pad->removing, 1);
            event.type = BT_BOND_REMOVED;
            event.status = param->remove_bond_dev_cmpl.status;
            memcpy(event.address, param->remove_bond_dev_cmpl.bda, 6);
            break;
        default:
            return;
    }
    bt_post(&event);
}

static void bt_hid_event(esp_hidh_cb_event_t type, esp_hidh_cb_param_t *param)
{
    struct bt_event event = {0};

    switch (type)
    {
        case ESP_HIDH_INIT_EVT:
            event.type = BT_HID_READY;
            event.status = param->init.status;
            break;
        case ESP_HIDH_DEINIT_EVT:
            if (s_pad)
                xSemaphoreGive(s_pad->closed);
            return;
        case ESP_HIDH_OPEN_EVT:
            if (param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING)
                return;
            event.type = BT_OPEN;
            event.status = param->open.status;
            event.handle = param->open.handle;
            event.incoming = !param->open.is_orig;
            memcpy(event.address, param->open.bd_addr, 6);
            break;
        case ESP_HIDH_CLOSE_EVT:
            event.type = BT_CLOSE;
            event.handle = param->close.handle;
            break;
        case ESP_HIDH_GET_DSCP_EVT:
        {
            const struct hidh_get_dscp_evt_param *d = &param->dscp;

            event.type = BT_DESCRIPTOR;
            event.status = d->status;
            event.handle = d->handle;
            event.vendor = d->vendor_id;
            event.product = d->product_id;
            /* The host frees its descriptor after this callback. */
            if (d->status == ESP_HIDH_OK && d->dsc_list && d->dl_len &&
                    d->dl_len <= BT_DESCRIPTOR_MAX)
                event.descriptor = heap_caps_malloc(d->dl_len,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (event.descriptor)
            {
                memcpy(event.descriptor, d->dsc_list, d->dl_len);
                event.size = d->dl_len;
            }
            else if (event.status == ESP_HIDH_OK)
                event.status = ESP_HIDH_ERR;
            if (!bt_post(&event))
                heap_caps_free(event.descriptor);
            return;
        }
        case ESP_HIDH_DATA_IND_EVT:
            /* Interrupt reports start with the report ID. Mapped controls
               end within the kept bytes. */
            if (param->data_ind.status != ESP_HIDH_OK || !param->data_ind.data)
                return;
            event.type = BT_REPORT;
            event.handle = param->data_ind.handle;
            event.size = param->data_ind.len < sizeof(event.data) ?
                    param->data_ind.len : sizeof(event.data);
            memcpy(event.data, param->data_ind.data, event.size);
            break;
        default:
            return;
    }
    bt_post(&event);
}

static void bt_sdp_event(esp_sdp_cb_event_t type, esp_sdp_cb_param_t *param)
{
    struct bt_event event = {0};

    if (type == ESP_SDP_INIT_EVT)
    {
        event.type = BT_SDP_READY;
        event.status = param->init.status;
    }
    else if (type == ESP_SDP_CREATE_RECORD_COMP_EVT)
    {
        event.type = BT_SDP_RECORD;
        event.status = param->create_record.status;
    }
    else
    {
        if (type == ESP_SDP_DEINIT_EVT && s_pad)
            xSemaphoreGive(s_pad->closed);
        return;
    }
    bt_post(&event);
}

static void bt_found(struct bt_pad *bt, const struct bt_event *event)
{
    if (!bt->discovering || bt->connecting || bt->connected || !event->candidate)
        return;
    struct gpsp_bt_device *device = bt_add_peer(bt, event->address, event->pad,
            (const char *)event->data);

    if (device)
    {
        device->found = true;
        ESP_LOGI(TAG, "Pad candidate found " ESP_BD_ADDR_STR,
                ESP_BD_ADDR_HEX(event->address));
    }
}

/* A pad this menu has not registered keeps no key or selection. */
static void bt_reject(struct bt_pad *bt, enum gpsp_bt_error error)
{
    int index = bt_find_peer(bt, bt->peer);

    bt->describing = false;
    bt_disconnect(bt);
    atomic_store(&s_failure, error);
    if (index < 0 || !bt->view.devices[index].paired)
    {
        bt_remove_bond(bt, bt->peer);
        bt->target = false;
        if (bt->nvs)
            bt_storage(nvs_erase_key(bt->nvs, "peer"));
        bt_scan_mode(bt);
    }
}

static void bt_open(struct bt_pad *bt, const struct bt_event *event)
{
    bool ours = bt->target && !memcmp(event->address, bt->peer, 6);

    ESP_LOGI(TAG, "HID open " ESP_BD_ADDR_STR " status=%d incoming=%d",
            ESP_BD_ADDR_HEX(event->address), event->status, event->incoming);
    if (event->status != ESP_HIDH_OK)
    {
        if (ours && bt->connecting)
        {
            bt->connecting = bt->disconnecting = false;
            if (!atomic_load(&s_manual_hold))
                atomic_store(&s_failure, GPSP_BT_CONNECT_FAILED);
            bt_scan_mode(bt);
        }
        return;
    }
    if (ours && bt->connected)
        return;
    /* Only the selected pad may stay connected; incoming needs the setting. */
    if (!ours || bt->connected || atomic_load(&s_manual_hold) ||
            (event->incoming && !bt->connecting && !bt->auto_connect))
    {
        uint8_t address[6];

        memcpy(address, event->address, 6);
        esp_bt_hid_host_disconnect(address);
        if (ours)
        {
            /* The close event ends the disconnecting state. */
            bt->connecting = false;
            bt->disconnecting = bt->connected = true;
            bt->handle = event->handle;
        }
        return;
    }
    /* Input waits for the descriptor that the host reads after each open. */
    bt->connecting = false;
    bt->connected = bt->describing = true;
    bt->sony = false;
    bt->handle = event->handle;
    bt->connect_us = esp_timer_get_time();
    bt->reports = bt->report_id = 0;
    gpsp_hid_gamepad_reset(&bt->hid);
    atomic_store(&s_buttons, 0);
    bt_scan_mode(bt);
}

/* The host reads a descriptor after every open, including ones this
   profile closes; only the selected connection's descriptor applies. */
static void bt_descriptor(struct bt_pad *bt, const struct bt_event *event)
{
    if (!bt->connected || bt->disconnecting || !bt->describing ||
            event->handle != bt->handle)
    {
        ESP_LOGI(TAG, "HID descriptor for handle=%u ignored", event->handle);
        return;
    }
    enum gpsp_hid_gamepad_result result = GPSP_HID_GAMEPAD_MALFORMED;
    /* Sony pads keep a decoder for the full reports they may resume. */
    bool sony = event->vendor == 0x054c && (event->product == 0x05c4 ||
            event->product == 0x09cc || event->product == 0x0ce6 ||
            event->product == 0x0df2);

    if (event->status == ESP_HIDH_OK && event->descriptor)
        result = gpsp_hid_gamepad_init(&bt->hid, event->descriptor, event->size,
                sony ? GPSP_HID_LAYOUT_SONY : GPSP_HID_LAYOUT_SEQUENTIAL);
    ESP_LOGI(TAG, "HID descriptor status=%d size=%u id=%04x:%04x result=%d fields=%u unmapped=%u",
            event->status, event->size, event->vendor, event->product, result,
            bt->hid.field_count, bt->hid.dropped);
    if (result != GPSP_HID_GAMEPAD_OK)
    {
        bt_reject(bt, result == GPSP_HID_GAMEPAD_NOT_GAMEPAD ? GPSP_BT_UNSUPPORTED :
                GPSP_BT_LAYOUT_FAILED);
        return;
    }
    bt->sony = sony;
    bt->pad = !bt->sony ? GPSP_BT_PAD_HID : event->product == 0x0ce6 ||
            event->product == 0x0df2 ? GPSP_BT_PAD_DS5 : GPSP_BT_PAD_DS4;
    bt->describing = false;
    struct gpsp_bt_device *device = bt_add_peer(bt, bt->peer, bt->pad, NULL);

    if (device)
        device->paired = true;
    bt_save_peer(bt);
    bt_save_peers(bt);
    atomic_store(&s_failure, GPSP_BT_OK);
    ESP_LOGI(TAG, "Pad connected handle=%u kind=%d reports=%u", bt->handle,
            bt->pad, bt->hid.report_count);
}

static void bt_report(struct bt_pad *bt, const struct bt_event *event)
{
    unsigned buttons;
    uint8_t id = event->size ? event->data[0] : 0;

    if (!bt->connected || bt->disconnecting || bt->describing ||
            event->handle != bt->handle)
        return;
    if (++bt->reports == 1)
        ESP_LOGI(TAG, "First Bluetooth input id=%02x size=%u", id, event->size);
    /* Full Sony reports carry a vendor page; replace other report state. */
    if (bt->sony && (id == 0x11 || id == 0x31))
    {
        if (!gpsp_playstation_decode(event->data, event->size, id == 0x31, true,
                &buttons))
            return;
        gpsp_hid_gamepad_reset(&bt->hid);
    }
    else if (!gpsp_hid_gamepad_decode(&bt->hid, event->data, event->size, &buttons))
        return;
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    unsigned previous = atomic_load(&s_buttons);

    if ((buttons ^ previous) & (1u << 16))
        ESP_LOGI(TAG, "HOME %s Bluetooth buttons=0x%05x",
                buttons & (1u << 16) ? "down" : "up", buttons);
#endif
    atomic_store(&s_buttons, buttons);
    bt->decoded++;
    if (bt->report_id != id)
    {
        bt->report_id = id;
        ESP_LOGI(TAG, "Bluetooth input format id=%02x size=%u", bt->report_id,
                event->size);
    }
}

static void bt_handle(struct bt_pad *bt, const struct bt_event *event)
{
    switch (event->type)
    {
        case BT_FOUND:
            bt_found(bt, event);
            break;
        case BT_SCAN_DONE:
            bt->discovering = false;
            if (bt->connect_after_search && bt->target)
            {
                bt->connect_after_search = false;
                bt_connect(bt);
            }
            break;
        case BT_PIN:
        case BT_CONFIRM:
        case BT_PASSKEY:
        {
            /* Only the selected pad pairs, and only with a legacy PIN of
               0000. Without a display or keys, numeric comparison and
               passkey entry are refused so pairing ends at once. */
            bool ours = bt->target && !memcmp(event->address, bt->peer, 6) &&
                    (bt->connecting || bt->connected) && !bt->disconnecting;
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            uint8_t address[6];

            memcpy(address, event->address, 6);
            ESP_LOGI(TAG, "Pairing request " ESP_BD_ADDR_STR " %s ours=%d",
                    ESP_BD_ADDR_HEX(address), event->type == BT_CONFIRM ?
                    "confirm" : event->type == BT_PASSKEY ?
                    (event->status ? "passkey shown" : "passkey") :
                    event->status ? "PIN 16 digits" : "PIN", ours);
            if (event->type == BT_PIN)
                esp_bt_gap_pin_reply(address, ours && !event->status, 4, pin);
            else if (event->type == BT_CONFIRM)
                esp_bt_gap_ssp_confirm_reply(address, false);
            else if (!event->status)
                esp_bt_gap_ssp_passkey_reply(address, false, 0);
            break;
        }
        case BT_AUTH:
        {
            int index = bt_find_peer(bt, event->address);
            bool ours = bt->target && !memcmp(event->address, bt->peer, 6);

            ESP_LOGI(TAG, "Authentication " ESP_BD_ADDR_STR " status=%d",
                    ESP_BD_ADDR_HEX(event->address), event->status);
            /* Just Works pairing is accepted by the host; keep only pads
               this menu registered or selected. */
            if (event->status == ESP_BT_STATUS_SUCCESS && !ours &&
                    (index < 0 || !bt->view.devices[index].paired))
                bt_remove_bond(bt, event->address);
            else if (event->status != ESP_BT_STATUS_SUCCESS && ours &&
                    bt->connecting)
                atomic_store(&s_failure, GPSP_BT_CONNECT_FAILED);
            break;
        }
        case BT_BOND_REMOVED:
            if (event->status != ESP_BT_STATUS_SUCCESS)
                atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
            break;
        case BT_HID_READY:
            bt->hid_ready = event->status == ESP_HIDH_OK;
            bt->failed |= !bt->hid_ready;
            break;
        case BT_SDP_READY:
        {
            esp_bluetooth_sdp_record_t record = {.dip = {
                .hdr = {.type = ESP_SDP_TYPE_DIP_SERVER},
                .vendor = 0xffff, .vendor_id_source = ESP_SDP_VENDOR_ID_SRC_BT,
                .product = 1, .version = 0x0100, .primary_record = true,
            }};

            /* Sony pads query the host's Device ID before reopening HID. */
            if (event->status != ESP_SDP_SUCCESS || esp_sdp_create_record(&record) != ESP_OK)
                bt->failed = true;
            break;
        }
        case BT_SDP_RECORD:
            bt->sdp_ready = event->status == ESP_SDP_SUCCESS;
            bt->failed |= !bt->sdp_ready;
            break;
        case BT_OPEN:
            bt_open(bt, event);
            break;
        case BT_CLOSE:
            if (bt->connected && event->handle == bt->handle)
            {
                bt->connected = bt->disconnecting = bt->describing = false;
                gpsp_hid_gamepad_reset(&bt->hid);
                atomic_store(&s_buttons, 0);
                bt_scan_mode(bt);
                ESP_LOGI(TAG, "Bluetooth pad disconnected");
            }
            break;
        case BT_DESCRIPTOR:
            bt_descriptor(bt, event);
            heap_caps_free(event->descriptor);
            break;
        case BT_REPORT:
            bt_report(bt, event);
            return;
    }
    if (!bt->working && bt->hid_ready && bt->sdp_ready)
    {
        bt->working = true;
        bt_scan_mode(bt);
        ESP_LOGI(TAG, "Classic HID ready address=" ESP_BD_ADDR_STR " core=%d",
                ESP_BD_ADDR_HEX(bt->view.adapter), xPortGetCoreID());
        if (bt->target)
            ESP_LOGI(TAG, "Saved pad " ESP_BD_ADDR_STR " ready for incoming reconnect",
                    ESP_BD_ADDR_HEX(bt->peer));
    }
    bt_publish(bt);
}

/* Keys belong to one adapter address; another adapter starts empty. */
static void bt_load(struct bt_pad *bt)
{
    uint8_t owner[6], auto_connect = 1, peer[7], peers[BT_PEERS * BT_PEER_SIZE];
    size_t size = sizeof(owner);

    if (nvs_open("gpsp_bt", NVS_READWRITE, &bt->nvs) != ESP_OK)
    {
        bt->nvs = 0;
        atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
        return;
    }
    if (nvs_get_blob(bt->nvs, "owner", owner, &size) != ESP_OK ||
            size != 6 || memcmp(owner, bt->view.adapter, 6))
    {
        ESP_LOGI(TAG, "New adapter " ESP_BD_ADDR_STR "; clearing saved pads",
                ESP_BD_ADDR_HEX(bt->view.adapter));
        bt_forget_all(bt);
        bt_storage(nvs_erase_key(bt->nvs, "peers"));
        bt_storage(nvs_erase_key(bt->nvs, "peer"));
        bt_storage(nvs_set_blob(bt->nvs, "owner", bt->view.adapter, 6));
        bt_storage(nvs_commit(bt->nvs));
    }
    nvs_get_u8(bt->nvs, "auto", &auto_connect);
    bt->auto_connect = auto_connect != 0;
    size = sizeof(peers);
    if (nvs_get_blob(bt->nvs, "peers", peers, &size) == ESP_OK && size % BT_PEER_SIZE == 0)
        for (unsigned i = 0; i < size; i += BT_PEER_SIZE)
        {
            char name[GPSP_BT_NAME_SIZE];

            memcpy(name, peers + i + 7, sizeof(name));
            name[sizeof(name) - 1] = 0;
            struct gpsp_bt_device *device = bt_add_peer(bt, peers + i,
                    peers[i + 6] <= GPSP_BT_PAD_HID ? peers[i + 6] : GPSP_BT_PAD_UNKNOWN,
                    name);

            if (device)
                device->paired = bt_has_key(device->address);
        }
    size = sizeof(peer);
    if (nvs_get_blob(bt->nvs, "peer", peer, &size) == ESP_OK && size == sizeof(peer))
    {
        memcpy(bt->peer, peer, 6);
        bt->pad = peer[6] <= GPSP_BT_PAD_HID ? peer[6] : GPSP_BT_PAD_UNKNOWN;
        bt->target = true;
        struct gpsp_bt_device *device = bt_add_peer(bt, bt->peer, bt->pad, NULL);

        if (device)
            device->paired = bt_has_key(device->address);
    }
}

static esp_err_t bt_profile_init(void)
{
    struct bt_pad *bt = heap_caps_calloc(1, sizeof(*bt),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint8_t *local = esp_bt_dev_get_address();

    if (!bt || !local)
    {
        heap_caps_free(bt);
        return ESP_ERR_NO_MEM;
    }
    bt->events = xQueueCreateWithCaps(BT_EVENTS, sizeof(struct bt_event),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bt->closed = xSemaphoreCreateCounting(2, 0);
    if (!bt->events || !bt->closed)
    {
        if (bt->events)
            vQueueDeleteWithCaps(bt->events);
        if (bt->closed)
            vSemaphoreDelete(bt->closed);
        heap_caps_free(bt);
        return ESP_ERR_NO_MEM;
    }
    memcpy(bt->view.adapter, local, 6);
    bt->auto_connect = true;
    bt_load(bt);
    bt_publish(bt);
    portENTER_CRITICAL(&s_view_lock);
    s_pad = bt;
    portEXIT_CRITICAL(&s_view_lock);
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    esp_err_t result = esp_bt_gap_register_callback(bt_gap_event);

    if (result == ESP_OK)
        result = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                &capability, sizeof(capability));
    if (result == ESP_OK)
        result = esp_bt_gap_set_device_name("gpSP");
    if (result == ESP_OK)
        result = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                ESP_BT_NON_DISCOVERABLE);
    if (result == ESP_OK)
        result = esp_sdp_register_callback(bt_sdp_event);
    if (result == ESP_OK)
        result = esp_sdp_init();
    if (result == ESP_OK)
        result = esp_bt_hid_host_register_callback(bt_hid_event);
    if (result == ESP_OK)
        result = esp_bt_hid_host_init();
    return result;
}

static esp_err_t bt_profile_poll(void)
{
    struct bt_pad *bt = s_pad;
    struct bt_event event;

    while (xQueueReceive(bt->events, &event, 0) == pdTRUE)
        bt_handle(bt, &event);
    bt_command(bt);
    if (atomic_load(&bt->overflow))
    {
        ESP_LOGW(TAG, "Bluetooth event queue overflow");
        bt->restart = true;
    }
    /* Bluedroid has no connect cancel; restart the host if it stalls. */
    if (bt->connecting && esp_timer_get_time() - bt->connect_us >= BT_CONNECT_TIMEOUT_US)
    {
        if (!atomic_load(&s_manual_hold))
            atomic_store(&s_failure, GPSP_BT_CONNECT_TIMEOUT);
        bt->restart = true;
    }
    if (bt->describing && esp_timer_get_time() - bt->connect_us >= BT_CONNECT_TIMEOUT_US)
    {
        ESP_LOGW(TAG, "HID descriptor did not arrive");
        bt_reject(bt, GPSP_BT_LAYOUT_FAILED);
        bt_publish(bt);
    }
    /* Profile setup failures stop the host until the adapter is attached
       again; a restart would fail the same way. */
    if (bt->failed)
    {
        ESP_LOGW(TAG, "Bluetooth profile setup failed");
        return ESP_FAIL;
    }
    return bt->restart ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void bt_profile_stop(bool removed)
{
    atomic_store(&s_buttons, 0);
    if (removed)
        atomic_store(&s_failure, GPSP_BT_OK);
    portENTER_CRITICAL(&s_view_lock);
    if (s_pad)
    {
        atomic_store(&s_pad->stopping, true);
        s_pad->published.state = GPSP_BT_STARTING;
    }
    portEXIT_CRITICAL(&s_view_lock);
}

/* Profile state outlives Bluedroid deinit, so close it before disable. */
static void bt_profile_close(void)
{
    struct bt_pad *bt = s_pad;
    unsigned waits = 0;

    if (!bt)
        return;
    for (unsigned i = 0; atomic_load(&bt->removing) > 0; i++)
    {
        if (i == BT_CLOSE_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "Bluetooth key removal timed out");
            atomic_store(&s_failure, GPSP_BT_STORAGE_FAILED);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    waits += esp_bt_hid_host_deinit() == ESP_OK;
    waits += esp_sdp_deinit() == ESP_OK;
    while (waits--)
        if (xSemaphoreTake(bt->closed, pdMS_TO_TICKS(BT_CLOSE_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Bluetooth profile close timed out");
            break;
        }
}

static void bt_profile_deinit(void)
{
    struct bt_pad *bt = s_pad;

    if (!bt)
        return;
    /* Bluedroid has stopped its tasks before the profile is released. */
    if (bt->nvs)
        nvs_close(bt->nvs);
    portENTER_CRITICAL(&s_view_lock);
    s_pad = NULL;
    portEXIT_CRITICAL(&s_view_lock);
    struct bt_event event;

    while (xQueueReceive(bt->events, &event, 0) == pdTRUE)
        heap_caps_free(event.descriptor);
    vSemaphoreDelete(bt->closed);
    vQueueDeleteWithCaps(bt->events);
    heap_caps_free(bt);
}

static void bt_profile_attached(void)
{
    atomic_store(&s_manual_hold, false);
    atomic_store(&s_failure, GPSP_BT_OK);
}

static const struct gpsp_usb_bt_profile s_profile = {
    .attached = bt_profile_attached,
    .init = bt_profile_init,
    .poll = bt_profile_poll,
    .stop = bt_profile_stop,
    .close = bt_profile_close,
    .deinit = bt_profile_deinit,
};

void gpsp_bt_init(void)
{
    gpsp_usb_bt_set_profile(&s_profile);
}

uint32_t gpsp_bt_read(void)
{
    return atomic_load(&s_buttons);
}

#endif

/* Test-only USB HCI controller model.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb_model.h"

#define MODEL_EVENTS 4

static const usb_device_desc_t device = {
    .bLength = 18, .bDescriptorType = 1, .bcdUSB = 0x0200,
    .bDeviceClass = 0xe0, .bDeviceSubClass = 1, .bDeviceProtocol = 1,
    .bMaxPacketSize0 = 64, .idVendor = 0x33fa, .idProduct = 0x0012,
    .bNumConfigurations = 1,
};
static const uint8_t configuration[] = {
    9, 2, 39, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
    7, 5, 0x81, 3, 16, 0, 1,
    7, 5, 0x02, 2, 64, 0, 0,
    7, 5, 0x82, 2, 64, 0, 0,
};

struct model_event
{
    uint8_t data[270];
    unsigned size;
};

struct model
{
    usb_host_client_config_t config;
    usb_transfer_t *pending[4];
    bool gone_sent, claimed;
    /* Each event ends an interrupt transfer, as Barrot adapters send them. */
    struct model_event events[MODEL_EVENTS];
    unsigned event_count, event_sent;
};
static struct model *active;
static atomic_bool present, stalled, control_stalled, no_classic;
static atomic_uint allocations, commands, connection_attempts, stall_after,
        remove_after, fail_after, status_after, acl_size = 1021, last_opcode, address = 1,
        refused_resets;
static atomic_bool connect_request, linked;
static atomic_int packet_type_credits = -1;
/* An event for the next idle slot, and recent commands. */
static uint8_t inject[64];
static atomic_uint inject_size;
static struct
{
    uint16_t opcode;
    uint8_t size, params[32];
} command_log[64];
static unsigned command_log_next;
static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;

void usb_model_event(unsigned code, const uint8_t *params, unsigned size)
{
    assert(size + 1 <= sizeof(inject) && !atomic_load(&inject_size));
    inject[0] = code;
    memcpy(inject + 1, params, size);
    atomic_store(&inject_size, size + 1);
}

void usb_model_clear_commands(void)
{
    portENTER_CRITICAL(&command_lock);
    memset(command_log, 0, sizeof(command_log));
    portEXIT_CRITICAL(&command_lock);
}

bool usb_model_command(unsigned opcode, uint8_t *params, unsigned size)
{
    bool found = false;

    portENTER_CRITICAL(&command_lock);
    for (unsigned i = 1; i <= 64 && !found; i++)
    {
        unsigned index = (command_log_next + 64 - i) % 64;

        if (command_log[index].size && command_log[index].opcode == opcode)
        {
            memcpy(params, command_log[index].params, size);
            found = true;
        }
    }
    portEXIT_CRITICAL(&command_lock);
    return found;
}

void usb_model_present(bool value) { atomic_store(&present, value); }
void usb_model_stall(bool value) { atomic_store(&stalled, value); }
void usb_model_stall_control(bool value) { atomic_store(&control_stalled, value); }
void usb_model_stall_after(unsigned count) { atomic_store(&stall_after, count); }
void usb_model_remove_after(unsigned count) { atomic_store(&remove_after, count); }
void usb_model_fail_after(unsigned count) { atomic_store(&fail_after, count); }
void usb_model_status_after(unsigned count) { atomic_store(&status_after, count); }
void usb_model_refuse_resets(unsigned count) { atomic_store(&refused_resets, count); }
void usb_model_refuse_packet_type(int credits) { atomic_store(&packet_type_credits, credits); }
void usb_model_connect(void) { atomic_store(&connect_request, true); }
void usb_model_no_classic(bool value) { atomic_store(&no_classic, value); }
void usb_model_acl_size(unsigned size) { atomic_store(&acl_size, size); }
void usb_model_address(unsigned first) { atomic_store(&address, first); }
unsigned usb_model_allocations(void) { return atomic_load(&allocations); }
unsigned usb_model_commands(void) { return atomic_load(&commands); }
unsigned usb_model_last_opcode(void) { return atomic_load(&last_opcode); }
unsigned usb_model_connection_attempts(void) { return atomic_load(&connection_attempts); }
bool usb_model_linked(void) { return atomic_load(&linked); }

esp_err_t usb_host_client_register(const usb_host_client_config_t *config,
        usb_host_client_handle_t *handle)
{
    assert(!active);
    active = calloc(1, sizeof(*active));
    assert(active);
    active->config = *config;
    *handle = (void *)active;
    return ESP_OK;
}

esp_err_t usb_host_client_deregister(usb_host_client_handle_t handle)
{
    assert((void *)handle == active && !active->claimed);
    for (unsigned i = 0; i < 4; i++)
        assert(!active->pending[i]);
    free(active);
    active = NULL;
    return ESP_OK;
}

esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t address,
        usb_device_handle_t *handle)
{
    assert((void *)client == active && address == 1);
    if (!atomic_load(&present))
        return ESP_ERR_NOT_FOUND;
    *handle = (void *)active;
    return ESP_OK;
}

esp_err_t usb_host_device_close(usb_host_client_handle_t client,
        usb_device_handle_t handle)
{
    assert((void *)client == active && (void *)handle == active);
    return ESP_OK;
}

esp_err_t usb_host_get_device_descriptor(usb_device_handle_t handle,
        const usb_device_desc_t **result)
{
    assert((void *)handle == active);
    *result = &device;
    return ESP_OK;
}

esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t handle,
        const usb_config_desc_t **result)
{
    assert((void *)handle == active);
    *result = (const void *)configuration;
    return ESP_OK;
}

esp_err_t usb_host_interface_claim(usb_host_client_handle_t client,
        usb_device_handle_t handle, uint8_t interface, uint8_t alternate)
{
    assert((void *)client == active && (void *)handle == active);
    assert(!interface && !alternate && !active->claimed);
    active->claimed = true;
    return ESP_OK;
}

esp_err_t usb_host_interface_release(usb_host_client_handle_t client,
        usb_device_handle_t handle, uint8_t interface)
{
    assert((void *)client == active && (void *)handle == active);
    assert(!interface && active->claimed);
    active->claimed = false;
    return ESP_OK;
}

esp_err_t usb_host_transfer_alloc(size_t size, int isoc, usb_transfer_t **result)
{
    assert(!isoc);
    usb_transfer_t transfer = {.data_buffer = calloc(1, size), .data_buffer_size = size};
    usb_transfer_t *storage = calloc(1, sizeof(*storage));
    assert(storage && transfer.data_buffer);
    memcpy(storage, &transfer, sizeof(transfer));
    *result = storage;
    atomic_fetch_add(&allocations, 1);
    return ESP_OK;
}

esp_err_t usb_host_transfer_free(usb_transfer_t *transfer)
{
    for (unsigned i = 0; i < 4; i++)
        assert(!active || transfer != active->pending[i]);
    free(transfer->data_buffer);
    free(transfer);
    atomic_fetch_sub(&allocations, 1);
    return ESP_OK;
}

static unsigned slot(unsigned endpoint)
{
    switch (endpoint)
    {
        case 0: return 0;
        case 0x81: return 1;
        case 0x82: return 2;
        case 0x02: return 3;
        default: assert(false); return 0;
    }
}

esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    unsigned index = slot(transfer->bEndpointAddress);
    assert(active->claimed && !active->pending[index]);
    assert((unsigned)transfer->num_bytes <= transfer->data_buffer_size);
    if (index == 1)
        assert(transfer->num_bytes == 16);
    active->pending[index] = transfer;
    transfer->status = USB_TRANSFER_STATUS_COMPLETED;
    return ESP_OK;
}

esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client,
        usb_transfer_t *transfer)
{
    assert((void *)client == active && transfer->bEndpointAddress == 0);
    usb_setup_packet_t *setup = (void *)transfer->data_buffer;
    assert(setup->bmRequestType == 0x20 && setup->wIndex == 0 &&
            setup->wLength == transfer->num_bytes - 8);
    if (atomic_load(&fail_after) && atomic_load(&commands) + 1 >= atomic_load(&fail_after))
        return ESP_FAIL;
    return usb_host_transfer_submit(transfer);
}

esp_err_t usb_host_endpoint_halt(usb_device_handle_t handle, uint8_t endpoint)
{
    assert((void *)handle == active);
    assert(endpoint != 0);
    unsigned index = slot(endpoint);
    if (active->pending[index])
        active->pending[index]->status = USB_TRANSFER_STATUS_CANCELED;
    return ESP_OK;
}

esp_err_t usb_host_endpoint_flush(usb_device_handle_t handle, uint8_t endpoint)
{
    assert((void *)handle == active);
    assert(endpoint != 0);
    return ESP_OK;
}

static uint8_t *event_add(struct model *model, unsigned code, unsigned payload)
{
    assert(model->event_count < MODEL_EVENTS);
    struct model_event *event = &model->events[model->event_count++];
    memset(event, 0, sizeof(*event));
    event->data[0] = code;
    event->data[1] = payload;
    event->size = payload + 2;
    /* Exercise the Barrot trailing-byte workaround on every event. */
    event->data[event->size++] = 0xff;
    return event->data;
}

static void response(struct model *model, const uint8_t *command)
{
    unsigned count = atomic_fetch_add(&commands, 1) + 1;
    unsigned opcode = command[0] | (command[1] << 8);

    atomic_store(&last_opcode, opcode);
    portENTER_CRITICAL(&command_lock);
    command_log[command_log_next].opcode = opcode;
    command_log[command_log_next].size = 1 + (command[2] < 31 ? command[2] : 31);
    memcpy(command_log[command_log_next].params, command + 3, command_log[command_log_next].size - 1);
    command_log_next = (command_log_next + 1) % 64;
    portEXIT_CRITICAL(&command_lock);
    if (atomic_load(&remove_after) && count >= atomic_load(&remove_after))
        atomic_store(&present, false);
    if (atomic_load(&stall_after) && count >= atomic_load(&stall_after))
        atomic_store(&stalled, true);
    if (atomic_load(&stalled) || !atomic_load(&present) || opcode == 0x0c35)
        return;
    unsigned refused = atomic_load(&refused_resets);
    bool refuse = opcode == 0x0c03 && refused &&
            atomic_compare_exchange_strong(&refused_resets, &refused, refused - 1);

    if (count == atomic_load(&status_after) || refuse)
    {
        /* A failed command: Command Complete with only the status. */
        uint8_t *event = event_add(model, 0x0e, 4);
        event[2] = 1;
        event[3] = command[0];
        event[4] = command[1];
        event[5] = refuse ? 0x07 : 0x1f;
        return;
    }
    /* Commands answered by Command Status, as in usb_bt.c. */
    static const uint16_t status_commands[] = {
        0x0401, 0x0405, 0x0406, 0x0409, 0x040a, 0x040f, 0x0411, 0x0413,
        0x0419, 0x041b, 0x041c, 0x041d, 0x041f, 0x0428, 0x0429, 0x0803,
        0x0804, 0x080b,
    };
    bool status_command = false;
    for (unsigned i = 0; i < sizeof(status_commands) / sizeof(*status_commands); i++)
        status_command |= status_commands[i] == opcode;
    if (status_command)
    {
        uint8_t *status = event_add(model, 0x0f, 4);
        int credits = atomic_load(&packet_type_credits);
        status[3] = 1;
        status[4] = command[0];
        status[5] = command[1];
        if (opcode == 0x040f && credits >= 0)
        {
            status[2] = 0x07;
            status[3] = credits;
        }
        /* Remote features, with extended features, then page 1. */
        if (opcode == 0x041b)
        {
            uint8_t *features = event_add(model, 0x0b, 11);
            features[3] = command[3];
            features[4] = command[4];
            memset(features + 5, 0xff, 8);
        }
        if (opcode == 0x041c)
        {
            uint8_t *features = event_add(model, 0x23, 13);
            features[3] = command[3];
            features[4] = command[4];
            features[5] = command[5];
            features[6] = 1;
            features[7] = 0x01;
        }
        if (opcode == 0x0405)
            atomic_fetch_add(&connection_attempts, 1);
        if (opcode == 0x0409 || opcode == 0x040a)
        {
            uint8_t *complete = event_add(model, 0x03, 11);
            complete[2] = opcode == 0x040a ? command[9] : 0;
            complete[3] = 0x80;
            memcpy(complete + 5, command + 3, 6);
            complete[11] = 1;
            atomic_store(&linked, opcode == 0x0409);
        }
        if (opcode == 0x0419)
        {
            /* Remote Name Request Complete: status, address, 248-byte name. */
            uint8_t *name = event_add(model, 0x07, 255);
            memcpy(name + 3, command + 3, 6);
            memcpy(name + 9, "Gamepad", 7);
        }
        if (opcode == 0x0406 && atomic_exchange(&linked, false))
        {
            uint8_t *down = event_add(model, 0x05, 4);
            down[3] = command[3];
            down[4] = command[4];
            down[5] = 0x16;
        }
        return;
    }
    uint8_t *event = event_add(model, 0x0e, 0);
    unsigned payload = 0;
    event[2] = 1;
    event[3] = command[0];
    event[4] = command[1];
    switch (opcode)
    {
        case 0x1001:
            payload = 8;
            event[6] = event[9] = 14;
            event[10] = 0xff;
            break;
        case 0x1002:
            payload = 64;
            break;
        case 0x1003:
            payload = 8;
            event[6] = 0xff;
            event[12] = 8;
            event[13] = 0x80;
            break;
        case 0x1004:
            payload = 10;
            event[6] = command[3];
            event[7] = 1;
            if (!command[3] && atomic_load(&no_classic))
                event[12] = 0x20;
            break;
        case 0x1005:
            payload = 7;
            event[6] = atomic_load(&acl_size);
            event[7] = atomic_load(&acl_size) >> 8;
            event[9] = 4;
            break;
        case 0x1009:
            payload = 6;
            for (unsigned i = 0; i < 6; i++)
                event[6 + i] = atomic_load(&address) + i;
            break;
        case 0x0c14:
            payload = 248;
            memcpy(event + 6, "BT test controller", 18);
            break;
    }
    struct model_event *last = &model->events[model->event_count - 1];
    event[1] = payload + 4;
    last->size = payload + 6;
    event[last->size++] = 0xff;
}

esp_err_t usb_host_client_handle_events(usb_host_client_handle_t handle, TickType_t ticks)
{
    struct model *model = (void *)handle;
    assert(model == active);
    if (!atomic_load(&present) && !model->gone_sent)
    {
        usb_host_client_event_msg_t event = {
            .event = USB_HOST_CLIENT_EVENT_DEV_GONE,
            .dev_gone = {.dev_hdl = (void *)model},
        };
        model->gone_sent = true;
        atomic_store(&linked, false);
        model->config.async.client_event_callback(&event,
                model->config.async.callback_arg);
        for (unsigned i = 0; i < 4; i++)
            if (model->pending[i])
                model->pending[i]->status = USB_TRANSFER_STATUS_NO_DEVICE;
    }
    if (atomic_load(&present) && !atomic_load(&stalled) && !model->event_count &&
            atomic_exchange(&connect_request, false))
    {
        /* A remote pad pages the adapter. */
        uint8_t *event = event_add(model, 0x04, 10);
        for (unsigned i = 0; i < 6; i++)
            event[2 + i] = 0x10 + i;
        event[9] = 0x25;
        event[11] = 1;
    }
    unsigned injected = atomic_load(&inject_size);
    if (atomic_load(&present) && !atomic_load(&stalled) && !model->event_count &&
            injected)
    {
        uint8_t *event = event_add(model, inject[0], injected - 1);
        memcpy(event + 2, inject + 1, injected - 1);
        atomic_store(&inject_size, 0);
    }
    for (unsigned i = 0; i < 4; i++)
    {
        usb_transfer_t *transfer = model->pending[i];
        if (!transfer)
            continue;
        bool cancelled = transfer->status != USB_TRANSFER_STATUS_COMPLETED;
        if (!cancelled && (i == 2 ||
                /* Commands wait for queued events, unless events are stalled. */
                (i == 0 && (atomic_load(&control_stalled) ||
                        (model->event_count && !atomic_load(&stalled)))) ||
                (i == 1 && (atomic_load(&stalled) || !model->event_count))))
            continue;
        model->pending[i] = NULL;
        transfer->actual_num_bytes = 0;
        if (!cancelled && i == 0)
        {
            response(model, transfer->data_buffer + 8);
            transfer->actual_num_bytes = transfer->num_bytes;
        }
        else if (!cancelled && i == 1)
        {
            struct model_event *event = &model->events[0];
            unsigned count = event->size - model->event_sent;
            if (count > transfer->num_bytes)
                count = transfer->num_bytes;
            memcpy(transfer->data_buffer, event->data + model->event_sent, count);
            transfer->actual_num_bytes = count;
            model->event_sent += count;
            if (model->event_sent == event->size)
            {
                model->event_sent = 0;
                memmove(model->events, model->events + 1,
                        --model->event_count * sizeof(*model->events));
            }
        }
        else if (!cancelled && i == 3)
            transfer->actual_num_bytes = transfer->num_bytes;
        transfer->callback(transfer);
    }
    vTaskDelay(ticks ? ticks : 1);
    return ESP_OK;
}

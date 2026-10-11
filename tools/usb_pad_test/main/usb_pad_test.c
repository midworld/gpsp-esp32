/* USB gamepad test firmware for ESP32-P4.
 *
 * Prints descriptors of each attached device, then reads the first
 * interrupt IN endpoint and decodes known gamepad reports.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

/* libretro joypad IDs, so the decoded mask matches gpsp_board_input_read(). */
enum pad_button
{
    PAD_B = 0,
    PAD_Y,
    PAD_SELECT,
    PAD_START,
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_A,
    PAD_X,
    PAD_L,
    PAD_R,
    PAD_L2,
    PAD_R2,
    PAD_L3,
    PAD_R3,
    PAD_BUTTON_COUNT
};

enum pad_proto
{
    PAD_PROTO_UNKNOWN,
    PAD_PROTO_HID,
    PAD_PROTO_SWITCH_PRO,
    PAD_PROTO_XINPUT,
    PAD_PROTO_GIP
};

struct pad_init_packet
{
    uint8_t len;
    uint8_t data[16];
};

struct pad
{
    uint8_t addr;
    usb_device_handle_t dev;
    uint16_t vid;
    uint16_t pid;
    enum pad_proto proto;
    int intf;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t mps_in;
    usb_transfer_t *in_xfer;
    usb_transfer_t *out_xfer;
    bool in_busy;
    bool out_busy;
    /* A transfer error halts the host pipe until it is cleared. */
    bool in_halted;
    bool out_halted;
    bool gone;
    int64_t in_retry_us;
    const struct pad_init_packet *init;
    unsigned init_count;
    unsigned init_step;
    unsigned init_tries;
    int64_t init_next_us;
    uint16_t buttons;
    uint8_t last[64];
    int last_len;
    uint32_t reports;
};

static const char *TAG = "usb_pad";

static const char *const s_button_names[PAD_BUTTON_COUNT] = {
    "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
    "A", "X", "L", "R", "L2", "R2", "L3", "R3"
};

static const char *const s_proto_names[] = {
    "unknown", "HID", "Switch Pro", "XInput (360)", "Xbox One (GIP)"
};

/* USB handshake, no-timeout mode, then full 0x30 input reports. */
static const struct pad_init_packet s_switch_init[] = {
    { 2, { 0x80, 0x02 } },
    { 2, { 0x80, 0x04 } },
    { 12, { 0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
            0x03, 0x30 } },
};

static const struct pad_init_packet s_gip_init[] = {
    { 5, { 0x05, 0x20, 0x00, 0x01, 0x00 } },
};

/* Player 1 LED; some XInput devices wait for this before reporting. */
static const struct pad_init_packet s_xinput_init[] = {
    { 3, { 0x01, 0x03, 0x06 } },
};

static usb_host_client_handle_t s_client;
static struct pad s_pad;
static uint8_t s_new_addr;

static void pad_hexdump(const char *label, const uint8_t *data, int len)
{
    char line[3 * 64 + 1];
    int count = len > 64 ? 64 : len;
    int i;

    for (i = 0; i < count; i++)
        snprintf(line + i * 3, 4, "%02x ", data[i]);
    line[count * 3] = '\0';
    ESP_LOGI(TAG, "%s (%d): %s", label, len, line);
}

static void pad_print_buttons(uint16_t buttons)
{
    char line[128];
    size_t pos = 0;
    int i;

    line[0] = '\0';
    for (i = 0; i < PAD_BUTTON_COUNT; i++)
    {
        if (buttons & (1u << i))
            pos += snprintf(line + pos, sizeof(line) - pos, "%s ",
                    s_button_names[i]);
    }
    ESP_LOGI(TAG, "buttons 0x%04x: %s", buttons, pos ? line : "-");
}

static void pad_set(uint16_t *buttons, bool pressed, enum pad_button id)
{
    if (pressed)
        *buttons |= 1u << id;
}

/* Button names follow position: B is bottom, A is right. */
static bool pad_decode_xinput(const uint8_t *r, int len, uint16_t *out)
{
    uint16_t b;

    if (len < 6 || r[0] != 0x00 || r[1] < 0x0e)
        return false;

    b = r[2] | (r[3] << 8);
    pad_set(out, b & 0x0001, PAD_UP);
    pad_set(out, b & 0x0002, PAD_DOWN);
    pad_set(out, b & 0x0004, PAD_LEFT);
    pad_set(out, b & 0x0008, PAD_RIGHT);
    pad_set(out, b & 0x0010, PAD_START);
    pad_set(out, b & 0x0020, PAD_SELECT);
    pad_set(out, b & 0x0040, PAD_L3);
    pad_set(out, b & 0x0080, PAD_R3);
    pad_set(out, b & 0x0100, PAD_L);
    pad_set(out, b & 0x0200, PAD_R);
    pad_set(out, b & 0x1000, PAD_B);
    pad_set(out, b & 0x2000, PAD_A);
    pad_set(out, b & 0x4000, PAD_Y);
    pad_set(out, b & 0x8000, PAD_X);
    pad_set(out, r[4] > 0x40, PAD_L2);
    pad_set(out, r[5] > 0x40, PAD_R2);
    return true;
}

static bool pad_decode_gip(const uint8_t *r, int len, uint16_t *out)
{
    if (len < 10 || r[0] != 0x20)
        return false;

    pad_set(out, r[4] & 0x04, PAD_START);
    pad_set(out, r[4] & 0x08, PAD_SELECT);
    pad_set(out, r[4] & 0x10, PAD_B);
    pad_set(out, r[4] & 0x20, PAD_A);
    pad_set(out, r[4] & 0x40, PAD_Y);
    pad_set(out, r[4] & 0x80, PAD_X);
    pad_set(out, r[5] & 0x01, PAD_UP);
    pad_set(out, r[5] & 0x02, PAD_DOWN);
    pad_set(out, r[5] & 0x04, PAD_LEFT);
    pad_set(out, r[5] & 0x08, PAD_RIGHT);
    pad_set(out, r[5] & 0x10, PAD_L);
    pad_set(out, r[5] & 0x20, PAD_R);
    pad_set(out, r[5] & 0x40, PAD_L3);
    pad_set(out, r[5] & 0x80, PAD_R3);
    pad_set(out, (r[6] | (r[7] << 8)) > 0x100, PAD_L2);
    pad_set(out, (r[8] | (r[9] << 8)) > 0x100, PAD_R2);
    return true;
}

static bool pad_decode_switch(const uint8_t *r, int len, uint16_t *out)
{
    if (len < 6 || r[0] != 0x30)
        return false;

    pad_set(out, r[3] & 0x01, PAD_Y);
    pad_set(out, r[3] & 0x02, PAD_X);
    pad_set(out, r[3] & 0x04, PAD_B);
    pad_set(out, r[3] & 0x08, PAD_A);
    pad_set(out, r[3] & 0x40, PAD_R);
    pad_set(out, r[3] & 0x80, PAD_R2);
    pad_set(out, r[4] & 0x01, PAD_SELECT);
    pad_set(out, r[4] & 0x02, PAD_START);
    pad_set(out, r[4] & 0x04, PAD_R3);
    pad_set(out, r[4] & 0x08, PAD_L3);
    pad_set(out, r[5] & 0x01, PAD_DOWN);
    pad_set(out, r[5] & 0x02, PAD_UP);
    pad_set(out, r[5] & 0x04, PAD_RIGHT);
    pad_set(out, r[5] & 0x08, PAD_LEFT);
    pad_set(out, r[5] & 0x40, PAD_L);
    pad_set(out, r[5] & 0x80, PAD_L2);
    return true;
}

static void pad_handle_report(const uint8_t *r, int len)
{
    uint16_t buttons = 0;
    bool decoded = false;

    s_pad.reports++;
    if (s_pad.reports <= 5)
        pad_hexdump("first", r, len);
    if (len <= 0)
        return;

    switch (s_pad.proto)
    {
        case PAD_PROTO_XINPUT:
            decoded = pad_decode_xinput(r, len, &buttons);
            break;
        case PAD_PROTO_GIP:
            decoded = pad_decode_gip(r, len, &buttons);
            break;
        case PAD_PROTO_SWITCH_PRO:
            decoded = pad_decode_switch(r, len, &buttons);
            break;
        default:
            break;
    }

    if (decoded)
    {
        if (buttons != s_pad.buttons)
        {
            s_pad.buttons = buttons;
            pad_print_buttons(buttons);
            pad_hexdump("report", r, len > 16 ? 16 : len);
        }
        return;
    }

    /* Unknown reports: print raw data only when it changes. */
    if (len != s_pad.last_len || memcmp(r, s_pad.last, len) != 0)
    {
        s_pad.last_len = len > (int)sizeof(s_pad.last)
                ? (int)sizeof(s_pad.last) : len;
        memcpy(s_pad.last, r, s_pad.last_len);
        pad_hexdump("raw", r, len);
    }
}

static void pad_in_cb(usb_transfer_t *xfer)
{
    s_pad.in_busy = false;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED)
        pad_handle_report(xfer->data_buffer, xfer->actual_num_bytes);
    else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE)
        ESP_LOGW(TAG, "IN transfer status %d", xfer->status);

    if (s_pad.gone || xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
            xfer->status == USB_TRANSFER_STATUS_CANCELED)
        return;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            usb_host_transfer_submit(xfer) == ESP_OK)
    {
        s_pad.in_busy = true;
        return;
    }

    /* The client task resets the endpoint and reads again. */
    s_pad.in_halted = true;
    s_pad.in_retry_us = esp_timer_get_time() + 100000;
}

static void pad_out_cb(usb_transfer_t *xfer)
{
    s_pad.out_busy = false;

    /* The next init packet goes out only after this one arrived. */
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED)
    {
        s_pad.init_step++;
        s_pad.init_tries = 0;
    }
    else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
            xfer->status != USB_TRANSFER_STATUS_CANCELED)
    {
        ESP_LOGW(TAG, "OUT transfer status %d", xfer->status);
        s_pad.out_halted = true;
    }
}

/* Clears a halted host pipe so it takes transfers again. */
static void pad_endpoint_reset(uint8_t address)
{
    usb_host_endpoint_halt(s_pad.dev, address);
    usb_host_endpoint_flush(s_pad.dev, address);
    usb_host_endpoint_clear(s_pad.dev, address);
}

static void pad_recover_in(void)
{
    if (!s_pad.dev || !s_pad.in_halted || s_pad.gone || s_pad.in_busy ||
            esp_timer_get_time() < s_pad.in_retry_us)
        return;

    pad_endpoint_reset(s_pad.ep_in);
    if (usb_host_transfer_submit(s_pad.in_xfer) == ESP_OK)
    {
        ESP_LOGI(TAG, "IN endpoint recovered");
        s_pad.in_halted = false;
        s_pad.in_busy = true;
    }
    else
        s_pad.in_retry_us = esp_timer_get_time() + 100000;
}

static void pad_send(const uint8_t *data, int len)
{
    usb_transfer_t *xfer = s_pad.out_xfer;

    if (!xfer || s_pad.out_busy || len > (int)xfer->data_buffer_size)
        return;

    memcpy(xfer->data_buffer, data, len);
    xfer->num_bytes = len;
    if (usb_host_transfer_submit(xfer) == ESP_OK)
    {
        s_pad.out_busy = true;
        pad_hexdump("sent", data, len);
    }
}

/* Picks the first alt 0 interface with an interrupt IN endpoint. */
static bool pad_find_interface(const usb_config_desc_t *config,
        const usb_intf_desc_t **intf_ret, const usb_ep_desc_t **in_ret,
        const usb_ep_desc_t **out_ret)
{
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)config;
    const usb_intf_desc_t *intf = NULL;
    const usb_ep_desc_t *in = NULL;
    const usb_ep_desc_t *out = NULL;
    int offset = 0;

    while ((desc = usb_parse_next_descriptor(desc, config->wTotalLength,
            &offset)) != NULL)
    {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
        {
            if (in)
                break;
            intf = (const usb_intf_desc_t *)desc;
            out = NULL;
            if (intf->bAlternateSetting != 0)
                intf = NULL;
        }
        else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                intf)
        {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;

            if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_INTR)
                continue;
            if (USB_EP_DESC_GET_EP_DIR(ep))
            {
                if (!in)
                    in = ep;
            }
            else if (!out)
                out = ep;
        }
    }

    if (!in)
        return false;

    *intf_ret = intf;
    *in_ret = in;
    *out_ret = out;
    return true;
}

static enum pad_proto pad_detect_proto(const usb_intf_desc_t *intf)
{
    if (intf->bInterfaceClass == USB_CLASS_HID)
    {
        if (s_pad.vid == 0x057e && s_pad.pid == 0x2009)
            return PAD_PROTO_SWITCH_PRO;
        return PAD_PROTO_HID;
    }

    if (intf->bInterfaceClass == USB_CLASS_VENDOR_SPEC)
    {
        if (intf->bInterfaceSubClass == 0x5d &&
                intf->bInterfaceProtocol == 0x01)
            return PAD_PROTO_XINPUT;
        if (intf->bInterfaceSubClass == 0x47 &&
                intf->bInterfaceProtocol == 0xd0)
            return PAD_PROTO_GIP;
    }

    return PAD_PROTO_UNKNOWN;
}

static void pad_print_strings(void)
{
    usb_device_info_t info;

    if (usb_host_device_info(s_pad.dev, &info) != ESP_OK)
        return;

    ESP_LOGI(TAG, "speed %s, config %d",
            info.speed == USB_SPEED_HIGH ? "high" :
            info.speed == USB_SPEED_FULL ? "full" : "low",
            info.bConfigurationValue);
    if (info.str_desc_manufacturer)
        usb_print_string_descriptor(info.str_desc_manufacturer);
    if (info.str_desc_product)
        usb_print_string_descriptor(info.str_desc_product);
    if (info.str_desc_serial_num)
        usb_print_string_descriptor(info.str_desc_serial_num);
}

/* Returns false while transfers are pending; retried after DEV_GONE. */
static bool pad_close(void)
{
    if (s_pad.in_busy || s_pad.out_busy)
        return false;

    if (s_pad.in_xfer)
        usb_host_transfer_free(s_pad.in_xfer);
    if (s_pad.out_xfer)
        usb_host_transfer_free(s_pad.out_xfer);
    if (s_pad.intf >= 0)
        usb_host_interface_release(s_client, s_pad.dev, s_pad.intf);
    usb_host_device_close(s_client, s_pad.dev);
    ESP_LOGI(TAG, "device closed after %lu reports",
            (unsigned long)s_pad.reports);
    memset(&s_pad, 0, sizeof(s_pad));
    return true;
}

static void pad_open(uint8_t addr)
{
    const usb_device_desc_t *dev_desc;
    const usb_config_desc_t *config;
    const usb_intf_desc_t *intf;
    const usb_ep_desc_t *in;
    const usb_ep_desc_t *out;
    esp_err_t result;

    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.intf = -1;

    result = usb_host_device_open(s_client, addr, &s_pad.dev);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "open addr %u failed: %s", addr,
                esp_err_to_name(result));
        s_pad.dev = NULL;
        return;
    }
    s_pad.addr = addr;

    ESP_ERROR_CHECK(usb_host_get_device_descriptor(s_pad.dev, &dev_desc));
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(s_pad.dev,
            &config));
    s_pad.vid = dev_desc->idVendor;
    s_pad.pid = dev_desc->idProduct;

    ESP_LOGI(TAG, "device %04x:%04x at addr %u", s_pad.vid, s_pad.pid,
            addr);
    pad_print_strings();
    usb_print_device_descriptor(dev_desc);
    usb_print_config_descriptor(config, NULL);

    if (!pad_find_interface(config, &intf, &in, &out))
    {
        ESP_LOGW(TAG, "no interrupt IN endpoint; not a gamepad");
        pad_close();
        return;
    }

    s_pad.proto = pad_detect_proto(intf);
    ESP_LOGI(TAG, "interface %u class %02x/%02x/%02x -> %s",
            intf->bInterfaceNumber, intf->bInterfaceClass,
            intf->bInterfaceSubClass, intf->bInterfaceProtocol,
            s_proto_names[s_pad.proto]);

    result = usb_host_interface_claim(s_client, s_pad.dev,
            intf->bInterfaceNumber, 0);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "claim failed: %s", esp_err_to_name(result));
        pad_close();
        return;
    }
    s_pad.intf = intf->bInterfaceNumber;

    s_pad.ep_in = in->bEndpointAddress;
    s_pad.mps_in = USB_EP_DESC_GET_MPS(in);
    ESP_ERROR_CHECK(usb_host_transfer_alloc(s_pad.mps_in, 0,
            &s_pad.in_xfer));
    s_pad.in_xfer->device_handle = s_pad.dev;
    s_pad.in_xfer->bEndpointAddress = s_pad.ep_in;
    s_pad.in_xfer->callback = pad_in_cb;
    s_pad.in_xfer->num_bytes = s_pad.mps_in;
    ESP_LOGI(TAG, "IN ep 0x%02x mps %u interval %u", s_pad.ep_in,
            s_pad.mps_in, in->bInterval);

    if (out)
    {
        s_pad.ep_out = out->bEndpointAddress;
        ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &s_pad.out_xfer));
        s_pad.out_xfer->device_handle = s_pad.dev;
        s_pad.out_xfer->bEndpointAddress = s_pad.ep_out;
        s_pad.out_xfer->callback = pad_out_cb;
        ESP_LOGI(TAG, "OUT ep 0x%02x", s_pad.ep_out);
    }

    if (s_pad.proto == PAD_PROTO_SWITCH_PRO)
    {
        s_pad.init = s_switch_init;
        s_pad.init_count = sizeof(s_switch_init) / sizeof(s_switch_init[0]);
    }
    else if (s_pad.proto == PAD_PROTO_XINPUT)
    {
        s_pad.init = s_xinput_init;
        s_pad.init_count = sizeof(s_xinput_init) / sizeof(s_xinput_init[0]);
    }
    else if (s_pad.proto == PAD_PROTO_GIP)
    {
        s_pad.init = s_gip_init;
        s_pad.init_count = sizeof(s_gip_init) / sizeof(s_gip_init[0]);
    }
    s_pad.init_next_us = esp_timer_get_time() + 100000;

    if (usb_host_transfer_submit(s_pad.in_xfer) == ESP_OK)
        s_pad.in_busy = true;
    ESP_LOGI(TAG, "reading reports; press buttons");
}

static void pad_poll_init(void)
{
    const struct pad_init_packet *packet;
    int64_t now;

    if (!s_pad.dev || s_pad.gone || s_pad.init_step >= s_pad.init_count ||
            s_pad.out_busy)
        return;

    now = esp_timer_get_time();
    if (now < s_pad.init_next_us)
        return;
    s_pad.init_next_us = now + 50000;

    if (s_pad.init_tries++ >= 5)
    {
        ESP_LOGW(TAG, "init packet %u failed; giving up", s_pad.init_step);
        s_pad.init_step = s_pad.init_count;
        return;
    }

    if (s_pad.out_halted)
    {
        pad_endpoint_reset(s_pad.ep_out);
        s_pad.out_halted = false;
    }

    packet = &s_pad.init[s_pad.init_step];
    pad_send(packet->data, packet->len);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg,
        void *arg)
{
    switch (msg->event)
    {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            s_new_addr = msg->new_dev.address;
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            if (s_pad.dev == msg->dev_gone.dev_hdl)
            {
                ESP_LOGI(TAG, "device gone");
                s_pad.gone = true;
            }
            break;
        default:
            break;
    }
}

static void usb_lib_task(void *arg)
{
    for (;;)
    {
        uint32_t flags;

        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void usb_client_task(void *arg)
{
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    int64_t stats_us = 0;
    uint32_t stats_reports = 0;

    ESP_ERROR_CHECK(usb_host_client_register(&config, &s_client));
    ESP_LOGI(TAG, "waiting for a USB device");

    for (;;)
    {
        /* Transfer callbacks also run inside this call. */
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));

        if (s_pad.dev && s_pad.gone)
            pad_close();

        if (s_new_addr)
        {
            if (s_pad.dev)
                ESP_LOGW(TAG, "ignoring addr %u; a device is open", s_new_addr);
            else
                pad_open(s_new_addr);
            s_new_addr = 0;
        }

        pad_recover_in();
        pad_poll_init();

        if (s_pad.dev && esp_timer_get_time() - stats_us >= 5000000)
        {
            stats_us = esp_timer_get_time();
            if (s_pad.reports != stats_reports)
                ESP_LOGI(TAG, "%lu reports received",
                        (unsigned long)s_pad.reports);
            stats_reports = s_pad.reports;
        }
    }
}

void app_main(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        /* 0 selects the high-speed OTG port on ESP32-P4. */
        .peripheral_map = 0,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL,
            0) != pdPASS ||
            xTaskCreatePinnedToCore(usb_client_task, "usb_pad", 6144, NULL, 4,
            NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "task creation failed");
}

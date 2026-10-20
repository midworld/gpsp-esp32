/* ESP32-P4 USB gamepad input on the high-speed OTG port.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_usb_pad.h"
#include "gpsp_bt.h"
#include "playstation_pad.h"
#include "usb_bt.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_USB_PAD

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libretro.h"
#include "usb/usb_host.h"

#if CONFIG_FREERTOS_UNICORE
#define PAD_TASK_CORE tskNO_AFFINITY
#else
#define PAD_TASK_CORE 1
#endif

/* Init packets go out this far apart; the pad answers each one. */
#define PAD_INIT_GAP_US 50000
#define PAD_INIT_TRIES 5
/* A failed IN endpoint is reset and read again after this delay. */
#define PAD_IN_RETRY_US 100000
#define PAD_OUT_BYTES 64
/* Service USB before the CPU-side display expansion. */
#define PAD_TASK_PRIORITY (tskIDLE_PRIORITY + 7)
/* Home sits above the 16 RetroPad buttons. */
#define PAD_HOME_ID 16

enum pad_proto
{
    PAD_PROTO_NONE,
    PAD_PROTO_XINPUT,
    PAD_PROTO_GIP,
    PAD_PROTO_SWITCH_PRO,
    PAD_PROTO_DS4,
    PAD_PROTO_DUALSENSE,
    /* Other HID devices: no buttons, raw reports in the log only. */
    PAD_PROTO_HID
};

static const char *const s_proto_names[] = {
    "none", "XInput", "Xbox One", "Switch Pro", "DualShock 4", "DualSense",
    "HID"
};

struct pad_packet
{
    uint8_t len;
    uint8_t data[12];
};

struct pad
{
    uint8_t addr;
    uint16_t vid, pid;
    usb_device_handle_t dev;
    enum pad_proto proto;
    int intf;
    usb_transfer_t *in_xfer;
    usb_transfer_t *out_xfer;
    bool in_busy;
    bool out_busy;
    /* A transfer error halts the host pipe until it is cleared. */
    bool in_halted;
    bool out_halted;
    bool gone;
    int64_t in_retry_us;
    const struct pad_packet *init;
    unsigned init_count;
    unsigned init_step;
    unsigned init_tries;
    int64_t init_next_us;
    unsigned in_errors, out_errors, submit_errors, reset_errors, recoveries;
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    unsigned reports, decoded, changes;
    int64_t opened_us, last_report_us, last_decoded_us;
    int64_t report_gap_us, loop_max_us;
#endif
#if CONFIG_GPSP_USB_PAD_HID_DUMP
    uint8_t last[64];
    int last_len;
#endif
};

static const char *TAG = "gpsp_usb_pad";

/* Player 1 LED. XInput pads may send nothing until the host sets it. */
static const struct pad_packet s_xinput_init[] = {
    { 3, { 0x01, 0x03, 0x06 } },
};

/* GIP power on; Xbox One pads send no input before it. */
static const struct pad_packet s_gip_init[] = {
    { 5, { 0x05, 0x20, 0x00, 0x01, 0x00 } },
};

/* USB handshake, no timeout, then full 0x30 input reports. */
static const struct pad_packet s_switch_init[] = {
    { 2, { 0x80, 0x02 } },
    { 2, { 0x80, 0x04 } },
    { 12, { 0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
            0x03, 0x30 } },
};

static usb_host_client_handle_t s_client;
static TaskHandle_t s_lib_task;
static TaskHandle_t s_client_task;
static atomic_bool s_running;
/* True while s_client is registered; the library task waits for it. */
static atomic_bool s_client_active;
static atomic_uint s_buttons;
/* Written by the client event callback, read by the same task. */
static uint8_t s_new_addr;
static struct pad s_pad;

static const char *pad_transfer_status(usb_transfer_status_t status)
{
    switch (status)
    {
        case USB_TRANSFER_STATUS_COMPLETED: return "completed";
        case USB_TRANSFER_STATUS_ERROR: return "error";
        case USB_TRANSFER_STATUS_TIMED_OUT: return "timeout";
        case USB_TRANSFER_STATUS_CANCELED: return "canceled";
        case USB_TRANSFER_STATUS_STALL: return "stall";
        case USB_TRANSFER_STATUS_OVERFLOW: return "overflow";
        case USB_TRANSFER_STATUS_SKIPPED: return "skipped";
        case USB_TRANSFER_STATUS_NO_DEVICE: return "no-device";
        default: return "unknown";
    }
}

/* Keep a failing endpoint from flooding the console. */
static bool pad_log_error(unsigned count)
{
    return count <= 3 || count % 50 == 0;
}

static esp_err_t pad_submit(usb_transfer_t *xfer)
{
    esp_err_t result = usb_host_transfer_submit(xfer);

    if (result != ESP_OK && pad_log_error(++s_pad.submit_errors))
        ESP_LOGW(TAG, "Submit addr=%u ep=0x%02x: %s count=%u",
                s_pad.addr, xfer->bEndpointAddress, esp_err_to_name(result),
                s_pad.submit_errors);
    return result;
}

#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
static void pad_health(int64_t now)
{
    ESP_LOGI(TAG, "USB health addr=%u %04x:%04x rx=%u decoded=%u changes=%u "
            "rx_age_ms=%lld input_age_ms=%lld gap_max_ms=%lld loop_max_ms=%lld "
            "busy=%d/%d halt=%d/%d err=%u/%u submit_err=%u reset_err=%u recover=%u",
            s_pad.addr, s_pad.vid, s_pad.pid, s_pad.reports, s_pad.decoded,
            s_pad.changes,
            s_pad.last_report_us ? (now - s_pad.last_report_us) / 1000 : -1LL,
            s_pad.last_decoded_us ? (now - s_pad.last_decoded_us) / 1000 : -1LL,
            s_pad.report_gap_us / 1000, s_pad.loop_max_us / 1000,
            s_pad.in_busy, s_pad.out_busy, s_pad.in_halted, s_pad.out_halted,
            s_pad.in_errors, s_pad.out_errors, s_pad.submit_errors,
            s_pad.reset_errors, s_pad.recoveries);
    s_pad.report_gap_us = 0;
    s_pad.loop_max_us = 0;
}
#endif

static void pad_set(unsigned *buttons, bool pressed, unsigned id)
{
    if (pressed)
        *buttons |= 1u << id;
}

/* RetroPad names follow position: B is bottom, A is right. */
static bool pad_decode_xinput(const uint8_t *r, int len, unsigned *out)
{
    unsigned b;

    if (len < 6 || r[0] != 0x00 || r[1] < 0x0e)
        return false;

    b = r[2] | (r[3] << 8);
    pad_set(out, b & 0x0001, RETRO_DEVICE_ID_JOYPAD_UP);
    pad_set(out, b & 0x0002, RETRO_DEVICE_ID_JOYPAD_DOWN);
    pad_set(out, b & 0x0004, RETRO_DEVICE_ID_JOYPAD_LEFT);
    pad_set(out, b & 0x0008, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    pad_set(out, b & 0x0010, RETRO_DEVICE_ID_JOYPAD_START);
    pad_set(out, b & 0x0020, RETRO_DEVICE_ID_JOYPAD_SELECT);
    pad_set(out, b & 0x0040, RETRO_DEVICE_ID_JOYPAD_L3);
    pad_set(out, b & 0x0080, RETRO_DEVICE_ID_JOYPAD_R3);
    pad_set(out, b & 0x0100, RETRO_DEVICE_ID_JOYPAD_L);
    pad_set(out, b & 0x0200, RETRO_DEVICE_ID_JOYPAD_R);
    pad_set(out, b & 0x0400, PAD_HOME_ID);
    pad_set(out, b & 0x1000, RETRO_DEVICE_ID_JOYPAD_B);
    pad_set(out, b & 0x2000, RETRO_DEVICE_ID_JOYPAD_A);
    pad_set(out, b & 0x4000, RETRO_DEVICE_ID_JOYPAD_Y);
    pad_set(out, b & 0x8000, RETRO_DEVICE_ID_JOYPAD_X);
    pad_set(out, r[4] > 0x40, RETRO_DEVICE_ID_JOYPAD_L2);
    pad_set(out, r[5] > 0x40, RETRO_DEVICE_ID_JOYPAD_R2);
    return true;
}

static bool pad_decode_gip(const uint8_t *r, int len, unsigned *out)
{
    if (len < 10 || r[0] != 0x20)
        return false;

    pad_set(out, r[4] & 0x04, RETRO_DEVICE_ID_JOYPAD_START);
    pad_set(out, r[4] & 0x08, RETRO_DEVICE_ID_JOYPAD_SELECT);
    pad_set(out, r[4] & 0x10, RETRO_DEVICE_ID_JOYPAD_B);
    pad_set(out, r[4] & 0x20, RETRO_DEVICE_ID_JOYPAD_A);
    pad_set(out, r[4] & 0x40, RETRO_DEVICE_ID_JOYPAD_Y);
    pad_set(out, r[4] & 0x80, RETRO_DEVICE_ID_JOYPAD_X);
    pad_set(out, r[5] & 0x01, RETRO_DEVICE_ID_JOYPAD_UP);
    pad_set(out, r[5] & 0x02, RETRO_DEVICE_ID_JOYPAD_DOWN);
    pad_set(out, r[5] & 0x04, RETRO_DEVICE_ID_JOYPAD_LEFT);
    pad_set(out, r[5] & 0x08, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    pad_set(out, r[5] & 0x10, RETRO_DEVICE_ID_JOYPAD_L);
    pad_set(out, r[5] & 0x20, RETRO_DEVICE_ID_JOYPAD_R);
    pad_set(out, r[5] & 0x40, RETRO_DEVICE_ID_JOYPAD_L3);
    pad_set(out, r[5] & 0x80, RETRO_DEVICE_ID_JOYPAD_R3);
    /* Triggers are 10-bit. */
    pad_set(out, (r[6] | (r[7] << 8)) > 0x100, RETRO_DEVICE_ID_JOYPAD_L2);
    pad_set(out, (r[8] | (r[9] << 8)) > 0x100, RETRO_DEVICE_ID_JOYPAD_R2);
    return true;
}

static bool pad_decode_switch(const uint8_t *r, int len, unsigned *out)
{
    if (len < 6 || r[0] != 0x30)
        return false;

    pad_set(out, r[3] & 0x01, RETRO_DEVICE_ID_JOYPAD_Y);
    pad_set(out, r[3] & 0x02, RETRO_DEVICE_ID_JOYPAD_X);
    pad_set(out, r[3] & 0x04, RETRO_DEVICE_ID_JOYPAD_B);
    pad_set(out, r[3] & 0x08, RETRO_DEVICE_ID_JOYPAD_A);
    pad_set(out, r[3] & 0x40, RETRO_DEVICE_ID_JOYPAD_R);
    pad_set(out, r[3] & 0x80, RETRO_DEVICE_ID_JOYPAD_R2);
    pad_set(out, r[4] & 0x01, RETRO_DEVICE_ID_JOYPAD_SELECT);
    pad_set(out, r[4] & 0x02, RETRO_DEVICE_ID_JOYPAD_START);
    pad_set(out, r[4] & 0x04, RETRO_DEVICE_ID_JOYPAD_R3);
    pad_set(out, r[4] & 0x08, RETRO_DEVICE_ID_JOYPAD_L3);
    pad_set(out, r[4] & 0x10, PAD_HOME_ID);
    pad_set(out, r[5] & 0x01, RETRO_DEVICE_ID_JOYPAD_DOWN);
    pad_set(out, r[5] & 0x02, RETRO_DEVICE_ID_JOYPAD_UP);
    pad_set(out, r[5] & 0x04, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    pad_set(out, r[5] & 0x08, RETRO_DEVICE_ID_JOYPAD_LEFT);
    pad_set(out, r[5] & 0x40, RETRO_DEVICE_ID_JOYPAD_L);
    pad_set(out, r[5] & 0x80, RETRO_DEVICE_ID_JOYPAD_L2);
    return true;
}

static bool pad_decode_ds4(const uint8_t *r, int len, unsigned *out)
{
    return len >= 0 && gpsp_playstation_decode(r, len, false, false, out);
}

static bool pad_decode_dualsense(const uint8_t *r, int len, unsigned *out)
{
    return len >= 0 && gpsp_playstation_decode(r, len, true, false, out);
}

#if CONFIG_GPSP_USB_PAD_HID_DUMP
/* Logs a report only when it changes, to find a new pad's layout. */
static void pad_dump(const uint8_t *r, int len)
{
    char line[3 * sizeof(s_pad.last) + 1];
    int i;

    if (len > (int)sizeof(s_pad.last))
        len = sizeof(s_pad.last);
    if (len == s_pad.last_len && memcmp(r, s_pad.last, len) == 0)
        return;

    memcpy(s_pad.last, r, len);
    s_pad.last_len = len;
    for (i = 0; i < len; i++)
        snprintf(line + i * 3, 4, "%02x ", r[i]);
    line[len * 3] = '\0';
    ESP_LOGI(TAG, "report (%d): %s", len, line);
}
#endif

static void pad_in_done(usb_transfer_t *xfer)
{
    const uint8_t *r = xfer->data_buffer;
    int len = xfer->actual_num_bytes;
    unsigned buttons = 0;
    bool decoded = false;

    s_pad.in_busy = false;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED)
    {
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
        int64_t now = esp_timer_get_time();
        int64_t gap = now - (s_pad.last_report_us ? s_pad.last_report_us :
                s_pad.opened_us);
        if (gap > s_pad.report_gap_us)
            s_pad.report_gap_us = gap;
        s_pad.last_report_us = now;
        if (++s_pad.reports == 1)
            ESP_LOGI(TAG, "First report addr=%u len=%d head=%02x %02x",
                    s_pad.addr, len, len > 0 ? r[0] : 0, len > 1 ? r[1] : 0);
#endif
        switch (s_pad.proto)
        {
            case PAD_PROTO_XINPUT:
                decoded = pad_decode_xinput(r, len, &buttons);
                break;
            case PAD_PROTO_GIP:
                /* The guide button comes in its own message. */
                if (pad_decode_gip(r, len, &buttons))
                {
                    buttons |= atomic_load(&s_buttons) & (1u << PAD_HOME_ID);
                    decoded = true;
                }
                else if (len >= 5 && r[0] == 0x07)
                {
                    buttons = atomic_load(&s_buttons) & ~(1u << PAD_HOME_ID);
                    pad_set(&buttons, r[4] & 0x01, PAD_HOME_ID);
                    decoded = true;
                }
                break;
            case PAD_PROTO_SWITCH_PRO:
                decoded = pad_decode_switch(r, len, &buttons);
                break;
            case PAD_PROTO_DS4:
                decoded = pad_decode_ds4(r, len, &buttons);
                break;
            case PAD_PROTO_DUALSENSE:
                decoded = pad_decode_dualsense(r, len, &buttons);
                break;
            default:
#if CONFIG_GPSP_USB_PAD_HID_DUMP
                pad_dump(r, len);
#endif
                break;
        }

        /* Other reports (LED state, command replies) keep the last state. */
        if (decoded)
        {
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
            s_pad.decoded++;
            s_pad.last_decoded_us = now;
            if ((buttons ^ atomic_load(&s_buttons)) & (1u << PAD_HOME_ID))
                ESP_LOGI(TAG, "HOME %s addr=%u buttons=0x%05x",
                        buttons & (1u << PAD_HOME_ID) ? "down" : "up",
                        s_pad.addr, buttons);
            if (buttons != atomic_load(&s_buttons))
                s_pad.changes++;
#endif
            atomic_store(&s_buttons, buttons);
        }
    }
    else if (pad_log_error(++s_pad.in_errors))
        ESP_LOGW(TAG, "IN addr=%u ep=0x%02x status=%s bytes=%d/%d "
                "gone=%d count=%u", s_pad.addr, xfer->bEndpointAddress,
                pad_transfer_status(xfer->status), len, xfer->num_bytes,
                s_pad.gone, s_pad.in_errors);

    if (s_pad.gone || !atomic_load(&s_running) ||
            xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
            xfer->status == USB_TRANSFER_STATUS_CANCELED)
        return;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            pad_submit(xfer) == ESP_OK)
    {
        s_pad.in_busy = true;
        return;
    }

    /* Release held buttons while the client task recovers the endpoint. */
    atomic_store(&s_buttons, 0);
    s_pad.in_halted = true;
    s_pad.in_retry_us = esp_timer_get_time() + PAD_IN_RETRY_US;
}

static void pad_out_done(usb_transfer_t *xfer)
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
        s_pad.out_halted = true;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED &&
            pad_log_error(++s_pad.out_errors))
        ESP_LOGW(TAG, "OUT addr=%u ep=0x%02x status=%s init=%u/%u count=%u",
                s_pad.addr, xfer->bEndpointAddress,
                pad_transfer_status(xfer->status), s_pad.init_step,
                s_pad.init_count, s_pad.out_errors);
}

/* Clears a halted host pipe so it takes transfers again. */
static void pad_endpoint_reset(uint8_t address)
{
    esp_err_t halt = usb_host_endpoint_halt(s_pad.dev, address);
    esp_err_t flush = usb_host_endpoint_flush(s_pad.dev, address);
    esp_err_t clear = usb_host_endpoint_clear(s_pad.dev, address);

    if ((halt != ESP_OK || flush != ESP_OK || clear != ESP_OK) &&
            pad_log_error(++s_pad.reset_errors))
        ESP_LOGW(TAG, "Reset addr=%u ep=0x%02x halt=%s flush=%s clear=%s count=%u",
                s_pad.addr, address, esp_err_to_name(halt),
                esp_err_to_name(flush), esp_err_to_name(clear),
                s_pad.reset_errors);
}

static enum pad_proto pad_detect(const usb_device_desc_t *dev_desc,
        const usb_intf_desc_t *intf)
{
    if (intf->bInterfaceClass == USB_CLASS_VENDOR_SPEC)
    {
        if (intf->bInterfaceSubClass == 0x5d &&
                intf->bInterfaceProtocol == 0x01)
            return PAD_PROTO_XINPUT;
        if (intf->bInterfaceSubClass == 0x47 &&
                intf->bInterfaceProtocol == 0xd0)
            return PAD_PROTO_GIP;
    }

    if (intf->bInterfaceClass == USB_CLASS_HID)
    {
        if (dev_desc->idVendor == 0x057e && dev_desc->idProduct == 0x2009)
            return PAD_PROTO_SWITCH_PRO;
        /* Sony: DualShock 4 v1, v2 and its wireless adapter, then DualSense
           and DualSense Edge. Both send input with no init. */
        if (dev_desc->idVendor == 0x054c)
        {
            switch (dev_desc->idProduct)
            {
                case 0x05c4:
                case 0x09cc:
                case 0x0ba0:
                    return PAD_PROTO_DS4;
                case 0x0ce6:
                case 0x0df2:
                    return PAD_PROTO_DUALSENSE;
            }
        }
#if CONFIG_GPSP_USB_PAD_HID_DUMP
        return PAD_PROTO_HID;
#endif
    }

    return PAD_PROTO_NONE;
}

/* Picks the first alt 0 pad interface with an interrupt IN endpoint;
   composite devices may put other interfaces first. A plain HID interface
   (dump only) is taken only when no pad interface exists. */
static enum pad_proto pad_find_interface(const usb_device_desc_t *dev_desc,
        const usb_config_desc_t *config, const usb_intf_desc_t **intf_ret,
        const usb_ep_desc_t **in_ret, const usb_ep_desc_t **out_ret)
{
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)config;
    const usb_intf_desc_t *intf = NULL;
    const usb_ep_desc_t *in = NULL;
    const usb_ep_desc_t *out = NULL;
    enum pad_proto fallback = PAD_PROTO_NONE;
    int offset = 0;

    for (;;)
    {
        desc = usb_parse_next_descriptor(desc, config->wTotalLength, &offset);

        /* An interface ends at the next one or at the end of the list. */
        if (!desc || desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
        {
            if (intf && in)
            {
                enum pad_proto proto = pad_detect(dev_desc, intf);

                /* A pad wins over a saved HID candidate; HID keeps the first. */
                if (proto != PAD_PROTO_NONE &&
                        (proto != PAD_PROTO_HID || fallback == PAD_PROTO_NONE))
                {
                    *intf_ret = intf;
                    *in_ret = in;
                    *out_ret = out;
                }
                if (proto == PAD_PROTO_HID)
                    fallback = proto;
                else if (proto != PAD_PROTO_NONE)
                    return proto;
            }
            if (!desc)
                return fallback;

            intf = (const usb_intf_desc_t *)desc;
            if (intf->bAlternateSetting != 0)
                intf = NULL;
            in = NULL;
            out = NULL;
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
}

static void pad_close(void)
{
    ESP_LOGI(TAG, "Close addr=%u gone=%d in_err=%u out_err=%u recover=%u",
            s_pad.addr, s_pad.gone, s_pad.in_errors, s_pad.out_errors,
            s_pad.recoveries);
    if (s_pad.in_xfer)
        usb_host_transfer_free(s_pad.in_xfer);
    if (s_pad.out_xfer)
        usb_host_transfer_free(s_pad.out_xfer);
    if (s_pad.intf >= 0)
        usb_host_interface_release(s_client, s_pad.dev, s_pad.intf);
    usb_host_device_close(s_client, s_pad.dev);
    memset(&s_pad, 0, sizeof(s_pad));
    atomic_store(&s_buttons, 0);
}

static void pad_open(uint8_t addr)
{
    const usb_device_desc_t *dev_desc;
    const usb_config_desc_t *config;
    const usb_intf_desc_t *intf;
    const usb_ep_desc_t *in;
    const usb_ep_desc_t *out;
    uint16_t mps;
    esp_err_t result;
    const char *stage = "device descriptor";

    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.intf = -1;
    s_pad.addr = addr;
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    s_pad.opened_us = esp_timer_get_time();
#endif

    /* A pad that switches modes can leave before it is opened. */
    result = usb_host_device_open(s_client, addr, &s_pad.dev);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Open addr=%u failed: %s", addr, esp_err_to_name(result));
        s_pad.dev = NULL;
        return;
    }

    result = usb_host_get_device_descriptor(s_pad.dev, &dev_desc);
    if (result != ESP_OK)
        goto fail;
    s_pad.vid = dev_desc->idVendor;
    s_pad.pid = dev_desc->idProduct;
    stage = "config descriptor";
    result = usb_host_get_active_config_descriptor(s_pad.dev, &config);
    if (result != ESP_OK)
        goto fail;

    if (gpsp_usb_bt_match(dev_desc, config))
    {
        ESP_LOGI(TAG, "USB Bluetooth adapter %04x:%04x addr=%u",
                s_pad.vid, s_pad.pid, addr);
        pad_close();
        gpsp_usb_bt_attach(addr);
        return;
    }

    s_pad.proto = pad_find_interface(dev_desc, config, &intf, &in, &out);
    if (s_pad.proto == PAD_PROTO_NONE)
    {
        ESP_LOGI(TAG, "ignoring USB device %04x:%04x", dev_desc->idVendor,
                dev_desc->idProduct);
        pad_close();
        return;
    }

    stage = "claim interface";
    result = usb_host_interface_claim(s_client, s_pad.dev,
            intf->bInterfaceNumber, 0);
    if (result != ESP_OK)
        goto fail;
    s_pad.intf = intf->bInterfaceNumber;

    /* Interrupt IN transfers must be a multiple of the packet size. */
    mps = USB_EP_DESC_GET_MPS(in);
    stage = "allocate IN";
    result = usb_host_transfer_alloc(mps, 0, &s_pad.in_xfer);
    if (result != ESP_OK)
        goto fail;
    s_pad.in_xfer->device_handle = s_pad.dev;
    s_pad.in_xfer->bEndpointAddress = in->bEndpointAddress;
    s_pad.in_xfer->callback = pad_in_done;
    s_pad.in_xfer->num_bytes = mps;

    if (out)
    {
        stage = "allocate OUT";
        result = usb_host_transfer_alloc(PAD_OUT_BYTES, 0, &s_pad.out_xfer);
        if (result != ESP_OK)
            goto fail;
        s_pad.out_xfer->device_handle = s_pad.dev;
        s_pad.out_xfer->bEndpointAddress = out->bEndpointAddress;
        s_pad.out_xfer->callback = pad_out_done;
    }

    if (s_pad.proto == PAD_PROTO_XINPUT)
    {
        s_pad.init = s_xinput_init;
        s_pad.init_count = sizeof(s_xinput_init) / sizeof(s_xinput_init[0]);
    }
    else if (s_pad.proto == PAD_PROTO_GIP)
    {
        s_pad.init = s_gip_init;
        s_pad.init_count = sizeof(s_gip_init) / sizeof(s_gip_init[0]);
    }
    else if (s_pad.proto == PAD_PROTO_SWITCH_PRO)
    {
        s_pad.init = s_switch_init;
        s_pad.init_count = sizeof(s_switch_init) / sizeof(s_switch_init[0]);
    }
    s_pad.init_next_us = esp_timer_get_time() + 2 * PAD_INIT_GAP_US;

    stage = "submit IN";
    result = pad_submit(s_pad.in_xfer);
    if (result != ESP_OK)
        goto fail;
    s_pad.in_busy = true;

    ESP_LOGI(TAG, "USB pad %04x:%04x (%s) addr=%u intf=%d "
            "IN=0x%02x mps=%u interval=%u OUT=0x%02x",
            s_pad.vid, s_pad.pid, s_proto_names[s_pad.proto], addr, s_pad.intf,
            in->bEndpointAddress, mps, in->bInterval,
            out ? out->bEndpointAddress : 0);
    return;

fail:
    ESP_LOGW(TAG, "Setup addr=%u stage=%s: %s", addr, stage,
            esp_err_to_name(result));
    pad_close();
}

static void pad_send_init(void)
{
    const struct pad_packet *packet;
    usb_transfer_t *xfer = s_pad.out_xfer;
    int64_t now;

    if (!xfer || s_pad.gone || s_pad.out_busy ||
            s_pad.init_step >= s_pad.init_count)
        return;

    now = esp_timer_get_time();
    if (now < s_pad.init_next_us)
        return;
    s_pad.init_next_us = now + PAD_INIT_GAP_US;

    if (s_pad.init_tries++ >= PAD_INIT_TRIES)
    {
        ESP_LOGW(TAG, "init packet %u failed; pad may stay silent",
                s_pad.init_step);
        s_pad.init_step = s_pad.init_count;
        return;
    }

    if (s_pad.out_halted)
    {
        pad_endpoint_reset(xfer->bEndpointAddress);
        s_pad.out_halted = false;
    }

    packet = &s_pad.init[s_pad.init_step];
    memcpy(xfer->data_buffer, packet->data, packet->len);
    xfer->num_bytes = packet->len;
    if (pad_submit(xfer) == ESP_OK)
        s_pad.out_busy = true;
}

static void pad_recover_in(void)
{
    if (!s_pad.in_halted || s_pad.gone || s_pad.in_busy ||
            esp_timer_get_time() < s_pad.in_retry_us)
        return;

    pad_endpoint_reset(s_pad.in_xfer->bEndpointAddress);
    if (pad_submit(s_pad.in_xfer) == ESP_OK)
    {
        s_pad.in_halted = false;
        s_pad.in_busy = true;
        if (pad_log_error(++s_pad.recoveries))
            ESP_LOGI(TAG, "IN resubmitted addr=%u ep=0x%02x count=%u",
                    s_pad.addr, s_pad.in_xfer->bEndpointAddress,
                    s_pad.recoveries);
    }
    else
        s_pad.in_retry_us = esp_timer_get_time() + PAD_IN_RETRY_US;
}

static void pad_client_event(const usb_host_client_event_msg_t *msg,
        void *arg)
{
    (void)arg;

    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        ESP_LOGI(TAG, "USB NEW_DEV addr=%u active_addr=%u",
                msg->new_dev.address, s_pad.dev ? s_pad.addr : 0);
        s_new_addr = msg->new_dev.address;
    }
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
            msg->dev_gone.dev_hdl == s_pad.dev)
    {
        ESP_LOGW(TAG, "USB DEV_GONE addr=%u %04x:%04x busy=%d/%d",
                s_pad.addr, s_pad.vid, s_pad.pid, s_pad.in_busy, s_pad.out_busy);
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
        pad_health(esp_timer_get_time());
#endif
        s_pad.gone = true;
        atomic_store(&s_buttons, 0);
    }
}

static void pad_cancel(void)
{
    if (s_pad.in_busy)
    {
        usb_host_endpoint_halt(s_pad.dev, s_pad.in_xfer->bEndpointAddress);
        usb_host_endpoint_flush(s_pad.dev, s_pad.in_xfer->bEndpointAddress);
    }
    if (s_pad.out_busy)
    {
        usb_host_endpoint_halt(s_pad.dev, s_pad.out_xfer->bEndpointAddress);
        usb_host_endpoint_flush(s_pad.dev, s_pad.out_xfer->bEndpointAddress);
    }
}

static void pad_client_task(void *arg)
{
    (void)arg;
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    int64_t last_loop_us = esp_timer_get_time();
    int64_t next_health_us = last_loop_us + 5000000;
#endif

    while (atomic_load(&s_running))
    {
        /* Transfer callbacks also run inside this call. */
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        gpsp_usb_bt_poll();

        /* Close only after the pending transfers have returned. */
        if (s_pad.dev && s_pad.gone && !s_pad.in_busy && !s_pad.out_busy)
            pad_close();

        if (s_new_addr)
        {
            if (!s_pad.dev)
                pad_open(s_new_addr);
            s_new_addr = 0;
        }

        if (s_pad.dev)
        {
            pad_recover_in();
            pad_send_init();
        }
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
        int64_t now = esp_timer_get_time();
        if (s_pad.dev && now - last_loop_us > s_pad.loop_max_us)
            s_pad.loop_max_us = now - last_loop_us;
        last_loop_us = now;
        if (now >= next_health_us)
        {
            if (s_pad.dev)
                pad_health(now);
            next_health_us = now + 5000000;
        }
#endif
    }

    gpsp_usb_bt_shutdown();
    if (s_pad.dev)
    {
        pad_cancel();
        while (s_pad.in_busy || s_pad.out_busy)
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        pad_close();
    }
    usb_host_client_deregister(s_client);
    s_client = NULL;
    atomic_store(&s_client_active, false);
    s_client_task = NULL;
    vTaskDelete(NULL);
}

static void pad_lib_task(void *arg)
{
    TaskHandle_t parent = arg;
    const usb_host_config_t config = {
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        /* 0 selects the high-speed port, the USB-A socket. */
        .peripheral_map = 0,
    };
    esp_err_t result;
    bool waiting_free = false;

    /* Installed here so the USB interrupt runs on this core. */
    result = usb_host_install(&config);
    xTaskNotify(parent, (uint32_t)result, eSetValueWithOverwrite);
    if (result != ESP_OK)
    {
        s_lib_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host started core=%d priority=%u", xPortGetCoreID(),
            (unsigned)uxTaskPriorityGet(NULL));

    for (;;)
    {
        uint32_t flags = 0;

        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (atomic_load(&s_running))
            continue;

        /* The last deregister sets NO_CLIENTS. A failed start never had a
           client, and only wakes this task with usb_host_lib_unblock(). */
        if (!waiting_free && ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) ||
                !atomic_load(&s_client_active)))
        {
            if (usb_host_device_free_all() == ESP_OK)
                break;
            waiting_free = true;
        }
        if (waiting_free && (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE))
            break;
    }

    usb_host_uninstall();
    s_lib_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t gpsp_usb_pad_init(void)
{
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = pad_client_event,
        },
    };
    uint32_t notified = ESP_FAIL;
    esp_err_t result;

    if (s_lib_task)
        return ESP_OK;

    gpsp_bt_init();
    atomic_store(&s_running, true);
    atomic_store(&s_buttons, 0);
    if (xTaskCreatePinnedToCore(pad_lib_task, "gpsp_usb_lib", 4096,
            xTaskGetCurrentTaskHandle(), PAD_TASK_PRIORITY, &s_lib_task,
            PAD_TASK_CORE) != pdPASS)
    {
        s_lib_task = NULL;
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    xTaskNotifyWait(0, UINT32_MAX, &notified, portMAX_DELAY);
    result = (esp_err_t)notified;
    if (result != ESP_OK)
        goto fail;

    result = usb_host_client_register(&client_config, &s_client);
    if (result != ESP_OK)
        goto fail_lib;
    atomic_store(&s_client_active, true);

    if (xTaskCreatePinnedToCore(pad_client_task, "gpsp_usb_pad", 4096, NULL,
            PAD_TASK_PRIORITY, &s_client_task, PAD_TASK_CORE) != pdPASS)
    {
        usb_host_client_deregister(s_client);
        s_client = NULL;
        atomic_store(&s_client_active, false);
        s_client_task = NULL;
        result = ESP_ERR_NO_MEM;
        goto fail_lib;
    }
    return ESP_OK;

fail_lib:
    /* Wakes the library task, which then sees no client and exits. */
    atomic_store(&s_running, false);
    usb_host_lib_unblock();
fail:
    atomic_store(&s_running, false);
    ESP_LOGW(TAG, "USB host start failed: %s", esp_err_to_name(result));
    return result;
}

uint32_t gpsp_usb_pad_read(void)
{
    return atomic_load(&s_buttons) | gpsp_bt_read();
}

void gpsp_usb_pad_shutdown(void)
{
    if (!s_lib_task)
        return;

    /* The client task checks s_running every 10 ms, then deregisters. Its
       handle is not used here, since it may be gone at any moment. */
    atomic_store(&s_running, false);
    while (s_client_task)
        vTaskDelay(pdMS_TO_TICKS(5));
    while (s_lib_task)
        vTaskDelay(pdMS_TO_TICKS(5));
    atomic_store(&s_buttons, 0);
}

#else

esp_err_t gpsp_usb_pad_init(void)
{
    return ESP_OK;
}

uint32_t gpsp_usb_pad_read(void)
{
    return 0;
}

void gpsp_usb_pad_shutdown(void)
{
}

#endif

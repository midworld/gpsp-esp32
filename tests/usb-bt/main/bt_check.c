/* Exercise the production profile and Bluedroid host with a USB model.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "libretro.h"
#include "usb_model.h"
#include "../../../components/gpsp_devices/bt_pad.c"

void bt_menu_check(void);
void audio_mute_check(void);

/* Pads are not modeled over the air: HID calls and bonds are recorded here
   and callbacks are injected as Bluedroid would send them. */
static uint8_t bonds[CONFIG_BT_SMP_MAX_BONDS][6], connect_address[6],
        disconnect_address[6], removed_address[6];
static unsigned bond_count, connect_calls, disconnect_calls, removed_calls;
static esp_bt_connection_mode_t scan_mode;

esp_err_t __real_esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c_mode,
        esp_bt_discovery_mode_t d_mode);

esp_err_t __wrap_esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c_mode,
        esp_bt_discovery_mode_t d_mode)
{
    assert(d_mode == ESP_BT_NON_DISCOVERABLE);
    scan_mode = c_mode;
    return __real_esp_bt_gap_set_scan_mode(c_mode, d_mode);
}

esp_err_t __wrap_esp_bt_hid_host_connect(esp_bd_addr_t address)
{
    memcpy(connect_address, address, 6);
    connect_calls++;
    return ESP_OK;
}

esp_err_t __wrap_esp_bt_hid_host_disconnect(esp_bd_addr_t address)
{
    memcpy(disconnect_address, address, 6);
    disconnect_calls++;
    return ESP_OK;
}

esp_err_t __wrap_esp_bt_gap_get_bond_device_list(int *count, esp_bd_addr_t *list)
{
    int size = *count < (int)bond_count ? *count : (int)bond_count;

    memcpy(list, bonds, size * 6);
    *count = size;
    return ESP_OK;
}

esp_err_t __wrap_esp_bt_gap_remove_bond_device(esp_bd_addr_t address)
{
    memcpy(removed_address, address, 6);
    removed_calls++;
    for (unsigned i = 0; i < bond_count; i++)
        if (!memcmp(bonds[i], address, 6))
            memcpy(bonds[i], bonds[--bond_count], 6);
    esp_bt_gap_cb_param_t param = {.remove_bond_dev_cmpl = {.status = ESP_BT_STATUS_SUCCESS}};
    memcpy(param.remove_bond_dev_cmpl.bda, address, 6);
    bt_gap_event(ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT, &param);
    return ESP_OK;
}

static void bond(const uint8_t address[6])
{
    if (!bt_has_key(address))
        memcpy(bonds[bond_count++], address, 6);
}

static void settle(void)
{
    vTaskDelay(30);
}

#define COD_GAMEPAD 0x002508

static void gap_found(const uint8_t address[6], unsigned product, const char *name,
        uint32_t cod, bool hid_service)
{
    uint8_t eir[64] = {0};
    unsigned size = 0;

    if (product)
    {
        const uint8_t id[] = {9, 0x10, 1, 0, 0x4c, 0x05, product, product >> 8, 0, 1};
        memcpy(eir, id, sizeof(id));
        size = sizeof(id);
    }
    if (hid_service)
    {
        const uint8_t uuids[] = {5, ESP_BT_EIR_TYPE_CMPL_16BITS_UUID, 0x00, 0x12, 0x24, 0x11};
        memcpy(eir + size, uuids, sizeof(uuids));
        size += sizeof(uuids);
    }
    if (name)
    {
        eir[size] = strlen(name) + 1;
        eir[size + 1] = ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME;
        memcpy(eir + size + 2, name, strlen(name));
        size += strlen(name) + 2;
    }
    esp_bt_gap_dev_prop_t props[] = {
        {.type = ESP_BT_GAP_DEV_PROP_EIR, .len = size, .val = eir},
        {.type = ESP_BT_GAP_DEV_PROP_COD, .len = 4, .val = &cod},
    };
    esp_bt_gap_cb_param_t param = {.disc_res = {.num_prop = cod ? 2 : 1, .prop = props}};
    memcpy(param.disc_res.bda, address, 6);
    bt_gap_event(ESP_BT_GAP_DISC_RES_EVT, &param);
}

static void gap_auth(const uint8_t address[6], esp_bt_status_t status)
{
    esp_bt_gap_cb_param_t param = {.auth_cmpl = {.stat = status}};
    memcpy(param.auth_cmpl.bda, address, 6);
    bt_gap_event(ESP_BT_GAP_AUTH_CMPL_EVT, &param);
    settle();
}

static void hid_open(const uint8_t address[6], bool incoming, esp_hidh_status_t status,
        uint8_t handle)
{
    esp_hidh_cb_param_t param = {.open = {
        .status = status, .is_orig = !incoming, .handle = handle,
        .conn_status = status ? ESP_HIDH_CONN_STATE_DISCONNECTED :
                ESP_HIDH_CONN_STATE_CONNECTED,
    }};
    memcpy(param.open.bd_addr, address, 6);
    bt_hid_event(ESP_HIDH_OPEN_EVT, &param);
    settle();
}

static void hid_close(uint8_t handle)
{
    esp_hidh_cb_param_t param = {.close = {.handle = handle,
        .conn_status = ESP_HIDH_CONN_STATE_DISCONNECTED}};
    bt_hid_event(ESP_HIDH_CLOSE_EVT, &param);
    settle();
}

/* Shaped like Sony report 0x01 (four axes, hat, 14 buttons, Rx/Ry) with a
   vendor-page 0x31 input. */
static const uint8_t s_sony_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x0e, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x95, 0x02, 0x81, 0x02,
    0x06, 0x00, 0xff, 0x85, 0x31, 0x09, 0x3b, 0x75, 0x08, 0x95, 0x4d, 0x81, 0x02, 0xc0,
};

/* A pad with no Sony ID: report 0x01 with 15 buttons and a hat. */
static const uint8_t s_generic_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x0f, 0x81, 0x02, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01,
    0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x85, 0x02, 0x05, 0x0c, 0x0a, 0x23, 0x02, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, 0xc0,
};

/* Eight buttons, then 99 bytes of padding: a 101-byte report. */
static const uint8_t s_long_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x63, 0x81, 0x03, 0xc0,
};

static const uint8_t s_keyboard_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0,
};

static void hid_input(uint8_t handle, const uint8_t *report, unsigned size)
{
    esp_hidh_cb_param_t param = {.data_ind = {.status = ESP_HIDH_OK,
        .handle = handle, .len = size, .data = (uint8_t *)report}};
    bt_hid_event(ESP_HIDH_DATA_IND_EVT, &param);
}

/* A full Sony report with cross and PS pressed. */
static void hid_report(uint8_t handle, bool corrupt)
{
    uint8_t report[78] = {0x31};

    report[9] = 0x28;
    report[11] = 1;
    uint32_t crc = gpsp_playstation_crc(report, 74, 0xa1);
    for (unsigned i = 0; i < 4; i++)
        report[74 + i] = crc >> (8 * i);
    report[73] ^= corrupt;
    hid_input(handle, report, sizeof(report));
}

/* The descriptor the host reads after each open; it frees the copy after
   the callback returns. */
static void hid_dscp(uint8_t handle, uint16_t vendor, uint16_t product,
        const uint8_t *map, unsigned size, esp_hidh_status_t status)
{
    uint8_t *copy = malloc(size ? size : 1);

    memcpy(copy, map, size);
    esp_hidh_cb_param_t param = {.dscp = {.status = status, .handle = handle,
        .vendor_id = vendor, .product_id = product, .dl_len = size, .dsc_list = copy}};
    bt_hid_event(ESP_HIDH_GET_DSCP_EVT, &param);
    memset(copy, 0xa5, size);
    free(copy);
    settle();
}

static void hid_connect(const uint8_t address[6], bool incoming, uint8_t handle,
        unsigned product)
{
    hid_open(address, incoming, ESP_HIDH_OK, handle);
    hid_dscp(handle, 0x054c, product, s_sony_map, sizeof(s_sony_map), ESP_HIDH_OK);
}

static struct gpsp_bt_info wait_state(enum gpsp_bt_state state)
{
    struct gpsp_bt_info info;
    int64_t end = esp_timer_get_time() + 3000000;
    do
    {
        gpsp_bt_get_info(&info);
        if (info.state == state)
            return info;
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    } while (true);
}

static void action(enum gpsp_bt_action request, const uint8_t address[6])
{
    assert(gpsp_bt_request(request, address));
    settle();
}

static bool transport_active(void)
{
    struct gpsp_usb_bt_info info;

    gpsp_usb_bt_get_info(&info);
    return info.active;
}

static struct gpsp_bt_info start(void)
{
    usb_model_present(true);
    gpsp_usb_bt_attach(1);
    gpsp_usb_bt_poll();
    return wait_state(GPSP_BT_IDLE);
}

static void wait_stopped(int64_t timeout_us)
{
    int64_t end = esp_timer_get_time() + timeout_us;
    while (transport_active())
    {
        assert(esp_timer_get_time() < end);
        vTaskDelay(1);
    }
    vTaskDelay(20);
    assert(!usb_model_allocations() && !gpsp_bt_read());
}

/* The host rewrites its config in NVS, whose RAM index follows flash
   page use. Measure without it. */
static size_t heap_free(void)
{
    assert(nvs_flash_deinit() == ESP_OK);
    size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    assert(nvs_flash_init() == ESP_OK);
    return free;
}

static const uint8_t peers[4][6] = {
    {0x10, 2, 3, 4, 5, 6}, {0x20, 2, 3, 4, 5, 6}, {0x30, 2, 3, 4, 5, 6},
    {0x40, 2, 3, 4, 5, 6},
};

static void generic_check(void)
{
    const uint8_t pad[6] = {0x60, 2, 3, 4, 5, 6}, keys[6] = {0x61, 2, 3, 4, 5, 6};
    struct gpsp_bt_info info = start();

    /* A non-Sony pad found by class and named like a Sony pad. */
    action(GPSP_BT_SEARCH, NULL);
    gap_found(pad, 0, "Wireless Controller", COD_GAMEPAD, false);
    gap_found(keys, 0, "Gamepad", COD_GAMEPAD, false);
    settle();
    action(GPSP_BT_STOP_SEARCH, NULL);
    wait_state(GPSP_BT_IDLE);
    action(GPSP_BT_CONNECT, pad);
    bond(pad);
    hid_open(pad, false, ESP_HIDH_OK, 30);
    hid_dscp(30, 0x1234, 0x0001, s_generic_map, sizeof(s_generic_map), ESP_HIDH_OK);
    info = wait_state(GPSP_BT_CONNECTED);
    assert(info.devices[0].pad == GPSP_BT_PAD_HID && info.devices[0].paired);
    /* Buttons 1 and 3, hat right, then Home from report 0x02. */
    const uint8_t buttons[] = {0x01, 0x05, 0x00, 0x02}, home[] = {0x02, 0x01},
            idle[] = {0x01, 0x00, 0x00, 0x08};
    hid_input(30, buttons, sizeof(buttons));
    hid_input(30, home, sizeof(home));
    settle();
    assert(gpsp_bt_read() == ((1u << RETRO_DEVICE_ID_JOYPAD_B) |
            (1u << RETRO_DEVICE_ID_JOYPAD_Y) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT) |
            (1u << 16)));
    hid_input(30, idle, sizeof(idle));
    settle();
    assert(gpsp_bt_read() == (1u << 16));
    /* Full Sony report IDs mean nothing to other pads. */
    hid_report(30, false);
    settle();
    assert(gpsp_bt_read() == (1u << 16));
    hid_close(30);
    wait_state(GPSP_BT_IDLE);
    assert(!gpsp_bt_read());

    /* Reconnects read the descriptor again before input. */
    action(GPSP_BT_AUTO_ON, NULL);
    hid_open(pad, true, ESP_HIDH_OK, 31);
    hid_input(31, buttons, sizeof(buttons));
    settle();
    assert(!gpsp_bt_read());
    hid_dscp(31, 0x1234, 0x0001, s_generic_map, sizeof(s_generic_map), ESP_HIDH_OK);
    wait_state(GPSP_BT_CONNECTED);
    hid_input(31, buttons, sizeof(buttons));
    settle();
    assert(gpsp_bt_read() == ((1u << RETRO_DEVICE_ID_JOYPAD_B) |
            (1u << RETRO_DEVICE_ID_JOYPAD_Y) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)));
    action(GPSP_BT_DISCONNECT, NULL);
    hid_close(31);
    wait_state(GPSP_BT_IDLE);
    puts("USB_BT_CHECK generic HID pad: descriptor, reports, reconnect: pass");

    /* A keyboard is refused and keeps no key or registration. */
    unsigned removed = removed_calls;
    action(GPSP_BT_CONNECT, keys);
    bond(keys);
    hid_open(keys, false, ESP_HIDH_OK, 32);
    hid_dscp(32, 0x1234, 0x0002, s_keyboard_map, sizeof(s_keyboard_map), ESP_HIDH_OK);
    info = wait_state(GPSP_BT_DISCONNECTING);
    assert(info.error == GPSP_BT_UNSUPPORTED && !memcmp(disconnect_address, keys, 6));
    assert(removed_calls == removed + 1 && !bt_has_key(keys) && !s_pad->target);
    hid_close(32);
    info = wait_state(GPSP_BT_IDLE);
    assert(!info.devices[1].paired && info.error == GPSP_BT_UNSUPPORTED);

    /* Failed, oversized, and malformed descriptors are layout errors. */
    const uint8_t truncated[] = {0x05, 0x01, 0x09, 0x05, 0xa1};
    static uint8_t large[BT_DESCRIPTOR_MAX + 1];
    for (unsigned i = 0; i < 3; i++)
    {
        action(GPSP_BT_CONNECT, keys);
        hid_open(keys, false, ESP_HIDH_OK, 33 + i);
        if (i == 0)
            hid_dscp(33 + i, 0x1234, 2, s_generic_map, sizeof(s_generic_map), ESP_HIDH_ERR);
        else if (i == 1)
            hid_dscp(33 + i, 0x1234, 2, large, sizeof(large), ESP_HIDH_OK);
        else
            hid_dscp(33 + i, 0x1234, 2, truncated, sizeof(truncated), ESP_HIDH_OK);
        info = wait_state(GPSP_BT_DISCONNECTING);
        assert(info.error == GPSP_BT_LAYOUT_FAILED);
        hid_close(33 + i);
        wait_state(GPSP_BT_IDLE);
    }
    /* A registered pad stays registered after a layout error. */
    action(GPSP_BT_CONNECT, pad);
    hid_open(pad, false, ESP_HIDH_OK, 36);
    hid_dscp(36, 0x1234, 1, truncated, sizeof(truncated), ESP_HIDH_ERR);
    wait_state(GPSP_BT_DISCONNECTING);
    hid_close(36);
    info = wait_state(GPSP_BT_IDLE);
    assert(info.devices[0].paired && bt_has_key(pad) && s_pad->target);

    /* Descriptors of other or earlier connections leave the selected one
       waiting for its own. */
    action(GPSP_BT_CONNECT, pad);
    hid_open(pad, false, ESP_HIDH_OK, 37);
    hid_open(keys, true, ESP_HIDH_OK, 38);
    hid_dscp(38, 0x1234, 2, s_keyboard_map, sizeof(s_keyboard_map), ESP_HIDH_OK);
    hid_dscp(36, 0x1234, 1, truncated, sizeof(truncated), ESP_HIDH_OK);
    hid_dscp(39, 0x1234, 1, s_generic_map, sizeof(s_generic_map), ESP_HIDH_ERR);
    info = wait_state(GPSP_BT_CONNECTING);
    assert(info.error == GPSP_BT_OK && bt_has_key(pad) && s_pad->describing);
    hid_dscp(37, 0x1234, 1, s_long_map, sizeof(s_long_map), ESP_HIDH_OK);
    info = wait_state(GPSP_BT_CONNECTED);
    assert(info.devices[0].pad == GPSP_BT_PAD_HID);
    /* Reports past the event buffer keep their leading bytes. */
    static uint8_t report[200] = {0x01, 0x03};
    hid_input(37, report, 101);
    settle();
    assert(gpsp_bt_read() == ((1u << RETRO_DEVICE_ID_JOYPAD_B) |
            (1u << RETRO_DEVICE_ID_JOYPAD_A)));
    report[1] = 0x01;
    hid_input(37, report, sizeof(report));
    settle();
    assert(gpsp_bt_read() == (1u << RETRO_DEVICE_ID_JOYPAD_B));
    action(GPSP_BT_DISCONNECT, NULL);
    hid_close(37);
    hid_close(38);
    wait_state(GPSP_BT_IDLE);
    puts("USB_BT_CHECK descriptors match their connection; long reports: pass");

    /* Another Sony product uses the generic layout, without Home or full reports. */
    action(GPSP_BT_CONNECT, pad);
    hid_open(pad, false, ESP_HIDH_OK, 40);
    hid_dscp(40, 0x054c, 0x1234, s_sony_map, sizeof(s_sony_map), ESP_HIDH_OK);
    info = wait_state(GPSP_BT_CONNECTED);
    assert(info.devices[0].pad == GPSP_BT_PAD_HID && !s_pad->sony);
    const uint8_t ps[] = {0x01, 0x80, 0x80, 0x80, 0x80, 0x28, 0x00, 0x01, 0x80, 0x80};
    hid_input(40, ps, sizeof(ps));
    settle();
    assert(gpsp_bt_read() == (1u << RETRO_DEVICE_ID_JOYPAD_A));
    hid_report(40, false);
    settle();
    assert(gpsp_bt_read() == (1u << RETRO_DEVICE_ID_JOYPAD_A));
    action(GPSP_BT_DISCONNECT, NULL);
    hid_close(40);
    wait_state(GPSP_BT_IDLE);
    puts("USB_BT_CHECK unknown Sony products use the generic layout: pass");
    action(GPSP_BT_FORGET_ALL, NULL);
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    puts("USB_BT_CHECK unsupported and unreadable descriptors are refused: pass");
}

/* Pairing goes through Bluedroid: HCI events in, replies out. */
static void pairing_event(unsigned code, const uint8_t address[6], const uint8_t *extra,
        unsigned size)
{
    uint8_t params[16];

    for (unsigned i = 0; i < 6; i++)
        params[i] = address[5 - i];
    memcpy(params + 6, extra, size);
    usb_model_event(code, params, 6 + size);
    settle();
}

/* Replies may wait for a remote name first. */
static bool pairing_reply(unsigned opcode, const uint8_t address[6], uint8_t *params)
{
    int64_t end = esp_timer_get_time() + 2000000;

    while (!usb_model_command(opcode, params, 24))
    {
        if (esp_timer_get_time() >= end)
            return false;
        vTaskDelay(1);
    }
    for (unsigned i = 0; i < 6; i++)
        if (params[i] != address[5 - i])
            return false;
    return true;
}

static void security_start(const uint8_t pad[6])
{
    start();
    action(GPSP_BT_SEARCH, NULL);
    gap_found(pad, 0, "Gamepad", 0x002504, false);
    action(GPSP_BT_CONNECT, pad);
    wait_state(GPSP_BT_CONNECTING);
    usb_model_clear_commands();
}

static void security_stop(void)
{
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
}

static void security_check(void)
{
    const uint8_t pad[6] = {0x70, 2, 3, 4, 5, 6}, other[6] = {0x71, 2, 3, 4, 5, 6};
    uint8_t params[24];

    /* Legacy pairing: the selected pad gets 0000, another pad is refused. */
    security_start(pad);
    pairing_event(0x16, pad, NULL, 0);
    assert(pairing_reply(0x040d, pad, params) && params[6] == 4 &&
            !memcmp(params + 7, "0000", 4));
    security_stop();
    security_start(pad);
    pairing_event(0x16, other, NULL, 0);
    assert(pairing_reply(0x040e, other, params) && !usb_model_command(0x040d, params, 1));
    security_stop();

    /* Secure Simple Pairing: no input or output here, so Just Works is
       confirmed by the host and passkey entry is refused at once. */
    const uint8_t display[] = {0x01, 0x00, 0x03}, keyboard[] = {0x02, 0x00, 0x03},
            number[] = {0x40, 0xe2, 0x01, 0x00};
    security_start(pad);
    pairing_event(0x31, pad, NULL, 0);
    assert(pairing_reply(0x042b, pad, params) && params[6] == 0x03);
    pairing_event(0x32, pad, display, sizeof(display));
    pairing_event(0x33, pad, number, sizeof(number));
    assert(pairing_reply(0x042c, pad, params) && usb_model_command(0x0419, params, 1));
    security_stop();
    security_start(pad);
    pairing_event(0x31, pad, NULL, 0);
    pairing_event(0x32, pad, keyboard, sizeof(keyboard));
    pairing_event(0x34, pad, NULL, 0);
    assert(pairing_reply(0x042f, pad, params) && !usb_model_command(0x042e, params, 1));
    security_stop();
    puts("USB_BT_CHECK pairing through the host: PIN, Just Works, passkey refusal: pass");
}

static void pairing_check(void)
{
    struct gpsp_bt_info info = start();
    assert(info.persistent && info.auto_connect && !info.count);
    assert(scan_mode == ESP_BT_NON_CONNECTABLE && !s_pad->target);
    action(GPSP_BT_AUTO_OFF, NULL);
    gpsp_bt_get_info(&info);
    assert(!info.auto_connect);

    action(GPSP_BT_SEARCH, NULL);
    wait_state(GPSP_BT_SEARCHING);
    for (unsigned i = 0; i < 2; i++)
    {
        gap_found(peers[0], 0x09cc, NULL, 0, false);
        gap_found(peers[1], 0, "DualSense Wireless Controller", COD_GAMEPAD, false);
    }
    /* A name alone is not a candidate; a class or HID service is. */
    gap_found(peers[2], 0, "Wireless Controller", 0, false);
    gap_found(peers[3], 0, NULL, 0, true);
    settle();
    gpsp_bt_get_info(&info);
    assert(info.count == 3 && info.state == GPSP_BT_SEARCHING);
    assert(info.devices[0].pad == GPSP_BT_PAD_DS4 && info.devices[1].pad == GPSP_BT_PAD_UNKNOWN);
    assert(!strcmp(info.devices[1].name, "DualSense Wirel"));
    assert(!memcmp(info.devices[2].address, peers[3], 6) && !info.devices[2].name[0]);
    assert(info.devices[0].found && !info.devices[0].paired);
    action(GPSP_BT_STOP_SEARCH, NULL);
    wait_state(GPSP_BT_IDLE);
    bt_menu_check();

    /* Disconnect while paging waits for the open event, then closes it. */
    action(GPSP_BT_CONNECT, peers[0]);
    assert(connect_calls == 1 && !memcmp(connect_address, peers[0], 6));
    wait_state(GPSP_BT_CONNECTING);
    assert(scan_mode == ESP_BT_CONNECTABLE);
    action(GPSP_BT_DISCONNECT, NULL);
    wait_state(GPSP_BT_DISCONNECTING);
    assert(atomic_load(&s_manual_hold) && !disconnect_calls);
    hid_open(peers[0], false, ESP_HIDH_OK, 1);
    assert(disconnect_calls == 1 && !memcmp(disconnect_address, peers[0], 6));
    wait_state(GPSP_BT_DISCONNECTING);
    hid_close(1);
    info = wait_state(GPSP_BT_IDLE);
    assert(!info.devices[0].paired && !gpsp_bt_read());
    puts("USB_BT_CHECK connect cancel closes the late HID channel: pass");

    /* A failed open reports the error and returns to idle. */
    action(GPSP_BT_CONNECT, peers[0]);
    hid_open(peers[0], false, ESP_HIDH_ERR, 0);
    info = wait_state(GPSP_BT_IDLE);
    assert(info.error == GPSP_BT_CONNECT_FAILED);

    /* Sony: input waits for the descriptor; report 0x01 uses it, and a
       resumed full report replaces its state. */
    action(GPSP_BT_CONNECT, peers[1]);
    bond(peers[1]);
    gap_auth(peers[1], ESP_BT_STATUS_SUCCESS);
    assert(!removed_calls);
    hid_open(peers[1], false, ESP_HIDH_OK, 2);
    info = wait_state(GPSP_BT_CONNECTING);
    assert(!info.devices[1].paired);
    const uint8_t basic[] = {0x01, 0x80, 0x80, 0x80, 0x80, 0x28, 0x00, 0x00, 0x80, 0x80};
    hid_input(2, basic, sizeof(basic));
    settle();
    assert(!gpsp_bt_read() && !s_pad->decoded);
    hid_dscp(2, 0x054c, 0x0ce6, s_sony_map, sizeof(s_sony_map), ESP_HIDH_OK);
    info = wait_state(GPSP_BT_CONNECTED);
    assert(info.error == GPSP_BT_OK && info.devices[1].paired &&
            info.devices[1].pad == GPSP_BT_PAD_DS5 && !memcmp(info.active, peers[1], 6));
    /* Hat 8 is centered; cross and circle are B and A. */
    const uint8_t press[] = {0x01, 0x80, 0x80, 0x80, 0x80, 0x68, 0x00, 0x01, 0x80, 0x80};
    hid_input(2, press, sizeof(press));
    settle();
    assert(gpsp_bt_read() == ((1u << RETRO_DEVICE_ID_JOYPAD_B) |
            (1u << RETRO_DEVICE_ID_JOYPAD_A) | (1u << 16)));
    hid_report(2, false);
    settle();
    assert(gpsp_bt_read() == ((1u << 16) | 1));
    unsigned decoded = s_pad->decoded;
    hid_report(2, true);
    hid_report(3, false);
    settle();
    assert(s_pad->decoded == decoded);
    hid_input(2, basic, sizeof(basic));
    settle();
    assert(gpsp_bt_read() == (1u << RETRO_DEVICE_ID_JOYPAD_B));
    puts("USB_BT_CHECK Sony descriptor/basic and full reports/input routing: pass");

    /* Other pads may not stay connected or bonded. */
    hid_open(peers[0], true, ESP_HIDH_OK, 4);
    assert(disconnect_calls == 2 && !memcmp(disconnect_address, peers[0], 6));
    gap_auth(peers[2], ESP_BT_STATUS_SUCCESS);
    assert(removed_calls == 1 && !memcmp(removed_address, peers[2], 6));
    wait_state(GPSP_BT_CONNECTED);
    action(GPSP_BT_DISCONNECT, NULL);
    assert(disconnect_calls == 3 && !memcmp(disconnect_address, peers[1], 6));
    wait_state(GPSP_BT_DISCONNECTING);
    hid_close(2);
    info = wait_state(GPSP_BT_IDLE);
    assert(!gpsp_bt_read() && scan_mode == ESP_BT_NON_CONNECTABLE);
    puts("USB_BT_CHECK unselected pads closed and unbonded: pass");

    /* Saved pads wait for the pad to reconnect; boot never pages. */
    unsigned connects = connect_calls;
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    info = start();
    assert(info.count == 1 && info.devices[0].paired &&
            info.devices[0].pad == GPSP_BT_PAD_DS5 &&
            !info.auto_connect && s_pad->target && !memcmp(s_pad->peer, peers[1], 6));
    assert(scan_mode == ESP_BT_NON_CONNECTABLE);
    hid_open(peers[1], true, ESP_HIDH_OK, 5);
    assert(disconnect_calls == 4);
    hid_close(5);
    wait_state(GPSP_BT_IDLE);
    action(GPSP_BT_AUTO_ON, NULL);
    assert(scan_mode == ESP_BT_CONNECTABLE);
    hid_connect(peers[1], true, 6, 0x0ce6);
    wait_state(GPSP_BT_CONNECTED);
    hid_close(6);
    wait_state(GPSP_BT_IDLE);
    assert(connect_calls == connects && !usb_model_connection_attempts());
    puts("USB_BT_CHECK saved pad reconnect: listening without outgoing paging");

    action(GPSP_BT_FORGET, peers[1]);
    info = wait_state(GPSP_BT_IDLE);
    assert(!bt_has_key(peers[1]) && !s_pad->target && !info.devices[0].paired);
    assert(scan_mode == ESP_BT_NON_CONNECTABLE);
    bond(peers[0]);
    bond(peers[2]);
    action(GPSP_BT_FORGET_ALL, NULL);
    assert(!bond_count && !atomic_load(&s_pad->removing));
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    info = start();
    assert(!info.count && info.auto_connect && info.error == GPSP_BT_OK);

    /* Keys belong to one adapter. */
    action(GPSP_BT_SEARCH, NULL);
    gap_found(peers[1], 0x0ce6, NULL, 0, false);
    action(GPSP_BT_CONNECT, peers[1]);
    bond(peers[1]);
    hid_connect(peers[1], false, 7, 0x0ce6);
    wait_state(GPSP_BT_CONNECTED);
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    usb_model_address(0x41);
    info = start();
    assert(!info.count && !s_pad->target && !bond_count);
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    usb_model_address(1);
    puts("USB_BT_CHECK search/selection/bonds/forget/adapter change/reconnect setting: pass");

    /* Four registrations persist; a fifth pad is refused, not stored. */
    start();
    action(GPSP_BT_SEARCH, NULL);
    for (unsigned i = 0; i < 5; i++)
    {
        const uint8_t pad[6] = {0x50 + i, 2, 3, 4, 5, 6};
        gap_found(pad, 0x0ce6, NULL, 0, false);
    }
    settle();
    action(GPSP_BT_STOP_SEARCH, NULL);
    wait_state(GPSP_BT_IDLE);
    for (unsigned i = 0; i < 4; i++)
    {
        const uint8_t pad[6] = {0x50 + i, 2, 3, 4, 5, 6};
        action(GPSP_BT_CONNECT, pad);
        bond(pad);
        hid_connect(pad, false, 10 + i, 0x0ce6);
        wait_state(GPSP_BT_CONNECTED);
        hid_close(10 + i);
        wait_state(GPSP_BT_IDLE);
    }
    unsigned before = connect_calls;
    const uint8_t fifth[6] = {0x54, 2, 3, 4, 5, 6};
    action(GPSP_BT_CONNECT, fifth);
    info = wait_state(GPSP_BT_IDLE);
    assert(connect_calls == before && info.error == GPSP_BT_STORAGE_FAILED);
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    info = start();
    assert(info.count == 4);
    for (unsigned i = 0; i < 4; i++)
        assert(info.devices[i].paired && info.devices[i].address[0] == 0x50 + i);
    action(GPSP_BT_FORGET_ALL, NULL);
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    puts("USB_BT_CHECK four registrations persist; a fifth is refused: pass");

    generic_check();
}

static void overflow_check(void)
{
    size_t idle = heap_free();
    start();
    TaskHandle_t worker = xTaskGetHandle("gpsp_usb_bt");
    assert(worker);
    vTaskSuspend(worker);
    /* Queued and dropped descriptor copies are freed too. */
    hid_dscp(1, 0x1234, 1, s_generic_map, sizeof(s_generic_map), ESP_HIDH_OK);
    for (unsigned i = 0; i < BT_EVENTS + 4; i++)
        hid_report(1, false);
    hid_dscp(1, 0x1234, 1, s_generic_map, sizeof(s_generic_map), ESP_HIDH_OK);
    vTaskResume(worker);
    /* The host restarts rather than run with lost events. */
    int64_t end = esp_timer_get_time() + 3000000;
    while (!s_pad || atomic_load(&s_pad->overflow) || !s_pad->working)
    {
        assert(esp_timer_get_time() < end);
        gpsp_usb_bt_poll();
        vTaskDelay(1);
    }
    assert(!gpsp_bt_read());
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    assert(heap_free() == idle);
    puts("USB_BT_CHECK event queue overflow restarts the host: pass");

    /* A profile setup failure stops the host instead of restarting it. */
    start();
    esp_hidh_cb_param_t param = {.init = {.status = ESP_HIDH_ERR}};
    bt_hid_event(ESP_HIDH_INIT_EVT, &param);
    wait_stopped(5000000);
    struct gpsp_bt_info info = wait_state(GPSP_BT_ERROR);
    assert(info.error == GPSP_BT_INIT_FAILED);
    for (unsigned i = 0; i < 20; i++)
        gpsp_usb_bt_poll();
    assert(!transport_active());
    start();
    gpsp_usb_bt_shutdown();
    wait_stopped(5000000);
    puts("USB_BT_CHECK profile setup failure stops until the next attach: pass");
}

static void checks(void *arg)
{
    (void)arg;
    gpsp_bt_init();
    assert(nvs_flash_init() == ESP_OK);
    vTaskDelay(20);
    size_t before = heap_free();
    for (unsigned i = 0; i < 20; i++)
        gpsp_usb_bt_poll();
    assert(!transport_active() && !usb_model_allocations());
    assert(heap_free() == before);
    puts("USB_BT_CHECK absent: no runtime allocations");
    assert(!gpsp_bt_request(GPSP_BT_SEARCH, NULL));
    audio_mute_check();
    pairing_check();
    security_check();
    overflow_check();

    size_t steady = 0;
    for (unsigned cycle = 0; cycle < 4; cycle++)
    {
        start();
        action(GPSP_BT_SEARCH, NULL);
        gap_found(peers[1], 0x0ce6, NULL, 0, false);
        action(GPSP_BT_CONNECT, peers[1]);
        bond(peers[1]);
        hid_connect(peers[1], false, 1, 0x0ce6);
        wait_state(GPSP_BT_CONNECTED);
        hid_report(1, false);
        settle();
        assert(gpsp_bt_read());
        if (cycle & 1)
            gpsp_usb_bt_shutdown();
        else
            usb_model_present(false);
        wait_stopped(5000000);
        size_t free = heap_free();
        printf("USB_BT_CHECK cycle=%u free=%u\n", cycle, (unsigned)free);
        if (cycle)
            assert(free == steady);
        steady = free;
    }
    puts("USB_BT_CHECK attach/connect/input/detach cycles: pass");
    puts("USB_BT_CHECK PASS");
    vTaskDelete(NULL);
}

void app_main(void)
{
    assert(xTaskCreatePinnedToCore(checks, "bt_check", 8192, NULL, 4, NULL, 1) == pdPASS);
}

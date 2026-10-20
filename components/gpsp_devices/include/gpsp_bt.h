/* USB Bluetooth pairing controls.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BT_H
#define GPSP_BT_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#define GPSP_BT_DEVICES 8

enum gpsp_bt_state
{
    GPSP_BT_DISABLED, GPSP_BT_ABSENT, GPSP_BT_STARTING, GPSP_BT_IDLE,
    GPSP_BT_SEARCHING, GPSP_BT_CONNECTING, GPSP_BT_CONNECTED,
    GPSP_BT_DISCONNECTING, GPSP_BT_ERROR,
};

enum gpsp_bt_error
{
    GPSP_BT_OK, GPSP_BT_INIT_FAILED, GPSP_BT_INIT_TIMEOUT,
    GPSP_BT_NO_CLASSIC, GPSP_BT_CONNECT_FAILED, GPSP_BT_CONNECT_TIMEOUT,
    GPSP_BT_STORAGE_FAILED, GPSP_BT_LAYOUT_FAILED, GPSP_BT_UNSUPPORTED,
};

/* Known from the Device ID or the connected pad's HID descriptor. */
enum gpsp_bt_pad
{
    GPSP_BT_PAD_UNKNOWN, GPSP_BT_PAD_DS4, GPSP_BT_PAD_DS5, GPSP_BT_PAD_HID,
};

#define GPSP_BT_NAME_SIZE 16

struct gpsp_bt_device
{
    uint8_t address[6];
    enum gpsp_bt_pad pad;
    bool paired, found;
    char name[GPSP_BT_NAME_SIZE];
};

struct gpsp_bt_info
{
    enum gpsp_bt_state state;
    enum gpsp_bt_error error;
    uint8_t adapter[6], active[6];
    bool auto_connect, persistent;
    unsigned count, completed;
    struct gpsp_bt_device devices[GPSP_BT_DEVICES];
};

enum gpsp_bt_action
{
    GPSP_BT_SEARCH, GPSP_BT_STOP_SEARCH, GPSP_BT_CONNECT,
    GPSP_BT_DISCONNECT, GPSP_BT_FORGET, GPSP_BT_FORGET_ALL,
    GPSP_BT_AUTO_ON, GPSP_BT_AUTO_OFF,
};

#if CONFIG_GPSP_USB_BT
void gpsp_bt_init(void);
uint32_t gpsp_bt_read(void);
void gpsp_bt_get_info(struct gpsp_bt_info *info);
/* Queues work on CPU1. Connect/forget identify the selected pad by address. */
bool gpsp_bt_request(enum gpsp_bt_action action, const uint8_t address[6]);
#else
static inline void gpsp_bt_init(void) { }
static inline uint32_t gpsp_bt_read(void) { return 0; }
static inline void gpsp_bt_get_info(struct gpsp_bt_info *info)
{
    *info = (struct gpsp_bt_info){.state = GPSP_BT_DISABLED};
}
static inline bool gpsp_bt_request(enum gpsp_bt_action action,
        const uint8_t address[6])
{
    (void)action;
    (void)address;
    return false;
}
#endif

#endif

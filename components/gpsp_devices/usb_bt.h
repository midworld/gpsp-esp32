/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_USB_BT_H
#define GPSP_USB_BT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "usb/usb_types_stack.h"

enum gpsp_usb_bt_state
{
    GPSP_USB_BT_ABSENT, GPSP_USB_BT_STARTING, GPSP_USB_BT_READY, GPSP_USB_BT_ERROR,
};

struct gpsp_usb_bt_info
{
    enum gpsp_usb_bt_state state;
    esp_err_t error;
    unsigned received;
    bool active;
};

/* Hooks run on CPU1. Poll runs on the USB task after init: ESP_OK keeps the
   host, ESP_ERR_INVALID_STATE restarts it, and other errors stop it. Init,
   stop, and close run on the stack task around enable and disable; deinit
   runs after the host is released. */
struct gpsp_usb_bt_profile
{
    void (*attached)(void);
    esp_err_t (*init)(void);
    esp_err_t (*poll)(void);
    void (*stop)(bool removed);
    void (*close)(void);
    void (*deinit)(void);
};

#if CONFIG_GPSP_USB_BT
void gpsp_usb_bt_set_profile(const struct gpsp_usb_bt_profile *profile);
void gpsp_usb_bt_get_info(struct gpsp_usb_bt_info *info);
bool gpsp_usb_bt_match(const usb_device_desc_t *device,
        const usb_config_desc_t *config);
void gpsp_usb_bt_attach(uint8_t address);
void gpsp_usb_bt_poll(void);
void gpsp_usb_bt_shutdown(void);
#else
static inline void gpsp_usb_bt_set_profile(const struct gpsp_usb_bt_profile *profile)
{
    (void)profile;
}
static inline void gpsp_usb_bt_get_info(struct gpsp_usb_bt_info *info)
{
    *info = (struct gpsp_usb_bt_info){0};
}
static inline bool gpsp_usb_bt_match(const usb_device_desc_t *device,
        const usb_config_desc_t *config)
{
    (void)device;
    (void)config;
    return false;
}
static inline void gpsp_usb_bt_attach(uint8_t address) { (void)address; }
static inline void gpsp_usb_bt_poll(void) { }
static inline void gpsp_usb_bt_shutdown(void) { }
#endif

#endif

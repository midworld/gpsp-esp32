/* ESP32-P4 USB gamepad input on the high-speed OTG port.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_USB_PAD_H
#define GPSP_USB_PAD_H

#include <stdint.h>

#include "esp_err.h"

/* Supports XInput (Xbox 360 class), Xbox One, Switch Pro Controller,
   DualShock 4 and DualSense devices. Optional USB HCI adapters provide
   Bluetooth Classic connections for PlayStation pads. */
esp_err_t gpsp_usb_pad_init(void);

/**
 * Read the attached pad.
 *
 * @return A RetroPad bitmask with the home button in bit 16, or 0 without
 *         a pad.
 */
uint32_t gpsp_usb_pad_read(void);

void gpsp_usb_pad_shutdown(void);

#endif /* GPSP_USB_PAD_H */

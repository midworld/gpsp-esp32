/* Generic HID gamepad layout from a report descriptor.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_HID_GAMEPAD_H
#define GPSP_HID_GAMEPAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPSP_HID_GAMEPAD_FIELDS 32
#define GPSP_HID_GAMEPAD_REPORTS 8
/* Mapped controls must end within this many report bytes. */
#define GPSP_HID_GAMEPAD_REPORT_BYTES 128
/* Pressed controls kept from one array field. */
#define GPSP_HID_GAMEPAD_ARRAY_SLOTS 8

enum gpsp_hid_gamepad_result
{
    GPSP_HID_GAMEPAD_OK,
    GPSP_HID_GAMEPAD_MALFORMED,
    GPSP_HID_GAMEPAD_NOT_GAMEPAD,
};

/* Default meaning of HID button numbers. */
enum gpsp_hid_gamepad_layout
{
    /* South, east, west, north, L, R, L2, R2, Select, Start, L3, R3. */
    GPSP_HID_LAYOUT_SEQUENTIAL,
    /* DualShock 4 and DualSense: west, south, east, north, as above,
       then PS as Home. */
    GPSP_HID_LAYOUT_SONY,
};

struct gpsp_hid_gamepad_field
{
    uint32_t offset;
    int32_t min, max;
    uint16_t page, usage, usage_max;
    uint8_t report, size, kind, slot;
};

struct gpsp_hid_gamepad
{
    struct gpsp_hid_gamepad_field fields[GPSP_HID_GAMEPAD_FIELDS];
    uint32_t state[GPSP_HID_GAMEPAD_REPORTS];
    uint16_t bits[GPSP_HID_GAMEPAD_REPORTS];
    uint8_t reports[GPSP_HID_GAMEPAD_REPORTS];
    /* Dropped counts buttons and controls left unmapped. */
    uint8_t field_count, report_count, dropped;
    bool report_ids;
    enum gpsp_hid_gamepad_layout layout;
};

/* Maps the absolute input controls of joystick and gamepad application
   collections. The caller picks the layout for pads it knows. */
enum gpsp_hid_gamepad_result gpsp_hid_gamepad_init(struct gpsp_hid_gamepad *pad,
        const uint8_t *descriptor, size_t size, enum gpsp_hid_gamepad_layout layout);
/* Decodes one input report, starting with its ID when the descriptor uses
   IDs. Returns false for reports without mapped controls or too short for
   them. Each report keeps its own buttons; the result combines them. */
bool gpsp_hid_gamepad_decode(struct gpsp_hid_gamepad *pad, const uint8_t *report,
        size_t size, unsigned *buttons);
void gpsp_hid_gamepad_reset(struct gpsp_hid_gamepad *pad);

#ifdef __cplusplus
}
#endif

#endif

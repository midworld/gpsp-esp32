/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "playstation_pad.h"
#include "libretro.h"

static void pad_set(unsigned *buttons, bool pressed, unsigned id)
{
    if (pressed)
        *buttons |= 1u << id;
}

uint32_t gpsp_playstation_crc(const uint8_t *data, unsigned size, uint8_t prefix)
{
    uint32_t crc = 0xffffffff;

    for (unsigned i = 0; i <= size; i++)
    {
        crc ^= i ? data[i - 1] : prefix;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

bool gpsp_playstation_decode(const uint8_t *report, unsigned size,
        bool dualsense, bool bluetooth, unsigned *buttons)
{
    unsigned offset;

    if (!size)
        return false;
    if (bluetooth && report[0] == 0x11 && !dualsense)
        offset = 7;
    else if (bluetooth && report[0] == 0x31 && dualsense)
        offset = 9;
    else if (report[0] == 0x01)
        offset = dualsense && !bluetooth ? 8 : 5;
    else
        return false;
    if (size < offset + 3)
        return false;
    if (bluetooth && report[0] != 0x01)
    {
        if (size != 78)
            return false;
        uint32_t crc = (uint32_t)report[74] | ((uint32_t)report[75] << 8) |
                ((uint32_t)report[76] << 16) | ((uint32_t)report[77] << 24);

        if (gpsp_playstation_crc(report, 74, 0xa1) != crc)
            return false;
    }
    *buttons = 0;
    /* Sony's USB wireless adapter continues reporting after disconnection. */
    if (!dualsense && !bluetooth && size >= 32 && (report[31] & 4))
        return true;
    const uint8_t *r = report + offset;
    unsigned hat = r[0] & 15;

    if (hat <= 7)
    {
        pad_set(buttons, hat == 7 || hat <= 1, RETRO_DEVICE_ID_JOYPAD_UP);
        pad_set(buttons, hat >= 1 && hat <= 3, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        pad_set(buttons, hat >= 3 && hat <= 5, RETRO_DEVICE_ID_JOYPAD_DOWN);
        pad_set(buttons, hat >= 5, RETRO_DEVICE_ID_JOYPAD_LEFT);
    }
    pad_set(buttons, r[0] & 0x10, RETRO_DEVICE_ID_JOYPAD_Y);
    pad_set(buttons, r[0] & 0x20, RETRO_DEVICE_ID_JOYPAD_B);
    pad_set(buttons, r[0] & 0x40, RETRO_DEVICE_ID_JOYPAD_A);
    pad_set(buttons, r[0] & 0x80, RETRO_DEVICE_ID_JOYPAD_X);
    pad_set(buttons, r[1] & 0x01, RETRO_DEVICE_ID_JOYPAD_L);
    pad_set(buttons, r[1] & 0x02, RETRO_DEVICE_ID_JOYPAD_R);
    pad_set(buttons, r[1] & 0x04, RETRO_DEVICE_ID_JOYPAD_L2);
    pad_set(buttons, r[1] & 0x08, RETRO_DEVICE_ID_JOYPAD_R2);
    pad_set(buttons, r[1] & 0x10, RETRO_DEVICE_ID_JOYPAD_SELECT);
    pad_set(buttons, r[1] & 0x20, RETRO_DEVICE_ID_JOYPAD_START);
    pad_set(buttons, r[1] & 0x40, RETRO_DEVICE_ID_JOYPAD_L3);
    pad_set(buttons, r[1] & 0x80, RETRO_DEVICE_ID_JOYPAD_R3);
    pad_set(buttons, r[2] & 0x01, 16);
    return true;
}

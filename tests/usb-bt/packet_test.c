/* PlayStation button mapping checks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "playstation_pad.h"
#include "libretro.h"

static void finish_crc(uint8_t report[78])
{
    uint32_t crc = gpsp_playstation_crc(report, 74, 0xa1);
    for (unsigned i = 0; i < 4; i++)
        report[74 + i] = crc >> (8 * i);
}

static void buttons(void)
{
    const unsigned ids[] = {
        RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_X,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3, 16,
    };
    for (unsigned sense = 0; sense < 2; sense++)
        for (unsigned bt = 0; bt < 2; bt++)
            for (unsigned simple = 0; simple < (bt ? 2u : 1u); simple++)
            {
                uint8_t report[78] = {0};
                report[0] = bt && !simple ? (sense ? 0x31 : 0x11) : 1;
                unsigned offset = bt && !simple ? (sense ? 9 : 7) :
                        sense && !bt ? 8 : 5;
                unsigned size = bt && !simple ? 78 : offset + 3;
                for (unsigned bit = 0; bit < 13; bit++)
                {
                    memset(report + offset, 0, 3);
                    report[offset] = 8;
                    unsigned index = bit + 4;
                    report[offset + index / 8] |= 1u << (index % 8);
                    if (bt && !simple)
                        finish_crc(report);
                    unsigned result = 0;
                    assert(gpsp_playstation_decode(report, size, sense, bt, &result));
                    assert(result == (1u << ids[bit]));
                    for (unsigned length = 0; length < size; length++)
                        assert(!gpsp_playstation_decode(report, length, sense, bt, &result));
                    if (bt && !simple)
                    {
                        report[73] ^= 1;
                        assert(!gpsp_playstation_decode(report, size, sense, bt, &result));
                    }
                }
            }
    uint8_t disconnected[32] = {1};
    unsigned result;
    disconnected[31] = 4;
    assert(gpsp_playstation_decode(disconnected, 32, false, false, &result));
    assert(!result);
    /* Independent zlib CRC32 fixture for prefix A1 followed by 123456789. */
    assert(gpsp_playstation_crc((const uint8_t *)"123456789", 9, 0xa1) == 0x88ed2411u);
}

int main(void)
{
    buttons();
    puts("USB_BT_PAD_PACKET PASS: USB/BT buttons, report lengths, CRC");
    return 0;
}

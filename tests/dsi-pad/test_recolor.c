/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpsp_virtual_pad.h"
#include "gpsp_font.h"
#include "libretro.h"

#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)
static uint32_t *s_tiles[GPSP_PAD_REGIONS];
static uint32_t s_masks[GPSP_PAD_REGIONS];
#include "pad_layout.inc"
#include "pad_pictures.inc"

int main(void)
{
    unsigned checked = 0;

    for (unsigned region = 0; region < GPSP_PAD_REGIONS; region++)
    {
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
        size_t bytes = r->width * r->height * sizeof(uint32_t);
        uint32_t *want = malloc(bytes);
        uint32_t masks[16], bits[4];
        unsigned count = 0;

        s_tiles[region] = malloc(bytes);
        assert(want && s_tiles[region]);
        for (unsigned b = 0; b < 32; b++)
            if (s_region_masks[region] & (1u << b))
            {
                assert(count < sizeof(bits) / sizeof(bits[0]));
                bits[count++] = 1u << b;
            }
        for (unsigned m = 0; m < (1u << count); m++)
        {
            masks[m] = 0;
            for (unsigned b = 0; b < count; b++)
                if (m & (1u << b))
                    masks[m] |= bits[b];
        }
        for (unsigned to = 0; to < (1u << count); to++)
        {
            pad_build(region, masks[to]);
            memcpy(want, s_tiles[region], bytes);
            for (unsigned from = 0; from < (1u << count); from++)
            {
                pad_build(region, masks[from]);
                pad_update(region, masks[to]);
                assert(s_masks[region] == masks[to]);
                assert(!memcmp(want, s_tiles[region], bytes));
                checked++;
            }
        }
        free(want);
        free(s_tiles[region]);
    }
    printf("DSI pad recolor PASS: rotation=%u, %u state transitions\n",
            CONFIG_GPSP_LCD_ROTATE_180, checked);
    return 0;
}

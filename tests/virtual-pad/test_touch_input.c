/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "gpsp_virtual_pad.h"
#include "libretro.h"

#define CONFIG_GPSP_LCD_DSI 1
#define GPSP_BOARD_INPUT_HOME GPSP_VIRTUAL_PAD_HOME
#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

static atomic_uint s_snapshot, s_pressed;
static struct gpsp_virtual_pad s_pad;
static void (*s_changed)(void);
static uint32_t s_physical;
static uint16_t s_input_state;
#if CONFIG_GPSP_MENU
static uint32_t s_input_prev;
static bool s_input_blocked, s_menu_pending;
#endif
#if GPSP_FRAMESKIP
#if CONFIG_GPSP_MENU
static bool s_fast_forward;
#endif
static uint32_t gpsp_input_map(uint32_t input)
{
    return (input & ~BUTTON(A)) | (input & BUTTON(A) ? BUTTON(B) : 0);
}
#endif

bool gpsp_touch_available(void) { return true; }
uint32_t gpsp_board_input_read_physical(void) { return s_physical; }
#include "touch_input.inc"

static void publish(uint32_t buttons)
{
    s_pad.buttons = buttons;
    touch_publish();
}

static void tap(uint32_t buttons)
{
    publish(buttons);
    publish(0);
}

static void check_gameplay(void)
{
    tap(BUTTON(A));
    gpsp_input_poll();
    assert(s_input_state == BUTTON(A));
    gpsp_input_poll();
    assert(!s_input_state);

    publish(BUTTON(A));
    for (unsigned i = 0; i < 3; i++)
    {
        gpsp_input_poll();
        assert(s_input_state == BUTTON(A));
    }
    publish(0);
    gpsp_input_poll();
    assert(!s_input_state);

    s_physical = BUTTON(A);
    tap(BUTTON(A));
    gpsp_input_poll();
#if GPSP_FRAMESKIP
    assert(s_input_state == (BUTTON(A) | BUTTON(B)));
#else
    assert(s_input_state == BUTTON(A));
#endif
    s_physical = 0;

    tap(BUTTON(A));
    gpsp_touch_cancel();
    gpsp_input_poll();
    assert(!s_input_state);
    tap(BUTTON(A));
    gpsp_touch_set_enabled(false);
    gpsp_touch_set_enabled(true);
    gpsp_input_poll();
    assert(!s_input_state);
}

static void check_menu(void)
{
    s_physical = BUTTON(R);
    tap(BUTTON(B));
    assert(gpsp_board_input_read() == (BUTTON(R) | BUTTON(B)));
    assert(gpsp_board_input_read() == BUTTON(R));
    s_physical = 0;

#if CONFIG_GPSP_MENU
    tap(BUTTON(SELECT));
    tap(BUTTON(START));
    gpsp_input_poll();
    assert(!s_menu_pending);
    gpsp_input_poll();
    assert(!s_input_state);

#if GPSP_FRAMESKIP
    s_fast_forward = true;
#endif
    publish(BUTTON(SELECT) | BUTTON(START));
    gpsp_input_poll();
    assert(s_menu_pending && !s_input_state);
#if GPSP_FRAMESKIP
    assert(!s_fast_forward);
#endif
    s_menu_pending = false;
    gpsp_input_poll();
    assert(!s_menu_pending && !s_input_state);
    publish(0);
    gpsp_input_poll();
    assert(!s_input_blocked);

    tap(GPSP_VIRTUAL_PAD_HOME);
    gpsp_input_poll();
    assert(s_menu_pending && !s_input_state);
    s_menu_pending = false;
    gpsp_input_poll();
    assert(!s_menu_pending);

    s_input_blocked = true;
    s_physical = BUTTON(B);
    tap(BUTTON(A) | GPSP_VIRTUAL_PAD_HOME);
    gpsp_input_poll();
    assert(!s_menu_pending && !s_input_state);
    s_physical = 0;
    gpsp_input_poll();
    assert(!s_input_blocked);
#endif
}

int main(void)
{
    gpsp_touch_set_enabled(true);
    check_gameplay();
    check_menu();
    printf("TOUCH_INPUT PASS: menu=%u frameskip=%u\n",
            CONFIG_GPSP_MENU, GPSP_FRAMESKIP);
    return 0;
}

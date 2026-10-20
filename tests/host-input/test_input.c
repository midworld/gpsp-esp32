/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#define main gpsp_host_viewer_main
#include "../../tools/espemu-host/src/main.c"
#undef main

static void button(SDL_Joystick *joystick, SDL_GameControllerButton id, bool down)
{
    assert(SDL_JoystickSetVirtualButton(joystick, id, down) == 0);
    SDL_PumpEvents();
}

int main(void)
{
    struct gpsp_host_state state = {0};
    bool home_held = false;

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0);
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(index >= 0);
    SDL_GameController *controller = SDL_GameControllerOpen(index);
    assert(controller);
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);

    SDL_AtomicSet(&state.connected, 1);
    button(joystick, SDL_CONTROLLER_BUTTON_A, true);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(SDL_AtomicGet(&state.input_bits) == GPSP_ESPEMU_INPUT_B);
    button(joystick, SDL_CONTROLLER_BUTTON_B, true);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(SDL_AtomicGet(&state.input_bits) ==
            (GPSP_ESPEMU_INPUT_A | GPSP_ESPEMU_INPUT_B));
    button(joystick, SDL_CONTROLLER_BUTTON_A, false);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(SDL_AtomicGet(&state.input_bits) == GPSP_ESPEMU_INPUT_A);
    button(joystick, SDL_CONTROLLER_BUTTON_B, false);

    button(joystick, SDL_CONTROLLER_BUTTON_GUIDE, true);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(gpsp_command_queue_pop(&state.commands) == GPSP_ESPEMU_COMMAND_HOME);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(!gpsp_command_queue_pop(&state.commands));
    button(joystick, SDL_CONTROLLER_BUTTON_GUIDE, false);
    gpsp_host_poll_input(&state, controller, &home_held);
    SDL_AtomicSet(&state.connected, 0);
    button(joystick, SDL_CONTROLLER_BUTTON_GUIDE, true);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(!gpsp_command_queue_pop(&state.commands));
    SDL_AtomicSet(&state.connected, 1);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(!gpsp_command_queue_pop(&state.commands));
    button(joystick, SDL_CONTROLLER_BUTTON_GUIDE, false);
    gpsp_host_poll_input(&state, controller, &home_held);
    button(joystick, SDL_CONTROLLER_BUTTON_GUIDE, true);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(gpsp_command_queue_pop(&state.commands) == GPSP_ESPEMU_COMMAND_HOME);

    assert(SDL_JoystickDetachVirtual(index) == 0);
    gpsp_host_poll_input(&state, controller, &home_held);
    assert(!home_held && !SDL_AtomicGet(&state.input_bits));
    assert(!gpsp_command_queue_pop(&state.commands));
    SDL_GameControllerClose(controller);
    SDL_Quit();
    puts("Host input positions/Guide edges/disconnect: PASS");
    return 0;
}

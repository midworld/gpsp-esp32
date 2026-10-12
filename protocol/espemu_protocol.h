/* gpSP ESP-EMU host protocol.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_ESPEMU_PROTOCOL_H
#define GPSP_ESPEMU_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Every TCP message starts with this 24-byte header:
 *
 *   magic:u32, version:u16, type:u16, payload_size:u32,
 *   sequence:u32, timestamp_us:u64
 *
 * Header and metadata integers use big-endian byte order. RGB565 pixel data
 * uses little-endian byte order so an ESP32 frame can be copied without a
 * conversion pass.
 *
 * HELLO payload: width:u16, height:u16, format:u16, flags:u16, fps_mHz:u32
 * FRAME payload: width:u16, height:u16, pitch:u16, format:u16, pixel bytes
 *   The size is the game screen, or the menu screen while the menu is open.
 * INPUT payload: RetroPad bits:u16, one-shot command:u16
 * STATE_STATUS payload: command:u16, result:u16 (zero is success)
 * PROFILE payload: fifteen u32 values; see GPSP_ESPEMU_PROFILE_* offsets.
 *
 * Sequence numbers are local to each sender. A frame timestamp records when
 * the firmware accepted that frame, using the ESP-IDF microsecond timer.
 */
#define GPSP_ESPEMU_PROTOCOL_MAGIC 0x47505350u
#define GPSP_ESPEMU_PROTOCOL_VERSION 1u

#define GPSP_ESPEMU_HEADER_SIZE 24u
#define GPSP_ESPEMU_HELLO_SIZE 12u
#define GPSP_ESPEMU_FRAME_INFO_SIZE 8u
#define GPSP_ESPEMU_INPUT_SIZE 4u
#define GPSP_ESPEMU_STATE_STATUS_SIZE 4u
#define GPSP_ESPEMU_PROFILE_SIZE 60u

enum gpsp_espemu_profile_offset
{
    GPSP_ESPEMU_PROFILE_INTERVAL_US = 0,
    GPSP_ESPEMU_PROFILE_FRAMES = 4,
    GPSP_ESPEMU_PROFILE_RUN_AVG_US = 8,
    GPSP_ESPEMU_PROFILE_RUN_MAX_US = 12,
    GPSP_ESPEMU_PROFILE_WAIT_AVG_US = 16,
    GPSP_ESPEMU_PROFILE_WAIT_MAX_US = 20,
    GPSP_ESPEMU_PROFILE_COPY_COUNT = 24,
    GPSP_ESPEMU_PROFILE_COPY_AVG_US = 28,
    GPSP_ESPEMU_PROFILE_COPY_MAX_US = 32,
    GPSP_ESPEMU_PROFILE_SEND_COUNT = 36,
    GPSP_ESPEMU_PROFILE_SEND_AVG_US = 40,
    GPSP_ESPEMU_PROFILE_SEND_MAX_US = 44,
    GPSP_ESPEMU_PROFILE_SUBMITTED = 48,
    GPSP_ESPEMU_PROFILE_SENT = 52,
    GPSP_ESPEMU_PROFILE_DROPPED = 56,
};

#define GPSP_ESPEMU_SCREEN_WIDTH 240u
#define GPSP_ESPEMU_SCREEN_HEIGHT 160u
#define GPSP_ESPEMU_SCREEN_PITCH (GPSP_ESPEMU_SCREEN_WIDTH * 2u)
#define GPSP_ESPEMU_FRAME_BYTES \
    (GPSP_ESPEMU_SCREEN_PITCH * GPSP_ESPEMU_SCREEN_HEIGHT)

#define GPSP_ESPEMU_UI_WIDTH 320u
#define GPSP_ESPEMU_UI_HEIGHT 240u
#define GPSP_ESPEMU_UI_FRAME_BYTES \
    (GPSP_ESPEMU_UI_WIDTH * 2u * GPSP_ESPEMU_UI_HEIGHT)

enum gpsp_espemu_message_type
{
    GPSP_ESPEMU_MESSAGE_HELLO = 1,
    GPSP_ESPEMU_MESSAGE_FRAME = 2,
    GPSP_ESPEMU_MESSAGE_INPUT = 3,
    GPSP_ESPEMU_MESSAGE_PING = 4,
    GPSP_ESPEMU_MESSAGE_PONG = 5,
    GPSP_ESPEMU_MESSAGE_PROFILE = 6,
    GPSP_ESPEMU_MESSAGE_STATE_STATUS = 7,
};

enum gpsp_espemu_command
{
    GPSP_ESPEMU_COMMAND_NONE = 0,
    GPSP_ESPEMU_COMMAND_SAVE_STATE = 1,
    GPSP_ESPEMU_COMMAND_LOAD_STATE = 2,
    /* Serial input only: print the firmware's task table. */
    GPSP_ESPEMU_COMMAND_TASKS = 3,
    /* Serial input only: change the speaker volume by one step. */
    GPSP_ESPEMU_COMMAND_VOLUME_UP = 4,
    GPSP_ESPEMU_COMMAND_VOLUME_DOWN = 5,
    /* Serial input only: write saves, close the game, turn outputs off. */
    GPSP_ESPEMU_COMMAND_QUIT = 6,
    /* Serial input only: press the pad's home button (opens the menu). */
    GPSP_ESPEMU_COMMAND_HOME = 7,
    /* Serial input only: print the screen to the log as a PNG. */
    GPSP_ESPEMU_COMMAND_SCREENSHOT = 8,
};

enum gpsp_espemu_pixel_format
{
    GPSP_ESPEMU_PIXEL_RGB565_LE = 1,
};

enum gpsp_espemu_input
{
    /* Bit positions match libretro RetroPad IDs used by the gpSP frontend. */
    GPSP_ESPEMU_INPUT_B = 1u << 0,
    GPSP_ESPEMU_INPUT_Y = 1u << 1,
    GPSP_ESPEMU_INPUT_SELECT = 1u << 2,
    GPSP_ESPEMU_INPUT_START = 1u << 3,
    GPSP_ESPEMU_INPUT_UP = 1u << 4,
    GPSP_ESPEMU_INPUT_DOWN = 1u << 5,
    GPSP_ESPEMU_INPUT_LEFT = 1u << 6,
    GPSP_ESPEMU_INPUT_RIGHT = 1u << 7,
    GPSP_ESPEMU_INPUT_A = 1u << 8,
    GPSP_ESPEMU_INPUT_X = 1u << 9,
    GPSP_ESPEMU_INPUT_L = 1u << 10,
    GPSP_ESPEMU_INPUT_R = 1u << 11,
    GPSP_ESPEMU_INPUT_L2 = 1u << 12,
    GPSP_ESPEMU_INPUT_R2 = 1u << 13,
};

/* Read and write metadata byte by byte. This avoids alignment assumptions
 * and gives both the ESP32 and desktop builds the same wire byte order. */
static inline uint16_t gpsp_espemu_read_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static inline uint32_t gpsp_espemu_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
            ((uint32_t)data[2] << 8) | data[3];
}

static inline uint64_t gpsp_espemu_read_u64(const uint8_t *data)
{
    return ((uint64_t)gpsp_espemu_read_u32(data) << 32) |
            gpsp_espemu_read_u32(data + 4);
}

static inline void gpsp_espemu_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static inline void gpsp_espemu_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static inline void gpsp_espemu_write_u64(uint8_t *data, uint64_t value)
{
    gpsp_espemu_write_u32(data, (uint32_t)(value >> 32));
    gpsp_espemu_write_u32(data + 4, (uint32_t)value);
}

static inline void gpsp_espemu_write_header(
        uint8_t *data, uint16_t type, uint32_t payload_size,
        uint32_t sequence, uint64_t timestamp_us)
{
    gpsp_espemu_write_u32(data, GPSP_ESPEMU_PROTOCOL_MAGIC);
    gpsp_espemu_write_u16(data + 4, GPSP_ESPEMU_PROTOCOL_VERSION);
    gpsp_espemu_write_u16(data + 6, type);
    gpsp_espemu_write_u32(data + 8, payload_size);
    gpsp_espemu_write_u32(data + 12, sequence);
    gpsp_espemu_write_u64(data + 16, timestamp_us);
}

#endif /* GPSP_ESPEMU_PROTOCOL_H */

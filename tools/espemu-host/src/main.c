/* SDL2 host viewer for gpSP network video.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "espemu_protocol.h"
#include "gpsp_command_queue.h"
#include "net.h"
#include "serial.h"

#define GPSP_HOST_RX_CAPACITY \
    (2u * (GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_FRAME_INFO_SIZE + \
                    GPSP_ESPEMU_UI_FRAME_BYTES))
#define GPSP_HOST_INPUT_INTERVAL_MS 100u
#define GPSP_HOST_REPORT_INTERVAL_MS 2000u
#define GPSP_HOST_PROFILE_HISTORY 120u
#define GPSP_HOST_PROFILE_HEIGHT 80

struct gpsp_host_profile_sample
{
    uint64_t guest_us;
    uint32_t interval_us;
    uint32_t frames;
    uint32_t run_avg_us;
    uint32_t run_max_us;
    uint32_t wait_avg_us;
    uint32_t wait_max_us;
    uint32_t copy_count;
    uint32_t copy_avg_us;
    uint32_t copy_max_us;
    uint32_t send_count;
    uint32_t send_avg_us;
    uint32_t send_max_us;
    uint32_t submitted;
    uint32_t sent;
    uint32_t dropped;
    uint32_t received;
    double wall_fps;
};

/* Serial mode volume before the board reports one. */
#define GPSP_HOST_VOLUME_UNKNOWN 1000

struct gpsp_host_state
{
    const char *bind_address;
    uint16_t port;
    /* Serial mode: input goes to this port; the LCD shows the game. */
    const char *serial_path;
    const char *rom_name;
    bool serial_reset;
    /* Atomics carry small values between the SDL and network threads. */
    SDL_atomic_t running;
    SDL_atomic_t connected;
    SDL_atomic_t input_bits;
    struct gpsp_command_queue commands;
    /* Network thread only: wait for this state result before sending more. */
    uint16_t pending_command;
    SDL_atomic_t state_status;
    /* Serial mode: last reported volume (GPSP_HOST_VOLUME_UNKNOWN before
       one) and whether the board closed the game. */
    SDL_atomic_t volume_db;
    SDL_atomic_t game_closed;
    /* The mutex protects the frame fields and their pixel allocation. */
    SDL_mutex *frame_mutex;
    uint8_t *frame_pixels;
    /* Game screen, or the menu screen while the menu is open. */
    uint16_t frame_width;
    uint16_t frame_height;
    uint32_t frame_sequence;
    uint32_t received_frames;
    uint32_t report_frame_count;
    uint64_t report_start_ms;
    uint64_t report_start_guest_us;
    bool frame_ready;
    FILE *profile_csv;
    uint32_t connection_index;
    uint32_t target_frame_us;
    uint32_t profile_received_frames;
    uint64_t profile_host_ms;
    struct gpsp_host_profile_sample profiles[GPSP_HOST_PROFILE_HISTORY];
    unsigned profile_next;
    unsigned profile_count;
};

static void gpsp_host_command(struct gpsp_host_state *state, uint16_t command)
{
    bool state_command = command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
            command == GPSP_ESPEMU_COMMAND_LOAD_STATE;

    if (state_command)
        SDL_AtomicSet(&state->state_status, 0);
    if (!gpsp_command_queue_push(&state->commands, command))
    {
        fprintf(stderr, "Command queue full: %u\n", command);
        if (state_command)
            SDL_AtomicSet(&state->state_status, (command << 16) | 1);
    }
}

static uint16_t gpsp_host_tcp_command(struct gpsp_host_state *state)
{
    uint16_t command;

    if (state->pending_command)
        return 0;
    command = gpsp_command_queue_pop(&state->commands);
    if (command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
            command == GPSP_ESPEMU_COMMAND_LOAD_STATE)
        state->pending_command = command;
    return command;
}

static bool gpsp_host_publish_profile(
        struct gpsp_host_state *state, const uint8_t *payload,
        uint32_t payload_size, uint64_t timestamp_us)
{
    struct gpsp_host_profile_sample sample;
    uint64_t now_ms = SDL_GetTicks64();
    uint64_t elapsed_ms = now_ms - state->profile_host_ms;

    if (payload_size != GPSP_ESPEMU_PROFILE_SIZE)
        return false;

    sample.guest_us = timestamp_us;
    sample.interval_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_INTERVAL_US);
    sample.frames = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_FRAMES);
    sample.run_avg_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_RUN_AVG_US);
    sample.run_max_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_RUN_MAX_US);
    sample.wait_avg_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_WAIT_AVG_US);
    sample.wait_max_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_WAIT_MAX_US);
    sample.copy_count = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_COPY_COUNT);
    sample.copy_avg_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_COPY_AVG_US);
    sample.copy_max_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_COPY_MAX_US);
    sample.send_count = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_SEND_COUNT);
    sample.send_avg_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_SEND_AVG_US);
    sample.send_max_us = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_SEND_MAX_US);
    sample.submitted = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_SUBMITTED);
    sample.sent = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_SENT);
    sample.dropped = gpsp_espemu_read_u32(
            payload + GPSP_ESPEMU_PROFILE_DROPPED);
    sample.received = state->received_frames - state->profile_received_frames;
    sample.wall_fps = elapsed_ms == 0 ? 0.0 :
            (double)sample.received * 1000.0 / (double)elapsed_ms;
    state->profile_received_frames = state->received_frames;
    state->profile_host_ms = now_ms;

    SDL_LockMutex(state->frame_mutex);
    state->profiles[state->profile_next] = sample;
    state->profile_next =
            (state->profile_next + 1) % GPSP_HOST_PROFILE_HISTORY;

    if (state->profile_count < GPSP_HOST_PROFILE_HISTORY)
        state->profile_count++;

    SDL_UnlockMutex(state->frame_mutex);

    fprintf(stderr,
            "Profile: run=%u/%u wait=%u/%u copy=%u/%u send=%u/%u "
            "us avg/max frames=%u received=%u dropped=%u wall_fps=%.2f\n",
            sample.run_avg_us, sample.run_max_us,
            sample.wait_avg_us, sample.wait_max_us,
            sample.copy_avg_us, sample.copy_max_us,
            sample.send_avg_us, sample.send_max_us,
            sample.frames, sample.received, sample.dropped, sample.wall_fps);

    if (state->profile_csv)
    {
        int written = fprintf(state->profile_csv,
                "%u,%lld,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f\n",
                state->connection_index, (long long)time(NULL),
                (unsigned long long)sample.guest_us, sample.interval_us,
                sample.frames, sample.run_avg_us, sample.run_max_us,
                sample.wait_avg_us, sample.wait_max_us,
                sample.copy_count, sample.copy_avg_us, sample.copy_max_us,
                sample.send_count, sample.send_avg_us, sample.send_max_us,
                sample.submitted, sample.sent, sample.dropped,
                sample.received, state->target_frame_us, sample.wall_fps);

        if (written < 0 || fflush(state->profile_csv) != 0)
        {
            perror("Profile CSV write");
            fclose(state->profile_csv);
            state->profile_csv = NULL;
        }
    }

    return true;
}

/* The network thread publishes a complete frame while the SDL thread holds
 * the same mutex during SDL_UpdateTexture. This keeps SDL calls on the main
 * thread without exposing a half-written image. */
static bool gpsp_host_publish_frame(
        struct gpsp_host_state *state, const uint8_t *payload,
        uint32_t payload_size, uint32_t sequence, uint64_t timestamp_us)
{
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t format;
    uint8_t *destination;
    unsigned pixel;

    if (payload_size < GPSP_ESPEMU_FRAME_INFO_SIZE)
        return false;

    width = gpsp_espemu_read_u16(payload);
    height = gpsp_espemu_read_u16(payload + 2);
    pitch = gpsp_espemu_read_u16(payload + 4);
    format = gpsp_espemu_read_u16(payload + 6);

    if (!((width == GPSP_ESPEMU_SCREEN_WIDTH &&
                    height == GPSP_ESPEMU_SCREEN_HEIGHT) ||
            (width == GPSP_ESPEMU_UI_WIDTH &&
                    height == GPSP_ESPEMU_UI_HEIGHT)) ||
            pitch != width * 2u ||
            payload_size != GPSP_ESPEMU_FRAME_INFO_SIZE +
                    (uint32_t)pitch * height ||
            format != GPSP_ESPEMU_PIXEL_RGB565_LE)
        return false;

    SDL_LockMutex(state->frame_mutex);
    destination = state->frame_pixels;
    memcpy(destination, payload + GPSP_ESPEMU_FRAME_INFO_SIZE,
            (size_t)pitch * height);
    state->frame_width = width;
    state->frame_height = height;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    /* SDL's RGB565 texture expects native-endian 16-bit pixel values. */
    for (pixel = 0; pixel < (unsigned)width * height; pixel++)
    {
        uint16_t value;
        memcpy(&value, destination + pixel * sizeof(value), sizeof(value));
        value = SDL_Swap16(value);
        memcpy(destination + pixel * sizeof(value), &value, sizeof(value));
    }
#else
    (void)pixel;
#endif

    if (!state->frame_ready)
        fprintf(stderr, "First video frame received (sequence %u)\n", sequence);

    state->received_frames++;

    if (state->report_start_ms == 0)
    {
        state->report_start_ms = SDL_GetTicks64();
        state->report_start_guest_us = timestamp_us;
        state->report_frame_count = state->received_frames;
    }
    else
    {
        uint64_t now_ms = SDL_GetTicks64();
        uint64_t report_time_ms = now_ms - state->report_start_ms;

        if (report_time_ms >= GPSP_HOST_REPORT_INTERVAL_MS)
        {
            uint32_t report_frames =
                    state->received_frames - state->report_frame_count;
            uint64_t guest_time_us =
                    timestamp_us - state->report_start_guest_us;
            double wall_fps =
                    (double)report_frames * 1000.0 / (double)report_time_ms;
            double guest_fps = guest_time_us == 0 ? 0.0 :
                    (double)report_frames * 1000000.0 / (double)guest_time_us;
            double realtime = (double)guest_time_us /
                    ((double)report_time_ms * 1000.0) * 100.0;

            fprintf(stderr,
                    "Video receive: frames=%u wall_fps=%.3f guest_fps=%.3f "
                    "realtime=%.1f%%\n",
                    state->received_frames, wall_fps, guest_fps, realtime);
            state->report_start_ms = now_ms;
            state->report_start_guest_us = timestamp_us;
            state->report_frame_count = state->received_frames;
        }
    }

    state->frame_sequence = sequence;
    state->frame_ready = true;
    SDL_UnlockMutex(state->frame_mutex);
    return true;
}

static bool gpsp_host_parse_messages(
        struct gpsp_host_state *state, uint8_t *buffer, size_t *buffer_size)
{
    /* TCP has no message boundaries. Consume every complete protocol message
     * and leave a partial tail at the start of the receive buffer. */
    while (*buffer_size >= GPSP_ESPEMU_HEADER_SIZE)
    {
        uint32_t magic = gpsp_espemu_read_u32(buffer);
        uint16_t version = gpsp_espemu_read_u16(buffer + 4);
        uint16_t type = gpsp_espemu_read_u16(buffer + 6);
        uint32_t payload_size = gpsp_espemu_read_u32(buffer + 8);
        uint32_t sequence;
        uint64_t timestamp_us;
        size_t message_size;
        const uint8_t *payload;

        if (magic != GPSP_ESPEMU_PROTOCOL_MAGIC ||
                version != GPSP_ESPEMU_PROTOCOL_VERSION ||
                payload_size >
                        GPSP_HOST_RX_CAPACITY - GPSP_ESPEMU_HEADER_SIZE)
            return false;

        sequence = gpsp_espemu_read_u32(buffer + 12);
        timestamp_us = gpsp_espemu_read_u64(buffer + 16);
        message_size = GPSP_ESPEMU_HEADER_SIZE + payload_size;
        payload = buffer + GPSP_ESPEMU_HEADER_SIZE;

        if (*buffer_size < message_size)
            break;

        if (type == GPSP_ESPEMU_MESSAGE_HELLO)
        {
            if (payload_size != GPSP_ESPEMU_HELLO_SIZE ||
                    gpsp_espemu_read_u16(payload) !=
                            GPSP_ESPEMU_SCREEN_WIDTH ||
                    gpsp_espemu_read_u16(payload + 2) !=
                            GPSP_ESPEMU_SCREEN_HEIGHT ||
                    gpsp_espemu_read_u16(payload + 4) !=
                            GPSP_ESPEMU_PIXEL_RGB565_LE)
                return false;

            fprintf(stderr,
                    "gpSP stream: %ux%u RGB565, nominal %.3f FPS\n",
                    gpsp_espemu_read_u16(payload),
                    gpsp_espemu_read_u16(payload + 2),
                    gpsp_espemu_read_u32(payload + 8) / 1000.0);
            SDL_LockMutex(state->frame_mutex);
            state->target_frame_us = gpsp_espemu_read_u32(payload + 8) == 0 ? 0 :
                    1000000000u / gpsp_espemu_read_u32(payload + 8);
            SDL_UnlockMutex(state->frame_mutex);
        }
        else if (type == GPSP_ESPEMU_MESSAGE_FRAME)
        {
            if (!gpsp_host_publish_frame(
                    state, payload, payload_size, sequence, timestamp_us))
                return false;
        }
        else if (type == GPSP_ESPEMU_MESSAGE_PROFILE)
        {
            if (!gpsp_host_publish_profile(
                    state, payload, payload_size, timestamp_us))
                return false;
        }
        else if (type == GPSP_ESPEMU_MESSAGE_STATE_STATUS)
        {
            if (payload_size != GPSP_ESPEMU_STATE_STATUS_SIZE)
                return false;

            uint16_t command = gpsp_espemu_read_u16(payload);
            uint16_t result = gpsp_espemu_read_u16(payload + 2);

            if (!state->pending_command || command != state->pending_command ||
                    result > 1)
                return false;
            state->pending_command = 0;
            SDL_AtomicSet(&state->state_status, (command << 16) | result);
            fprintf(stderr, "State %s %s\n",
                    command == GPSP_ESPEMU_COMMAND_SAVE_STATE ? "save" : "load",
                    result == 0 ? "succeeded" : "failed");
        }

        *buffer_size -= message_size;
        memmove(buffer, buffer + message_size, *buffer_size);
    }

    return true;
}

/* Serialize one input update for the network thread's partial-send state. */
static void gpsp_host_prepare_input(
        uint8_t *buffer, size_t *size, size_t *offset,
        uint32_t sequence, uint16_t input_bits, uint16_t command)
{
    gpsp_espemu_write_header(buffer, GPSP_ESPEMU_MESSAGE_INPUT,
            GPSP_ESPEMU_INPUT_SIZE, sequence, 0);
    gpsp_espemu_write_u16(buffer + GPSP_ESPEMU_HEADER_SIZE, input_bits);
    gpsp_espemu_write_u16(buffer + GPSP_ESPEMU_HEADER_SIZE + 2, command);
    *size = GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_INPUT_SIZE;
    *offset = 0;
}

/* Own all sockets and publish completed frames to the SDL main thread. */
static int gpsp_host_network_thread(void *argument)
{
    struct gpsp_host_state *state = argument;
    gpsp_host_socket_t listener;
    gpsp_host_socket_t client = GPSP_HOST_INVALID_SOCKET;
    uint8_t *receive_buffer;
    size_t receive_size = 0;
    uint8_t transmit_buffer[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_INPUT_SIZE];
    size_t transmit_size = 0;
    size_t transmit_offset = 0;
    uint32_t transmit_sequence = 0;
    uint16_t last_input = UINT16_MAX;
    uint64_t last_input_ms = 0;

    if (!gpsp_host_net_init())
    {
        fprintf(stderr, "Failed to initialize sockets\n");
        SDL_AtomicSet(&state->running, 0);
        return 1;
    }

    /* The viewer is the network server even though it is the user-facing
     * client. ESP-EMU can reach a host listener through its virtual gateway. */
    listener = gpsp_host_net_listen(state->bind_address, state->port);

    if (listener == GPSP_HOST_INVALID_SOCKET)
    {
        fprintf(stderr, "Failed to listen on %s:%u\n",
                state->bind_address, state->port);
        gpsp_host_net_shutdown();
        SDL_AtomicSet(&state->running, 0);
        return 1;
    }

    receive_buffer = malloc(GPSP_HOST_RX_CAPACITY);

    if (!receive_buffer)
    {
        fprintf(stderr, "Failed to allocate the receive buffer\n");
        gpsp_host_net_close(listener);
        gpsp_host_net_shutdown();
        SDL_AtomicSet(&state->running, 0);
        return 1;
    }

    fprintf(stderr, "Waiting for gpSP on %s:%u\n",
            state->bind_address, state->port);

    while (SDL_AtomicGet(&state->running))
    {
        bool disconnected = false;

        if (client == GPSP_HOST_INVALID_SOCKET)
        {
            client = gpsp_host_net_accept(listener);

            if (client == GPSP_HOST_INVALID_SOCKET)
            {
                SDL_Delay(10);
                continue;
            }

            receive_size = 0;
            transmit_size = 0;
            transmit_offset = 0;
            state->pending_command = 0;
            last_input = UINT16_MAX;
            last_input_ms = 0;
            state->received_frames = 0;
            state->report_frame_count = 0;
            state->report_start_ms = 0;
            state->report_start_guest_us = 0;
            state->profile_received_frames = 0;
            state->profile_host_ms = SDL_GetTicks64();
            SDL_LockMutex(state->frame_mutex);
            state->connection_index++;
            state->profile_next = 0;
            state->profile_count = 0;
            state->frame_ready = false;
            SDL_UnlockMutex(state->frame_mutex);
            SDL_AtomicSet(&state->connected, 1);
            SDL_AtomicSet(&state->state_status, 0);
            fprintf(stderr, "gpSP connected\n");
        }

        while (receive_size < GPSP_HOST_RX_CAPACITY)
        {
            int received = gpsp_host_net_receive(client,
                    receive_buffer + receive_size,
                    GPSP_HOST_RX_CAPACITY - receive_size);

            if (received > 0)
            {
                receive_size += (size_t)received;

                if (!gpsp_host_parse_messages(
                        state, receive_buffer, &receive_size))
                {
                    disconnected = true;
                    break;
                }

                continue;
            }

            if (received == 0 || !gpsp_host_net_would_block())
                disconnected = true;

            break;
        }

        if (!disconnected && receive_size == GPSP_HOST_RX_CAPACITY)
            disconnected = true;

        if (!disconnected && transmit_size == 0)
        {
            uint64_t now_ms = SDL_GetTicks64();
            uint16_t input = (uint16_t)SDL_AtomicGet(&state->input_bits);
            uint16_t command = gpsp_host_tcp_command(state);

            /* Periodic repeats let firmware release stale buttons after this
             * process or its connection disappears unexpectedly. */
            if (command || input != last_input ||
                    now_ms - last_input_ms >= GPSP_HOST_INPUT_INTERVAL_MS)
            {
                if (input != last_input)
                    fprintf(stderr, "Input changed: bits=0x%03x\n", input);

                gpsp_host_prepare_input(transmit_buffer, &transmit_size,
                        &transmit_offset, transmit_sequence++, input, command);
                last_input = input;
                last_input_ms = now_ms;
            }
        }

        /* Preserve unsent bytes so a short non-blocking send cannot split the
         * protocol stream incorrectly. */
        if (!disconnected && transmit_size > 0)
        {
            int sent = gpsp_host_net_send(client,
                    transmit_buffer + transmit_offset,
                    transmit_size - transmit_offset);

            if (sent > 0)
            {
                transmit_offset += (size_t)sent;

                if (transmit_offset == transmit_size)
                {
                    transmit_size = 0;
                    transmit_offset = 0;
                }
            }
            else if (sent == 0 || !gpsp_host_net_would_block())
            {
                disconnected = true;
            }
        }

        if (disconnected)
        {
            gpsp_host_net_close(client);
            client = GPSP_HOST_INVALID_SOCKET;
            SDL_AtomicSet(&state->connected, 0);
            if (state->pending_command)
            {
                fprintf(stderr, "Command cancelled on disconnect: %u\n",
                        state->pending_command);
                SDL_AtomicSet(&state->state_status,
                        (state->pending_command << 16) | 1);
                state->pending_command = 0;
            }
            for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
            {
                uint16_t command = gpsp_command_queue_pop(&state->commands);

                if (!command)
                    break;
                fprintf(stderr, "Command cancelled on disconnect: %u\n", command);
                if (command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
                        command == GPSP_ESPEMU_COMMAND_LOAD_STATE)
                    SDL_AtomicSet(&state->state_status, (command << 16) | 1);
            }
            fprintf(stderr, "gpSP disconnected\n");
        }
        else
        {
            SDL_Delay(1);
        }
    }

    gpsp_host_net_close(client);
    gpsp_host_net_close(listener);
    gpsp_host_net_shutdown();
    free(receive_buffer);
    return 0;
}

/* Sends one input update; the payload matches the network message. */
static bool gpsp_host_serial_send_input(
        int port, uint32_t sequence, uint16_t input_bits, uint16_t command)
{
    uint8_t buffer[GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_INPUT_SIZE];
    size_t size;
    size_t offset;

    gpsp_host_prepare_input(buffer, &size, &offset, sequence, input_bits,
            command);
    return gpsp_host_serial_write(port, buffer, size);
}

/* Handles one device log line: print it, answer the ROM prompt and pick
 * up state results. */
static void gpsp_host_serial_line(struct gpsp_host_state *state, int port,
        const char *line, bool *rom_sent, bool *input_ready)
{
    if (strstr(line, "Serial input ready"))
        *input_ready = true;
    /* A new boot: drop what the last session reported. */
    if (strstr(line, "ESP-ROM:"))
    {
        SDL_AtomicSet(&state->volume_db, GPSP_HOST_VOLUME_UNKNOWN);
        SDL_AtomicSet(&state->game_closed, 0);
    }

    const char *status = strstr(line, "SERIAL_STATE command=");

    printf("%s\n", line);
    fflush(stdout);

    if (state->rom_name && !*rom_sent && strstr(line, "ROM name?"))
    {
        gpsp_host_serial_write(port, state->rom_name, strlen(state->rom_name));
        gpsp_host_serial_write(port, "\n", 1);
        *rom_sent = true;
        fprintf(stderr, "Sent ROM name: %s\n", state->rom_name);
    }

    if (status)
    {
        unsigned command = 0;
        unsigned ok = 0;

        if (sscanf(status, "SERIAL_STATE command=%u ok=%u", &command, &ok) == 2)
            SDL_AtomicSet(&state->state_status,
                    (int)((command << 16) | (ok ? 0u : 1u)));
    }

    const char *volume = strstr(line, "SERIAL_VOLUME db=");
    int db;

    if (volume && sscanf(volume, "SERIAL_VOLUME db=%d", &db) == 1)
        SDL_AtomicSet(&state->volume_db, db);
    if (strstr(line, "Game closed by request"))
        SDL_AtomicSet(&state->game_closed, 1);
}

/* Own the serial port: forward input and print the device log. */
static int gpsp_host_serial_thread(void *argument)
{
    struct gpsp_host_state *state = argument;
    char line[512];
    size_t line_size = 0;
    bool rom_sent = false;
    bool input_ready = true;
    uint32_t sequence = 0;
    uint16_t last_input = UINT16_MAX;
    uint64_t last_input_ms = 0;
    int port = gpsp_host_serial_open(state->serial_path);

    if (port < 0)
    {
        SDL_AtomicSet(&state->running, 0);
        return 1;
    }

    /* The ROM prompt appears only at boot. */
    if (state->serial_reset || state->rom_name)
    {
        gpsp_host_serial_reset(port);
        /* Input bytes would corrupt the ROM name the board reads at boot. */
        input_ready = false;
    }
    SDL_AtomicSet(&state->connected, 1);
    fprintf(stderr, "Serial input on %s\n", state->serial_path);

    while (SDL_AtomicGet(&state->running))
    {
        uint8_t data[256];
        int count = gpsp_host_serial_read(port, data, sizeof(data));
        uint64_t now_ms = SDL_GetTicks64();
        uint16_t input = (uint16_t)SDL_AtomicGet(&state->input_bits);
        uint16_t command;

        if (count < 0)
        {
            fprintf(stderr, "Serial port closed\n");
            break;
        }

        for (int i = 0; i < count; i++)
        {
            if (data[i] == '\n' || line_size == sizeof(line) - 1)
            {
                line[line_size] = '\0';
                if (line_size && line[line_size - 1] == '\r')
                    line[line_size - 1] = '\0';
                gpsp_host_serial_line(state, port, line, &rom_sent,
                        &input_ready);
                line_size = 0;
            }
            else
                line[line_size++] = (char)data[i];
        }

        command = input_ready ? gpsp_command_queue_pop(&state->commands) : 0;
        /* Repeats let the firmware release buttons if this process stops. */
        if (input_ready && (command || input != last_input ||
                now_ms - last_input_ms >= GPSP_HOST_INPUT_INTERVAL_MS))
        {
            if (!gpsp_host_serial_send_input(port, sequence++, input, command))
            {
                fprintf(stderr, "Serial write failed\n");
                break;
            }
            last_input = input;
            last_input_ms = now_ms;
        }

        if (count == 0)
            SDL_Delay(2);
    }

    SDL_AtomicSet(&state->connected, 0);
    SDL_AtomicSet(&state->running, 0);
    gpsp_host_serial_close(port);
    return 0;
}

static SDL_GameController *gpsp_host_open_controller(void)
{
    int joystick;

    for (joystick = 0; joystick < SDL_NumJoysticks(); joystick++)
    {
        if (SDL_IsGameController(joystick))
        {
            SDL_GameController *controller = SDL_GameControllerOpen(joystick);

            if (controller)
                fprintf(stderr, "Host controller: %s (forwarded to gpSP)\n",
                        SDL_GameControllerName(controller));
            return controller;
        }
    }

    return NULL;
}

static uint16_t gpsp_host_read_input(SDL_GameController *controller)
{
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    uint16_t input = 0;

    /* Keyboard and controller states are merged into one RetroPad mask. */
    if (keys[SDL_SCANCODE_Z])
        input |= GPSP_ESPEMU_INPUT_B;
    if (keys[SDL_SCANCODE_BACKSPACE])
        input |= GPSP_ESPEMU_INPUT_SELECT;
    if (keys[SDL_SCANCODE_RETURN])
        input |= GPSP_ESPEMU_INPUT_START;
    if (keys[SDL_SCANCODE_UP])
        input |= GPSP_ESPEMU_INPUT_UP;
    if (keys[SDL_SCANCODE_DOWN])
        input |= GPSP_ESPEMU_INPUT_DOWN;
    if (keys[SDL_SCANCODE_LEFT])
        input |= GPSP_ESPEMU_INPUT_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])
        input |= GPSP_ESPEMU_INPUT_RIGHT;
    if (keys[SDL_SCANCODE_X])
        input |= GPSP_ESPEMU_INPUT_A;
    if (keys[SDL_SCANCODE_A])
        input |= GPSP_ESPEMU_INPUT_L;
    if (keys[SDL_SCANCODE_S])
        input |= GPSP_ESPEMU_INPUT_R;
    if (keys[SDL_SCANCODE_V])
        input |= GPSP_ESPEMU_INPUT_X;
    if (keys[SDL_SCANCODE_C])
        input |= GPSP_ESPEMU_INPUT_Y;
    if (keys[SDL_SCANCODE_D])
        input |= GPSP_ESPEMU_INPUT_L2;
    if (keys[SDL_SCANCODE_F])
        input |= GPSP_ESPEMU_INPUT_R2;

    if (!controller || !SDL_GameControllerGetAttached(controller))
        return input;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B))
        input |= GPSP_ESPEMU_INPUT_A;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK))
        input |= GPSP_ESPEMU_INPUT_SELECT;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START))
        input |= GPSP_ESPEMU_INPUT_START;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP))
        input |= GPSP_ESPEMU_INPUT_UP;
    if (SDL_GameControllerGetButton(controller,
            SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        input |= GPSP_ESPEMU_INPUT_DOWN;
    if (SDL_GameControllerGetButton(controller,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        input |= GPSP_ESPEMU_INPUT_LEFT;
    if (SDL_GameControllerGetButton(controller,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        input |= GPSP_ESPEMU_INPUT_RIGHT;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A))
        input |= GPSP_ESPEMU_INPUT_B;
    if (SDL_GameControllerGetButton(controller,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        input |= GPSP_ESPEMU_INPUT_L;
    if (SDL_GameControllerGetButton(controller,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        input |= GPSP_ESPEMU_INPUT_R;

    return input;
}

static void gpsp_host_poll_input(struct gpsp_host_state *state,
        SDL_GameController *controller, bool *home_held)
{
    bool home = controller && SDL_GameControllerGetAttached(controller) &&
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE);

    /* The wire button mask is 16 bits; Home uses the command field. */
    if (home && !*home_held && SDL_AtomicGet(&state->connected))
        gpsp_host_command(state, GPSP_ESPEMU_COMMAND_HOME);
    *home_held = home;
    SDL_AtomicSet(&state->input_bits, gpsp_host_read_input(controller));
}

static const uint8_t *gpsp_host_fps_glyph(char character)
{
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14},
        {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2},
        {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14},
        {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 1, 14},
    };
    static const uint8_t f[7] = {31, 16, 16, 30, 16, 16, 16};
    static const uint8_t p[7] = {30, 17, 17, 30, 16, 16, 16};
    static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
    static const uint8_t point[7] = {0, 0, 0, 0, 0, 12, 12};
    static const uint8_t minus[7] = {0, 0, 0, 31, 0, 0, 0};
    static const uint8_t blank[7] = {0};

    if (character >= '0' && character <= '9')
        return digits[character - '0'];

    switch (character)
    {
        case 'F': return f;
        case 'P': return p;
        case 'S': return s;
        case '.': return point;
        case '-': return minus;
        default: return blank;
    }
}

static void gpsp_host_draw_emulation_fps(SDL_Renderer *renderer, double fps)
{
    char label[32];
    size_t length;
    int left;
    SDL_Rect background;

    snprintf(label, sizeof(label), "%.1f FPS", fps);
    length = strlen(label);
    left = GPSP_ESPEMU_SCREEN_WIDTH - (int)length * 6 - 4;
    background = (SDL_Rect){left, 2, (int)length * 6 + 3, 11};

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
    SDL_RenderFillRect(renderer, &background);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    for (size_t index = 0; index < length; index++)
    {
        const uint8_t *glyph = gpsp_host_fps_glyph(label[index]);

        for (unsigned row = 0; row < 7; row++)
        {
            for (unsigned column = 0; column < 5; column++)
            {
                if (glyph[row] & (1u << (4 - column)))
                    SDL_RenderDrawPoint(renderer,
                            left + 2 + (int)index * 6 + (int)column,
                            4 + (int)row);
            }
        }
    }
}

/* Serial mode controls: volume down, volume up, home, close the game. */
enum gpsp_host_button
{
    GPSP_HOST_BUTTON_NONE = -1,
    GPSP_HOST_BUTTON_VOLUME_DOWN,
    GPSP_HOST_BUTTON_VOLUME_UP,
    GPSP_HOST_BUTTON_HOME,
    GPSP_HOST_BUTTON_QUIT,
    GPSP_HOST_BUTTON_COUNT,
};


static SDL_Rect gpsp_host_button_rect(enum gpsp_host_button button)
{
    return (SDL_Rect){15 + (int)button * 55, 40, 45, 30};
}

static enum gpsp_host_button gpsp_host_button_at(int x, int y)
{
    SDL_Point point = {x, y};

    for (int button = 0; button < GPSP_HOST_BUTTON_COUNT; button++)
    {
        SDL_Rect rect = gpsp_host_button_rect((enum gpsp_host_button)button);
        if (SDL_PointInRect(&point, &rect))
            return (enum gpsp_host_button)button;
    }
    return GPSP_HOST_BUTTON_NONE;
}

static void gpsp_host_draw_text(SDL_Renderer *renderer, const char *text,
        int left, int top)
{
    for (size_t index = 0; text[index]; index++)
    {
        const uint8_t *glyph = gpsp_host_fps_glyph(text[index]);

        for (unsigned row = 0; row < 7; row++)
            for (unsigned column = 0; column < 5; column++)
                if (glyph[row] & (1u << (4 - column)))
                    SDL_RenderDrawPoint(renderer, left + (int)index * 6 +
                            (int)column, top + (int)row);
    }
}

static void gpsp_host_draw_controls(SDL_Renderer *renderer, int volume_db,
        bool closed)
{
    for (int button = 0; button < GPSP_HOST_BUTTON_COUNT; button++)
    {
        SDL_Rect rect = gpsp_host_button_rect((enum gpsp_host_button)button);
        int cx = rect.x + rect.w / 2;
        int cy = rect.y + rect.h / 2;

        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderDrawRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        if (button == GPSP_HOST_BUTTON_QUIT)
        {
            /* A power sign: a broken circle and a bar. */
            for (int angle = 50; angle <= 310; angle += 5)
            {
                double radians = (angle - 90) * 3.14159265 / 180.0;
                SDL_RenderDrawPoint(renderer, cx + (int)(8 * SDL_cos(radians)),
                        cy + (int)(8 * SDL_sin(radians)));
            }
            SDL_RenderDrawLine(renderer, cx, cy - 10, cx, cy - 2);
        }
        else if (button == GPSP_HOST_BUTTON_HOME)
        {
            /* A house: a roof over a box. */
            SDL_RenderDrawLine(renderer, cx - 9, cy - 1, cx, cy - 9);
            SDL_RenderDrawLine(renderer, cx, cy - 9, cx + 9, cy - 1);
            SDL_Rect body = {cx - 6, cy - 2, 13, 10};
            SDL_RenderDrawRect(renderer, &body);
        }
        else
        {
            SDL_RenderDrawLine(renderer, cx - 8, cy, cx + 8, cy);
            if (button == GPSP_HOST_BUTTON_VOLUME_UP)
                SDL_RenderDrawLine(renderer, cx, cy - 8, cx, cy + 8);
        }
    }

    /* Volume in dB between the volume buttons, once the board reports it. */
    if (volume_db != GPSP_HOST_VOLUME_UNKNOWN)
    {
        char label[16];
        SDL_Rect down = gpsp_host_button_rect(GPSP_HOST_BUTTON_VOLUME_DOWN);
        SDL_Rect up = gpsp_host_button_rect(GPSP_HOST_BUTTON_VOLUME_UP);
        int center = (down.x + down.w + up.x) / 2;

        snprintf(label, sizeof(label), "%d", volume_db);
        gpsp_host_draw_text(renderer, label,
                center - (int)strlen(label) * 3, 26);
    }
    if (closed)
    {
        SDL_SetRenderDrawColor(renderer, 200, 60, 60, 255);
        SDL_Rect rect = gpsp_host_button_rect(GPSP_HOST_BUTTON_QUIT);
        SDL_RenderDrawRect(renderer, &rect);
    }
}

static uint16_t gpsp_host_button_command(enum gpsp_host_button button)
{
    switch (button)
    {
        case GPSP_HOST_BUTTON_VOLUME_DOWN:
            return GPSP_ESPEMU_COMMAND_VOLUME_DOWN;
        case GPSP_HOST_BUTTON_VOLUME_UP:
            return GPSP_ESPEMU_COMMAND_VOLUME_UP;
        case GPSP_HOST_BUTTON_HOME:
            return GPSP_ESPEMU_COMMAND_HOME;
        case GPSP_HOST_BUTTON_QUIT:
            return GPSP_ESPEMU_COMMAND_QUIT;
        default:
            return GPSP_ESPEMU_COMMAND_NONE;
    }
}

static uint32_t gpsp_host_profile_value(
        const struct gpsp_host_profile_sample *sample, unsigned lane)
{
    switch (lane)
    {
        case 0: return sample->run_avg_us;
        case 1: return sample->wait_avg_us;
        case 2: return sample->copy_avg_us;
        default: return sample->send_avg_us;
    }
}

static void gpsp_host_draw_profiles(
        SDL_Renderer *renderer,
        const struct gpsp_host_profile_sample *profiles,
        unsigned count, uint32_t target_frame_us)
{
    static const uint8_t colors[4][3] = {
        {255, 100, 100}, {100, 170, 255},
        {255, 205, 70}, {100, 230, 145},
    };
    static const uint8_t labels[4][7] = {
        {30, 17, 17, 30, 20, 18, 17}, /* R: run */
        {17, 17, 17, 21, 21, 21, 10}, /* W: wait */
        {15, 16, 16, 16, 16, 16, 15}, /* C: copy */
        {15, 16, 16, 14, 1, 1, 30},   /* S: send */
    };
    unsigned lane;

    SDL_SetRenderDrawColor(renderer, 19, 23, 32, 255);
    SDL_Rect panel = {
        0, GPSP_ESPEMU_SCREEN_HEIGHT,
        GPSP_ESPEMU_SCREEN_WIDTH, GPSP_HOST_PROFILE_HEIGHT,
    };
    SDL_RenderFillRect(renderer, &panel);

    for (lane = 0; lane < 4; lane++)
    {
        int top = GPSP_ESPEMU_SCREEN_HEIGHT + (int)lane * 20;
        int baseline = top + 17;
        uint32_t scale = lane < 2 ? 25000u : 10000u;
        unsigned index;
        int last_x = -1;
        int last_y = baseline;

        for (index = 0; index < count; index++)
        {
            uint32_t value = gpsp_host_profile_value(&profiles[index], lane);

            if (value > scale)
                scale = value;
        }

        scale += scale / 10;
        SDL_SetRenderDrawColor(renderer, 45, 51, 63, 255);
        SDL_RenderDrawLine(renderer, 0, baseline,
                GPSP_ESPEMU_SCREEN_WIDTH - 1, baseline);

        if (lane == 0 && target_frame_us > 0 &&
                target_frame_us <= scale)
        {
            int target_y = baseline -
                    (int)((uint64_t)target_frame_us * 14u / scale);

            SDL_SetRenderDrawColor(renderer, 105, 110, 120, 255);

            for (index = 0; index < GPSP_ESPEMU_SCREEN_WIDTH; index += 6)
                SDL_RenderDrawLine(renderer, (int)index, target_y,
                        (int)index + 2, target_y);
        }

        SDL_SetRenderDrawColor(renderer,
                colors[lane][0], colors[lane][1], colors[lane][2], 255);

        for (unsigned row = 0; row < 7; row++)
        {
            for (unsigned column = 0; column < 5; column++)
            {
                if (labels[lane][row] & (1u << (4 - column)))
                    SDL_RenderDrawPoint(renderer,
                            2 + (int)column, top + 4 + (int)row);
            }
        }

        for (index = 0; index < count; index++)
        {
            uint32_t value = gpsp_host_profile_value(&profiles[index], lane);
            int x = GPSP_ESPEMU_SCREEN_WIDTH - 1 -
                    (int)(count - 1 - index) * 2;
            int y = baseline - (int)((uint64_t)value * 14u / scale);

            if (last_x >= 0)
                SDL_RenderDrawLine(renderer, last_x, last_y, x, y);
            else
                SDL_RenderDrawPoint(renderer, x, y);

            last_x = x;
            last_y = y;
        }
    }
}

static void gpsp_host_print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s [bind-address] [port] [profile.csv]\n"
            "       %s --serial PORT [--rom NAME] [--reset]\n",
            program, program);
}

int main(int argc, char **argv)
{
    struct gpsp_host_state state = {
        .bind_address = "127.0.0.1",
        .port = 24680,
    };
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_GameController *controller = NULL;
    bool home_held = false;
    SDL_Thread *network_thread = NULL;
    uint32_t displayed_sequence = UINT32_MAX;
    uint32_t displayed_connection = 0;
    SDL_Rect source_rect = {
        0, 0, GPSP_ESPEMU_SCREEN_WIDTH, GPSP_ESPEMU_SCREEN_HEIGHT,
    };
    int exit_code = 1;

    if (argc >= 3 && !strcmp(argv[1], "--serial"))
    {
        state.serial_path = argv[2];
        for (int i = 3; i < argc; i++)
        {
            if (!strcmp(argv[i], "--rom") && i + 1 < argc)
                state.rom_name = argv[++i];
            else if (!strcmp(argv[i], "--reset"))
                state.serial_reset = true;
            else
            {
                gpsp_host_print_usage(argv[0]);
                return 1;
            }
        }
        argc = 1;
    }

    if (argc > 4)
    {
        gpsp_host_print_usage(argv[0]);
        return 1;
    }

    if (argc >= 2)
        state.bind_address = argv[1];

    if (argc >= 3)
    {
        char *end = NULL;
        unsigned long port = strtoul(argv[2], &end, 10);

        if (!end || *end != '\0' || port < 1024 || port > 65535)
        {
            gpsp_host_print_usage(argv[0]);
            return 1;
        }

        state.port = (uint16_t)port;
    }

    if (argc == 4)
    {
        state.profile_csv = fopen(argv[3], "w");

        if (!state.profile_csv)
        {
            perror(argv[3]);
            return 1;
        }

        fprintf(state.profile_csv,
                "connection,host_unix_s,guest_us,interval_us,frames,"
                "run_avg_us,run_max_us,wait_avg_us,wait_max_us,"
                "copy_count,copy_avg_us,copy_max_us,send_count,"
                "send_avg_us,send_max_us,submitted,sent,dropped,"
                "received,target_frame_us,wall_fps\n");
        fflush(state.profile_csv);
        fprintf(stderr, "Writing profile CSV: %s\n", argv[3]);
    }

    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        if (state.profile_csv)
            fclose(state.profile_csv);
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    state.frame_mutex = SDL_CreateMutex();
    state.frame_pixels = malloc(GPSP_ESPEMU_UI_FRAME_BYTES);

    if (!state.frame_mutex || !state.frame_pixels)
    {
        fprintf(stderr, "Failed to allocate the frame buffer\n");
        goto cleanup;
    }

    memset(state.frame_pixels, 0, GPSP_ESPEMU_UI_FRAME_BYTES);
    state.frame_width = GPSP_ESPEMU_SCREEN_WIDTH;
    state.frame_height = GPSP_ESPEMU_SCREEN_HEIGHT;
    gpsp_command_queue_init(&state.commands);
    SDL_AtomicSet(&state.running, 1);
    SDL_AtomicSet(&state.volume_db, GPSP_HOST_VOLUME_UNKNOWN);

    /* Serial mode only needs a small window to own keyboard focus. */
    window = SDL_CreateWindow("gpSP Viewer",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            GPSP_ESPEMU_SCREEN_WIDTH * (state.serial_path ? 1 : 3),
            (state.serial_path ? GPSP_ESPEMU_SCREEN_HEIGHT / 2 :
                    (GPSP_ESPEMU_SCREEN_HEIGHT + GPSP_HOST_PROFILE_HEIGHT) * 3),
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window)
    {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    if (!renderer)
    {
        fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    /* Serial mode draws only the controls, at the window's own size. */
    SDL_RenderSetLogicalSize(renderer,
            GPSP_ESPEMU_SCREEN_WIDTH,
            state.serial_path ? GPSP_ESPEMU_SCREEN_HEIGHT / 2 :
                    GPSP_ESPEMU_SCREEN_HEIGHT + GPSP_HOST_PROFILE_HEIGHT);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
            SDL_TEXTUREACCESS_STREAMING,
            GPSP_ESPEMU_UI_WIDTH, GPSP_ESPEMU_UI_HEIGHT);

    if (!texture)
    {
        fprintf(stderr, "Texture creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    controller = gpsp_host_open_controller();
    network_thread = state.serial_path ?
            SDL_CreateThread(gpsp_host_serial_thread, "espemu-serial", &state) :
            SDL_CreateThread(gpsp_host_network_thread, "espemu-network", &state);

    if (!network_thread)
    {
        fprintf(stderr, "Network thread creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    while (SDL_AtomicGet(&state.running))
    {
        SDL_Event event;
        struct gpsp_host_profile_sample profiles[GPSP_HOST_PROFILE_HISTORY];
        struct gpsp_host_profile_sample latest = {0};
        unsigned profile_count;
        uint32_t target_frame_us;
        double emulation_fps;
        char title[256];

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN &&
                            event.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
            {
                SDL_AtomicSet(&state.running, 0);
            }
            else if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                    SDL_AtomicGet(&state.connected) &&
                    (event.key.keysym.scancode == SDL_SCANCODE_F5 ||
                            event.key.keysym.scancode == SDL_SCANCODE_F8))
            {
                gpsp_host_command(&state,
                        event.key.keysym.scancode == SDL_SCANCODE_F5 ?
                        GPSP_ESPEMU_COMMAND_SAVE_STATE :
                        GPSP_ESPEMU_COMMAND_LOAD_STATE);
            }
            else if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                    state.serial_path && SDL_AtomicGet(&state.connected) &&
                    event.key.keysym.scancode == SDL_SCANCODE_F2)
            {
                /* The board prints its task table to the log. */
                gpsp_host_command(&state, GPSP_ESPEMU_COMMAND_TASKS);
            }
            else if (event.type == SDL_KEYDOWN && state.serial_path &&
                    SDL_AtomicGet(&state.connected))
            {
                enum gpsp_host_button button = GPSP_HOST_BUTTON_NONE;
                SDL_Scancode key = event.key.keysym.scancode;

                if (key == SDL_SCANCODE_MINUS || key == SDL_SCANCODE_KP_MINUS)
                    button = GPSP_HOST_BUTTON_VOLUME_DOWN;
                else if (key == SDL_SCANCODE_EQUALS ||
                        key == SDL_SCANCODE_KP_PLUS)
                    button = GPSP_HOST_BUTTON_VOLUME_UP;
                else if (key == SDL_SCANCODE_Q && !event.key.repeat)
                    button = GPSP_HOST_BUTTON_HOME;
                else if (key == SDL_SCANCODE_F10 && !event.key.repeat)
                    button = GPSP_HOST_BUTTON_QUIT;
                if (button != GPSP_HOST_BUTTON_NONE)
                    gpsp_host_command(&state,
                            gpsp_host_button_command(button));
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN && state.serial_path &&
                    event.button.button == SDL_BUTTON_LEFT &&
                    SDL_AtomicGet(&state.connected))
            {
                enum gpsp_host_button button =
                        gpsp_host_button_at(event.button.x, event.button.y);

                if (button != GPSP_HOST_BUTTON_NONE)
                    gpsp_host_command(&state,
                            gpsp_host_button_command(button));
            }
            else if (event.type == SDL_CONTROLLERDEVICEADDED && !controller)
            {
                controller = gpsp_host_open_controller();
            }
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                    event.cdevice.which ==
                            SDL_JoystickInstanceID(
                                    SDL_GameControllerGetJoystick(controller)))
            {
                SDL_GameControllerClose(controller);
                controller = gpsp_host_open_controller();
                home_held = false;
            }
        }

        gpsp_host_poll_input(&state, controller, &home_held);

        SDL_LockMutex(state.frame_mutex);

        if (displayed_connection != state.connection_index)
        {
            displayed_connection = state.connection_index;
            displayed_sequence = UINT32_MAX;
        }

        if (state.frame_ready && state.frame_sequence != displayed_sequence)
        {
            source_rect.w = state.frame_width;
            source_rect.h = state.frame_height;
            SDL_UpdateTexture(texture, &source_rect, state.frame_pixels,
                    state.frame_width * 2);
            displayed_sequence = state.frame_sequence;
        }

        profile_count = state.profile_count;
        target_frame_us = state.target_frame_us;

        for (unsigned index = 0; index < profile_count; index++)
        {
            unsigned source = (state.profile_next + GPSP_HOST_PROFILE_HISTORY -
                    profile_count + index) % GPSP_HOST_PROFILE_HISTORY;
            profiles[index] = state.profiles[source];
        }

        if (profile_count > 0)
            latest = profiles[profile_count - 1];

        SDL_UnlockMutex(state.frame_mutex);
        emulation_fps = latest.interval_us == 0 ? 0.0 :
                (double)latest.frames * 1000000.0 /
                        (double)latest.interval_us;

        if (state.serial_path)
        {
            snprintf(title, sizeof(title), "gpSP Serial | %s | input 0x%03x",
                    SDL_AtomicGet(&state.connected) ? "connected" : "closed",
                    (unsigned)SDL_AtomicGet(&state.input_bits));
            int volume_db = SDL_AtomicGet(&state.volume_db);
            size_t used = strlen(title);
            if (volume_db != GPSP_HOST_VOLUME_UNKNOWN)
                snprintf(title + used, sizeof(title) - used, " | vol %d dB",
                        volume_db);
            used = strlen(title);
            if (SDL_AtomicGet(&state.game_closed))
                snprintf(title + used, sizeof(title) - used, " | game closed");
        }
        else if (!SDL_AtomicGet(&state.connected))
            snprintf(title, sizeof(title), "gpSP Viewer - waiting");
        else if (profile_count == 0)
            snprintf(title, sizeof(title), "gpSP Viewer - measuring");
        else
            snprintf(title, sizeof(title),
                    "gpSP Viewer | %.1f FPS", emulation_fps);

        int state_status = SDL_AtomicGet(&state.state_status);
        if (state_status)
        {
            const char *action = (state_status >> 16) ==
                    GPSP_ESPEMU_COMMAND_SAVE_STATE ? "save" : "load";
            const char *outcome = (state_status & 0xffff) == 0 ?
                    "complete" : "failed";
            size_t used = strlen(title);
            snprintf(title + used, sizeof(title) - used,
                    " | %s %s", action, outcome);
        }

        SDL_SetWindowTitle(window, title);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (state.serial_path)
        {
            gpsp_host_draw_controls(renderer, SDL_AtomicGet(&state.volume_db),
                    SDL_AtomicGet(&state.game_closed));
            SDL_RenderPresent(renderer);
            SDL_Delay(10);
            continue;
        }
        SDL_Rect game_rect = {
            0, 0, GPSP_ESPEMU_SCREEN_WIDTH, GPSP_ESPEMU_SCREEN_HEIGHT,
        };
        /* The 4:3 menu screen fits the game area's height. */
        if (source_rect.w == GPSP_ESPEMU_UI_WIDTH)
        {
            game_rect.w = GPSP_ESPEMU_SCREEN_HEIGHT * 4 / 3;
            game_rect.x = (GPSP_ESPEMU_SCREEN_WIDTH - game_rect.w) / 2;
        }
        SDL_RenderCopy(renderer, texture, &source_rect, &game_rect);
        gpsp_host_draw_emulation_fps(renderer, emulation_fps);
        gpsp_host_draw_profiles(renderer, profiles,
                profile_count, target_frame_us);
        SDL_RenderPresent(renderer);
    }

    exit_code = 0;

cleanup:
    SDL_AtomicSet(&state.running, 0);

    if (network_thread)
        SDL_WaitThread(network_thread, NULL);

    if (controller)
        SDL_GameControllerClose(controller);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    if (state.frame_mutex)
        SDL_DestroyMutex(state.frame_mutex);

    free(state.frame_pixels);
    if (state.profile_csv)
        fclose(state.profile_csv);
    SDL_Quit();
    return exit_code;
}

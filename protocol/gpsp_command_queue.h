/* One-shot command queue for the firmware and host viewer.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_COMMAND_QUEUE_H
#define GPSP_COMMAND_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define GPSP_COMMAND_QUEUE_CAPACITY 16u

/* Nonzero commands, one producer and one consumer; initialize first. */
struct gpsp_command_queue
{
    atomic_uint read;
    atomic_uint write;
    uint16_t commands[GPSP_COMMAND_QUEUE_CAPACITY];
};

static inline void gpsp_command_queue_init(struct gpsp_command_queue *queue)
{
    atomic_init(&queue->read, 0);
    atomic_init(&queue->write, 0);
}

static inline bool gpsp_command_queue_push(
        struct gpsp_command_queue *queue, uint16_t command)
{
    unsigned write = atomic_load_explicit(&queue->write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&queue->read, memory_order_acquire);

    if (write - read >= GPSP_COMMAND_QUEUE_CAPACITY)
        return false;
    queue->commands[write % GPSP_COMMAND_QUEUE_CAPACITY] = command;
    atomic_store_explicit(&queue->write, write + 1, memory_order_release);
    return true;
}

static inline uint16_t gpsp_command_queue_pop(struct gpsp_command_queue *queue)
{
    unsigned read = atomic_load_explicit(&queue->read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&queue->write, memory_order_acquire);
    uint16_t command;

    if (read == write)
        return 0;
    command = queue->commands[read % GPSP_COMMAND_QUEUE_CAPACITY];
    atomic_store_explicit(&queue->read, read + 1, memory_order_release);
    return command;
}

#endif

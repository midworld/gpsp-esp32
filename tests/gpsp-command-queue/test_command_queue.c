/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "espemu_protocol.h"
#include "gpsp_command_queue.h"

#define COMMAND_COUNT 100000u

static struct gpsp_command_queue queue;

static uint16_t command_at(unsigned index)
{
    return index % 2 ? GPSP_ESPEMU_COMMAND_LOAD_STATE :
            GPSP_ESPEMU_COMMAND_SAVE_STATE;
}

static void *produce(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < COMMAND_COUNT; i++)
        while (!gpsp_command_queue_push(&queue, command_at(i)))
            sched_yield();
    return NULL;
}

static void *consume(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < COMMAND_COUNT; i++)
    {
        uint16_t command;

        while (!(command = gpsp_command_queue_pop(&queue)))
            sched_yield();
        assert(command == command_at(i));
    }
    return NULL;
}

int main(void)
{
    pthread_t producer, consumer;

    gpsp_command_queue_init(&queue);
    assert(gpsp_command_queue_pop(&queue) == 0);
    for (unsigned batch = 0; batch < 100; batch++)
    {
        for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
            assert(gpsp_command_queue_push(&queue, command_at(i)));
        assert(!gpsp_command_queue_push(&queue, GPSP_ESPEMU_COMMAND_QUIT));
        for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
            assert(gpsp_command_queue_pop(&queue) == command_at(i));
        assert(gpsp_command_queue_pop(&queue) == 0);
    }
    puts("PASS: bursts retain order and overflow preserves pending commands");

    atomic_store(&queue.read, UINT_MAX - 7);
    atomic_store(&queue.write, UINT_MAX - 7);
    for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
        assert(gpsp_command_queue_push(&queue, command_at(i)));
    assert(!gpsp_command_queue_push(&queue, GPSP_ESPEMU_COMMAND_QUIT));
    for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
        assert(gpsp_command_queue_pop(&queue) == command_at(i));
    puts("PASS: counter wrap retains order and capacity");

    gpsp_command_queue_init(&queue);
    assert(!pthread_create(&producer, NULL, produce, NULL));
    assert(!pthread_create(&consumer, NULL, consume, NULL));
    assert(!pthread_join(producer, NULL));
    assert(!pthread_join(consumer, NULL));
    assert(gpsp_command_queue_pop(&queue) == 0);
    puts("PASS: concurrent producer and consumer retain 100000 commands");
    return 0;
}

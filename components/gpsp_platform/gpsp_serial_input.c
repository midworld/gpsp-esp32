/* Button input from the host viewer over the console UART or
 * USB Serial/JTAG.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_serial_input.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_SERIAL_INPUT

#include <stdatomic.h>
#include <string.h>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espemu_protocol.h"
#include "gpsp_command_queue.h"
#if CONFIG_GPSP_AUDIO
#include "gpsp_audio.h"
#endif
#if CONFIG_GPSP_STATUS
#include "gpsp_status.h"
#endif

#define GPSP_SERIAL_MESSAGE_SIZE (GPSP_ESPEMU_HEADER_SIZE + GPSP_ESPEMU_INPUT_SIZE)
#define GPSP_SERIAL_INPUT_TIMEOUT_US 500000
/* A home command reads as a press this long. */
#define GPSP_SERIAL_HOME_US 100000
#define GPSP_SERIAL_HOME_BIT (1u << 16)

/* CPU1 when it exists: CPU0 runs the emulator. */
#if CONFIG_FREERTOS_UNICORE
#define GPSP_SERIAL_TASK_CORE tskNO_AFFINITY
#else
#define GPSP_SERIAL_TASK_CORE 1
#endif

static const char *TAG = "gpsp_serial";
static atomic_uint_fast32_t s_input_bits;
/* Receive bursts wait here; child screens use a separate UI queue. */
static struct gpsp_command_queue s_commands;
static atomic_bool s_quit_pending;
static _Atomic int64_t s_home_until_us;
static atomic_bool s_running;
static TaskHandle_t s_task;

/* Applies one complete input message; false when the header is not one. */
static bool gpsp_serial_apply(const uint8_t *message)
{
    if (gpsp_espemu_read_u32(message) != GPSP_ESPEMU_PROTOCOL_MAGIC ||
            gpsp_espemu_read_u16(message + 4) != GPSP_ESPEMU_PROTOCOL_VERSION ||
            gpsp_espemu_read_u16(message + 6) != GPSP_ESPEMU_MESSAGE_INPUT ||
            gpsp_espemu_read_u32(message + 8) != GPSP_ESPEMU_INPUT_SIZE)
        return false;

    uint16_t input = gpsp_espemu_read_u16(message + GPSP_ESPEMU_HEADER_SIZE);
    uint16_t command =
            gpsp_espemu_read_u16(message + GPSP_ESPEMU_HEADER_SIZE + 2);

    atomic_store_explicit(&s_input_bits, input, memory_order_relaxed);
#if CONFIG_GPSP_STATUS
    if (command == GPSP_ESPEMU_COMMAND_TASKS)
        gpsp_status_print_tasks();
#endif
#if CONFIG_GPSP_AUDIO
    /* The viewer reads the line to show the volume. */
    if (command == GPSP_ESPEMU_COMMAND_VOLUME_UP ||
            command == GPSP_ESPEMU_COMMAND_VOLUME_DOWN)
    {
        int db = gpsp_audio_volume_step(
                command == GPSP_ESPEMU_COMMAND_VOLUME_UP ? 1 : -1);
        ESP_LOGI(TAG, "SERIAL_VOLUME db=%d", db);
    }
#endif
    if (command == GPSP_ESPEMU_COMMAND_HOME)
        atomic_store_explicit(&s_home_until_us,
                esp_timer_get_time() + GPSP_SERIAL_HOME_US,
                memory_order_relaxed);
    if (command == GPSP_ESPEMU_COMMAND_QUIT)
        atomic_store_explicit(&s_quit_pending, true, memory_order_release);
    else if (command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
            command == GPSP_ESPEMU_COMMAND_LOAD_STATE ||
            command == GPSP_ESPEMU_COMMAND_SCREENSHOT)
    {
        if (!gpsp_command_queue_push(&s_commands, command))
        {
            if (command != GPSP_ESPEMU_COMMAND_SCREENSHOT)
                gpsp_serial_input_report_command(command, false);
            else
                ESP_LOGW(TAG, "Screenshot command queue full");
        }
    }
    return true;
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define GPSP_SERIAL_PORT_NAME "USB Serial/JTAG"

/* Buffered output uses the SDK's bounded wait when the host stops reading. */
static bool gpsp_serial_open(void)
{
    usb_serial_jtag_driver_config_t config =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

    config.rx_buffer_size = 512;
    config.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&config) != ESP_OK)
        return false;
    usb_serial_jtag_vfs_use_driver();
    return true;
}

static void gpsp_serial_close(void)
{
    usb_serial_jtag_vfs_use_nonblocking();
    usb_serial_jtag_driver_uninstall();
}

static int gpsp_serial_read(uint8_t *data, size_t size, TickType_t wait)
{
    return usb_serial_jtag_read_bytes(data, size, wait);
}
#else
#define GPSP_SERIAL_STR2(x) #x
#define GPSP_SERIAL_STR(x) GPSP_SERIAL_STR2(x)
#define GPSP_SERIAL_PORT_NAME "UART" GPSP_SERIAL_STR(CONFIG_ESP_CONSOLE_UART_NUM)

static bool gpsp_serial_open(void)
{
    return uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL,
            0) == ESP_OK;
}

static void gpsp_serial_close(void)
{
    uart_driver_delete(CONFIG_ESP_CONSOLE_UART_NUM);
}

static int gpsp_serial_read(uint8_t *data, size_t size, TickType_t wait)
{
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, size, wait);
}
#endif

static void gpsp_serial_task(void *arg)
{
    uint8_t message[GPSP_SERIAL_MESSAGE_SIZE];
    size_t length = 0;
    int64_t last_input_us = 0;

    (void)arg;

    while (atomic_load_explicit(&s_running, memory_order_relaxed))
    {
        int count = gpsp_serial_read(message + length,
                GPSP_SERIAL_MESSAGE_SIZE - length, pdMS_TO_TICKS(50));

        if (count > 0)
            length += (size_t)count;

        /* Keep the bytes from the first possible magic; drop the rest. */
        while (length >= 4 && gpsp_espemu_read_u32(message) !=
                GPSP_ESPEMU_PROTOCOL_MAGIC)
        {
            memmove(message, message + 1, --length);
        }

        if (length == GPSP_SERIAL_MESSAGE_SIZE)
        {
            if (gpsp_serial_apply(message))
            {
                last_input_us = esp_timer_get_time();
                length = 0;
            }
            else
                memmove(message, message + 1, --length);
        }

        /* A closed viewer or unplugged cable releases held buttons. */
        if (last_input_us &&
                esp_timer_get_time() - last_input_us > GPSP_SERIAL_INPUT_TIMEOUT_US)
        {
            atomic_store_explicit(&s_input_bits, 0, memory_order_relaxed);
            last_input_us = 0;
        }
    }

    gpsp_serial_close();
    s_task = NULL;
    vTaskDelete(NULL);
}

void gpsp_serial_input_start(void)
{
    if (s_task)
        return;
    if (!gpsp_serial_open())
    {
        ESP_LOGW(TAG, "Console driver unavailable; serial input off");
        return;
    }

    gpsp_command_queue_init(&s_commands);
    atomic_store(&s_input_bits, 0);
    atomic_store(&s_home_until_us, 0);
    atomic_store(&s_quit_pending, false);
    atomic_store(&s_running, true);
    if (xTaskCreatePinnedToCore(gpsp_serial_task, "gpsp_serial", 3072, NULL,
            tskIDLE_PRIORITY + 4, &s_task, GPSP_SERIAL_TASK_CORE) != pdPASS)
    {
        atomic_store(&s_running, false);
        gpsp_serial_close();
        ESP_LOGW(TAG, "Serial input task failed");
        return;
    }
    ESP_LOGI(TAG, "Serial input ready on " GPSP_SERIAL_PORT_NAME);
}

uint32_t gpsp_serial_input_read(void)
{
    uint32_t input = (uint16_t)atomic_load_explicit(&s_input_bits,
            memory_order_relaxed);

    if (esp_timer_get_time() < atomic_load_explicit(&s_home_until_us,
            memory_order_relaxed))
        input |= GPSP_SERIAL_HOME_BIT;
    return input;
}

uint16_t gpsp_serial_input_read_command(void)
{
    if (atomic_exchange_explicit(&s_quit_pending, false, memory_order_acq_rel))
    {
        for (unsigned i = 0; i < GPSP_COMMAND_QUEUE_CAPACITY; i++)
        {
            uint16_t command = gpsp_command_queue_pop(&s_commands);

            if (!command)
                break;
            if (command == GPSP_ESPEMU_COMMAND_SAVE_STATE ||
                    command == GPSP_ESPEMU_COMMAND_LOAD_STATE)
                gpsp_serial_input_report_command(command, false);
        }
        return GPSP_ESPEMU_COMMAND_QUIT;
    }
    return gpsp_command_queue_pop(&s_commands);
}

/* The viewer reads this line from the log to show the result. */
void gpsp_serial_input_report_command(uint16_t command, bool success)
{
    if (s_task)
        ESP_LOGI(TAG, "SERIAL_STATE command=%u ok=%u", command, success);
}

void gpsp_serial_input_stop(void)
{
    if (!s_task)
        return;
    atomic_store(&s_running, false);
    while (s_task)
        vTaskDelay(1);
    atomic_store(&s_input_bits, 0);
    atomic_store(&s_home_until_us, 0);
}

#endif

/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "test_sdk.h"
#include "../../components/gpsp_platform/gpsp_serial_input.c"
#include "storage_console.inc"

static bool s_installed, s_vfs_driver, s_fail_install, s_fail_task;
static unsigned s_installs, s_closes, s_flushes;
static unsigned s_rx_size, s_tx_size;
static const char *s_stream;
static int64_t s_now;
static void (*s_worker)(void *);

void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

int64_t esp_timer_get_time(void) { return s_now += 1000; }

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    assert(!s_installed);
    if (s_fail_install)
        return ESP_FAIL;
    s_installs++;
    s_installed = true;
    s_rx_size = config->rx_buffer_size;
    s_tx_size = config->tx_buffer_size;
    return ESP_OK;
}

esp_err_t usb_serial_jtag_driver_uninstall(void)
{
    assert(s_installed && !s_vfs_driver);
    s_installed = false;
    s_closes++;
    return ESP_OK;
}

void usb_serial_jtag_vfs_use_driver(void)
{
    assert(s_installed);
    s_vfs_driver = true;
}

void usb_serial_jtag_vfs_use_nonblocking(void)
{
    assert(s_installed);
    s_vfs_driver = false;
}

int usb_serial_jtag_read_bytes(void *data, size_t size, TickType_t wait)
{
    (void)wait;
    assert(s_installed);
    size_t count = s_stream ? strlen(s_stream) : 0;
    if (count > size)
        count = size;
    if (count)
    {
        memcpy(data, s_stream, count);
        s_stream += count;
    }
    return count;
}

esp_err_t uart_driver_install(int port, unsigned rx, unsigned tx, unsigned queue,
        void *handle, unsigned flags)
{
    (void)tx;
    (void)queue;
    (void)handle;
    (void)flags;
    assert(port == 0);
    usb_serial_jtag_driver_config_t config = {rx, 0};
    return usb_serial_jtag_driver_install(&config);
}

esp_err_t uart_driver_delete(int port)
{
    assert(port == 0);
    return usb_serial_jtag_driver_uninstall();
}

esp_err_t uart_flush_input(int port)
{
    assert(port == 0 && s_installed);
    s_flushes++;
    return ESP_OK;
}

int uart_read_bytes(int port, void *data, size_t size, TickType_t wait)
{
    assert(port == 0);
    return usb_serial_jtag_read_bytes(data, size, wait);
}

int xTaskCreatePinnedToCore(void (*worker)(void *), const char *name,
        unsigned stack, void *arg, unsigned priority, TaskHandle_t *task, int core)
{
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    assert(core == 1);
    if (s_fail_task)
        return 0;
    s_worker = worker;
    *task = (void *)1;
    return pdPASS;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    assert(!atomic_load(&s_running));
    s_worker(NULL);
}

void vTaskDelete(void *task) { (void)task; }

int main(void)
{
    char line[16];
    s_stream = "  Doom 2.GBA  \n";
    assert(gpsp_console_read_line(line, sizeof(line), 2000));
    assert(!strcmp(gpsp_trim(line), "Doom 2.GBA") && !s_installed);
    s_stream = "abcdefgh\n";
    assert(!gpsp_console_read_line(line, 5, 2000));
    assert(!s_installed);
    s_stream = "partial";
    assert(!gpsp_console_read_line(line, sizeof(line), 2000));
    assert(!s_installed);
    unsigned installs = s_installs;
    assert(!gpsp_console_read_line(line, 0, 2000) && s_installs == installs);
    s_fail_install = true;
    assert(!gpsp_console_read_line(line, sizeof(line), 2000));
    gpsp_serial_input_start();
    assert(!s_task && !s_installed);
    s_fail_install = false;
    s_fail_task = true;
    gpsp_serial_input_start();
    assert(!s_task && !s_installed && !atomic_load(&s_running));
    s_fail_task = false;
    gpsp_serial_input_start();
    assert(s_task && s_installed && s_rx_size == 512);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    assert(s_vfs_driver && s_tx_size == 4096 && !s_flushes);
#else
    assert(!s_vfs_driver && s_flushes == 3);
#endif
    installs = s_installs;
    gpsp_serial_input_start();
    assert(s_installs == installs);

    uint8_t message[GPSP_SERIAL_MESSAGE_SIZE] = {0};
    gpsp_espemu_write_header(message, GPSP_ESPEMU_MESSAGE_INPUT,
            GPSP_ESPEMU_INPUT_SIZE, 0, 0);
    gpsp_espemu_write_u16(message + GPSP_ESPEMU_HEADER_SIZE, 0x40);
    gpsp_espemu_write_u16(message + GPSP_ESPEMU_HEADER_SIZE + 2,
            GPSP_ESPEMU_COMMAND_HOME);
    assert(gpsp_serial_apply(message));
    assert(gpsp_serial_input_read() == (0x40 | GPSP_SERIAL_HOME_BIT));
    gpsp_serial_input_stop();
    assert(!s_task && !s_installed && !gpsp_serial_input_read());
    gpsp_serial_input_start();
    assert(!gpsp_serial_input_read());
    gpsp_serial_input_stop();
    assert(!s_task && !s_installed && s_installs == s_closes);
    printf("Console PASS: %s, ROM bounds, commands, failures and restart\n",
            GPSP_SERIAL_PORT_NAME);
    return 0;
}

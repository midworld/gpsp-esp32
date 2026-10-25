#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = r'''#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef int uart_port_t;
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define tskIDLE_PRIORITY 0
void test_log(const char *, const char *, ...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
int64_t esp_timer_get_time(void);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *,
        unsigned, TaskHandle_t *, int);
void vTaskDelay(unsigned);
void vTaskDelete(void *);
typedef struct { unsigned rx_buffer_size, tx_buffer_size; } usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() {256, 256}
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
esp_err_t usb_serial_jtag_driver_uninstall(void);
void usb_serial_jtag_vfs_use_driver(void);
void usb_serial_jtag_vfs_use_nonblocking(void);
int usb_serial_jtag_read_bytes(void *, size_t, TickType_t);
esp_err_t uart_driver_install(int, unsigned, unsigned, unsigned, void *, unsigned);
esp_err_t uart_driver_delete(int);
esp_err_t uart_flush_input(int);
int uart_read_bytes(int, void *, size_t, TickType_t);
'''

storage = (ROOT / 'main/gpsp_storage.c').read_text()
# Compile the production console reader without filesystem/storage setup.
fragment = storage[storage.index('static char *gpsp_trim('):
                   storage.index('bool gpsp_storage_config(')]
with tempfile.TemporaryDirectory(prefix='gpsp-console-') as directory:
    build = Path(directory)
    (build / 'test_sdk.h').write_text(SDK)
    (build / 'storage_console.inc').write_text(fragment)
    for name in ('esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h',
                 'freertos/task.h', 'driver/uart.h', 'driver/usb_serial_jtag.h',
                 'driver/usb_serial_jtag_vfs.h'):
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "test_sdk.h"\n')
    for usb in (0, 1):
        (build / 'sdkconfig.h').write_text(
            '#pragma once\n#define CONFIG_GPSP_SERIAL_INPUT 1\n'
            f'#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG {usb}\n'
            f'#define CONFIG_ESP_CONSOLE_UART {1-usb}\n'
            '#define CONFIG_ESP_CONSOLE_UART_NUM 0\n')
        command = shlex.split(os.environ.get('HOST_CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
            '-fsanitize=address,undefined', '-I', str(build),
            '-I', str(ROOT / 'protocol'), '-I', str(ROOT / 'components/gpsp_platform'),
            str(Path(__file__).with_name('test_console.c')), '-o', str(build / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(build / 'test')], check=True)

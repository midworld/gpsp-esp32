#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#include <assert.h>
#define ESP_ERROR_CHECK(result) assert((result) == ESP_OK)
""",
    "sdkconfig.h": """#pragma once
#define CONFIG_GPSP_LCD 1
#define CONFIG_GPSP_AUDIO 1
#define CONFIG_GPSP_USB_PAD 1
#define CONFIG_GPSP_STATUS 1
#define CONFIG_GPSP_SERIAL_INPUT 1
#define CONFIG_GPSP_MENU 1
#ifndef CONFIG_GPSP_ROM_BUFFER_SIZE
#define CONFIG_GPSP_ROM_BUFFER_SIZE 0
#endif
""",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n",
    "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)
void *heap_caps_malloc(size_t size, unsigned caps);
""",
    "esp_psram.h": "#pragma once\n#include <stddef.h>\nsize_t esp_psram_get_size(void);\n",
    "freertos/FreeRTOS.h": "#pragma once\n",
    "freertos/event_groups.h": """#pragma once
typedef void *EventGroupHandle_t;
typedef unsigned EventBits_t;
""",
}

# ESP-EMU with the menu: no LCD or other P4 Nano peripherals.
ESPEMU_MENU_CONFIG = """#pragma once
#define CONFIG_ESPEMU 1
#define CONFIG_GPSP_MENU 1
#define CONFIG_GPSP_ROM_BUFFER_SIZE 0
"""


def compile_test(build, source, config):
    for name, content in HEADERS.items():
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(config if config and name == "sdkconfig.h" else content)
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                "-ffunction-sections", "-fdata-sections"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for path in [build, ROOT / "boards/espemu", ROOT / "boards/espemu/include",
                 ROOT / "boards/p4_nano/include", ROOT / "protocol",
                 ROOT / "components/gpsp/core/include"]:
        command += ["-I", str(path)]
    command += [str(ROOT / "boards/espemu/espemu_board.c"),
                str(Path(__file__).with_name(source)),
                "-Wl,-dead_strip" if sys.platform == "darwin" else
                "-Wl,--gc-sections", "-o", str(build / "test")]
    return command


with tempfile.TemporaryDirectory(prefix="gpsp-board-") as folder:
    build = Path(folder) / "board"
    command = compile_test(build, "test_board.c", None)
    for limit in (0, 6):
        subprocess.run(command + [f"-DCONFIG_GPSP_ROM_BUFFER_SIZE={limit}"], check=True)
        subprocess.run([str(build / "test")], check=True)

    build = Path(folder) / "espemu-menu"
    subprocess.run(compile_test(build, "test_menu.c", ESPEMU_MENU_CONFIG),
                   check=True)
    subprocess.run([str(build / "test")], check=True)

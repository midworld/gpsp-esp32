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
    "esp_psram.h": "#pragma once\n#include <stddef.h>\nsize_t esp_psram_get_size(void);\n",
    "freertos/FreeRTOS.h": "#pragma once\n",
    "freertos/event_groups.h": """#pragma once
typedef void *EventGroupHandle_t;
typedef unsigned EventBits_t;
""",
}

with tempfile.TemporaryDirectory(prefix="gpsp-board-") as folder:
    build = Path(folder)
    for name, content in HEADERS.items():
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                "-ffunction-sections", "-fdata-sections"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for path in [build, ROOT / "boards/espemu/include",
                 ROOT / "boards/p4_nano/include", ROOT / "components/gpsp/core/include"]:
        command += ["-I", str(path)]
    command += [str(ROOT / "boards/espemu/espemu_board.c"),
                str(Path(__file__).with_name("test_board.c")),
                "-Wl,-dead_strip" if sys.platform == "darwin" else
                "-Wl,--gc-sections", "-o", str(build / "test_board")]
    for limit in (0, 6):
        subprocess.run(command + [f"-DCONFIG_GPSP_ROM_BUFFER_SIZE={limit}"], check=True)
        subprocess.run([str(build / "test_board")], check=True)

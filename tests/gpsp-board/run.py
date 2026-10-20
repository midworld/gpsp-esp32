#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import re
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
    for path in [build, ROOT / "components/gpsp_platform", ROOT / "components/gpsp_platform/include",
                 ROOT / "components/gpsp_devices/include", ROOT / "components/gpsp_video/include", ROOT / "protocol",
                 ROOT / "components/gpsp_display/include",
                 ROOT / "components/gpsp/core/include"]:
        command += ["-I", str(path)]
    command += [str(ROOT / "components/gpsp_platform/gpsp_board.c"),
                str(Path(__file__).with_name(source)),
                "-Wl,-dead_strip" if sys.platform == "darwin" else
                "-Wl,--gc-sections", "-o", str(build / "test")]
    return command


def source_block(text, start):
    first = text.index(start)
    opening = text.index("{", first)
    depth = 0
    tokens = (r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
              r"'(?:\\.|[^'\\])*'|[{}]")
    for token in re.finditer(tokens, text[opening:], re.DOTALL):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if not depth:
                return text[first:opening + token.end()] + "\n"
    raise ValueError(f"Unclosed source block: {start}")


def video_sources(build):
    build.mkdir(parents=True)
    frontend = (ROOT / "main/gpsp_frontend.c").read_text()
    lcd = (ROOT / "components/gpsp_video/lcd.c").read_text()
    # Compile the production callbacks and queue operations with a stopped worker.
    (build / "video_callbacks.inc").write_text("\n".join(
        source_block(frontend, name) for name in (
            "static void gpsp_unload_game(", "static void gpsp_video_refresh(")))
    (build / "lcd_buffer_state.inc").write_text(
        source_block(lcd, "enum lcd_buffer_state") + ";\n")
    (build / "lcd_buffers.inc").write_text("\n".join(
        source_block(lcd, name) for name in (
            "static int gpsp_lcd_find(", "bool gpsp_lcd_present(",
            "uint16_t *gpsp_lcd_next_buffer(", "void gpsp_lcd_release_buffer(")))


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

    build = Path(folder) / "video-buffers"
    video_sources(build)
    for benchmark in (0, 1):
        command = compile_test(build, "test_video.c", None)
        subprocess.run(command + [f"-DCONFIG_GPSP_BENCHMARK={benchmark}",
                                  "-fsanitize=address,undefined"], check=True)
        subprocess.run([str(build / "test")], check=True)

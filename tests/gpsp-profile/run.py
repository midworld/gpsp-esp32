#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="gpsp-profile-") as folder:
    build = Path(folder)
    (build / "freertos").mkdir()
    (build / "sdkconfig.h").write_text("")
    (build / "freertos/FreeRTOS.h").write_text('''#pragma once
#include <assert.h>
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) assert((*lock)++ == 0)
#define portEXIT_CRITICAL(lock) assert(--(*lock) == 0)
''')
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    command += ["-I", str(build), "-I",
                str(root / "components/gpsp_platform/include")]
    for lcd, detail, benchmark, dsi, status, host in (
            (1, 0, 0, 0, 1, ""), (1, 1, 0, 0, 1, ""), (1, 1, 0, 1, 1, ""),
            (0, 0, 0, 0, 1, ""), (0, 1, 0, 0, 1, ""), (1, 0, 1, 0, 1, ""),
            (1, 0, 0, 0, 1, "127.0.0.1"), (1, 0, 0, 0, 0, ""),
            (1, 1, 0, 0, 0, "")):
        flags = [f"-DCONFIG_GPSP_LCD={lcd}",
                 f"-DCONFIG_GPSP_PERFORMANCE_DIAGNOSTICS={detail}",
                 f"-DCONFIG_GPSP_BENCHMARK={benchmark}",
                 f"-DCONFIG_GPSP_LCD_DSI_DIAGNOSTICS={dsi}",
                 f'-DCONFIG_GPSP_SERVER_HOST="{host}"',
                 f"-DCONFIG_GPSP_STATUS={status}"]
        subprocess.run(command + flags +
                       [str(root / "components/gpsp_platform/gpsp_profile.c"),
                        str(Path(__file__).with_name("test_profile.c")),
                        "-o", str(build / "test")], check=True)
        subprocess.run([str(build / "test")], check=True)
print("Profile: light, detailed, transport and benchmark checks passed")

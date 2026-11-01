#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def block(text, start):
    first = text.index(start)
    opening = text.index("{", first)
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]',
                             text[opening:], re.DOTALL):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if not depth:
                return text[first:opening + token.end()] + "\n"
    raise ValueError(start)


with tempfile.TemporaryDirectory(prefix="gpsp-virtual-pad-") as folder:
    binary = Path(folder) / "test"
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for include in ["components/gpsp_devices/include",
                    "components/gpsp/core/libretro/libretro-common/include"]:
        command += ["-I", str(root / include)]
    command += ["-I", folder, str(root / "components/gpsp_devices/virtual_pad.c")]
    subprocess.run(command + [str(Path(__file__).with_name("test_virtual_pad.c")),
                             "-o", str(binary)], check=True)
    subprocess.run([binary], check=True)

    sources = {
        "components/gpsp_devices/touch_st7123.c": (
            "static bool touch_publish(", "uint32_t gpsp_touch_read(",
            "uint32_t gpsp_touch_pressed(", "void gpsp_touch_set_enabled(",
            "void gpsp_touch_cancel("),
        "components/gpsp_platform/gpsp_board.c": (
            "uint32_t gpsp_board_virtual_input(",
            "uint32_t gpsp_board_virtual_pressed(",
            "uint32_t gpsp_board_input_read("),
        "main/gpsp_frontend.c": (
            "static uint32_t gpsp_menu_buttons(", "static bool gpsp_menu_requested(",
            "static void gpsp_input_poll("),
    }
    functions = []
    for path, names in sources.items():
        text = (root / path).read_text()
        for name in names:
            function = block(text, name)
            if name in ("static uint32_t gpsp_menu_buttons(",
                        "static bool gpsp_menu_requested("):
                function = "#if CONFIG_GPSP_MENU\n" + function + "#endif\n"
            functions.append(function)
    (Path(folder) / "touch_input.inc").write_text("\n".join(functions))
    for menu in (0, 1):
        for frameskip in (0, 1):
            subprocess.run(command + [f"-DCONFIG_GPSP_MENU={menu}",
                           f"-DGPSP_FRAMESKIP={frameskip}",
                           str(Path(__file__).with_name("test_touch_input.c")),
                           "-o", str(binary)], check=True)
            subprocess.run([binary], check=True)

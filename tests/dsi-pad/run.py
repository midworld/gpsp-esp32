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


with tempfile.TemporaryDirectory(prefix="gpsp-dsi-pad-") as folder:
    build = Path(folder)
    (build / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n")
    pad = (root / "components/gpsp_video/dsi_pad.c").read_text()
    layout = (root / "components/gpsp_devices/virtual_pad.c").read_text()
    (build / "pad_blend.inc").write_text(block(pad, "static esp_err_t pad_blend("))
    (build / "pad_layout.inc").write_text(
        block(layout, "const struct gpsp_pad_rect gpsp_pad_rects[") + ";\n")
    pictures = block(pad, "static const uint32_t s_region_masks[") + ";\n"
    for name in ("static void pad_pixel(", "static uint32_t pad_color(",
                 "static void pad_text(", "static void pad_build(",
                 "static inline void pad_fill(", "static void pad_recolor(",
                 "static void pad_update("):
        pictures += block(pad, name)
    (build / "pad_pictures.inc").write_text(pictures)
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for path in [build, root / "components/gpsp_video",
                 root / "components/gpsp_devices/include",
                 root / "components/gpsp_font/include",
                 root / "components/gpsp/core/libretro/libretro-common/include"]:
        command += ["-I", str(path)]
    for rotation in (0, 1):
        subprocess.run(command + [f"-DCONFIG_GPSP_LCD_ROTATE_180={rotation}",
                       str(Path(__file__).with_name("test_blend.c")),
                       "-o", str(build / "test")], check=True)
        subprocess.run([str(build / "test")], check=True)
        subprocess.run(command + [f"-DCONFIG_GPSP_LCD_ROTATE_180={rotation}",
                       "-Wno-sign-compare",
                       str(Path(__file__).with_name("test_recolor.c")),
                       str(root / "components/gpsp_font/gpsp_font.c"),
                       "-o", str(build / "test")], check=True)
        subprocess.run([str(build / "test")], check=True)

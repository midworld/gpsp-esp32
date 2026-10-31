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


with tempfile.TemporaryDirectory(prefix="gpsp-sdmmc-access-") as folder:
    build = Path(folder)
    source = (root / "components/gpsp_devices/sdmmc.c").read_text()
    defaults = re.search(r"#if CONFIG_GPSP_SD_SINGLE_BLOCK_WRITES\n.*?#endif\n",
                         source, re.DOTALL)
    if not defaults:
        raise ValueError("Single-block write defaults not found")
    functions = defaults[0]
    for name in ("static esp_err_t sd_access(", "static DSTATUS sd_status(",
                 "static DRESULT sd_read(", "static DRESULT sd_write(",
                 "static DRESULT sd_ioctl("):
        functions += block(source, name)
    (build / "sd_access.inc").write_text(functions)
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for single in (0, 1):
        subprocess.run(command + [f"-DCONFIG_GPSP_SD_SINGLE_BLOCK_WRITES={single}",
                       "-I", str(build), str(Path(__file__).with_name("test_access.c")),
                       "-o", str(build / "test")], check=True)
        subprocess.run([str(build / "test")], check=True)

#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="gpsp-virtual-pad-") as folder:
    binary = Path(folder) / "test"
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    for include in ["components/gpsp_devices/include",
                    "components/gpsp/core/libretro/libretro-common/include"]:
        command += ["-I", str(root / include)]
    command += [str(root / "components/gpsp_devices/virtual_pad.c"),
                str(Path(__file__).with_name("test_virtual_pad.c")), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)

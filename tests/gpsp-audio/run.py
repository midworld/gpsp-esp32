#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="gpsp-audio-") as folder:
    binary = Path(folder) / "test"
    command = shlex.split(os.environ.get("HOST_CC", "cc"))
    command += ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-fsanitize=address,undefined"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    command += ["-I", str(root / "components/gpsp_devices"),
                str(root / "components/gpsp_devices/audio_highpass.c"),
                str(Path(__file__).with_name("test_highpass.c")),
                "-lm", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

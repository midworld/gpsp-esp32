#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Build the code chunk ROM without a copyrighted GBA header."""

import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if len(sys.argv) != 2:
    raise SystemExit("usage: build_rom.py OUTPUT.gba")
OUTPUT = Path(sys.argv[1])
OBJECT = OUTPUT.with_suffix(".o")
ELF = OUTPUT.with_suffix(".elf")
BINARY = OUTPUT.with_suffix(".bin")
CLANG = os.environ.get(
    "ARM_CLANG", "/usr/bin/clang" if Path("/usr/bin/clang").exists() else "clang")

subprocess.run([
    CLANG, "--target=armv4t-none-eabi",
    "-c", "-o", str(OBJECT), str(HERE / "chunk.s"),
], check=True)
subprocess.run([
    os.environ.get("LLD", "lld"), "-flavor", "gnu", "-m", "armelf",
    "-Ttext=0x08000000", "-o", str(ELF), str(OBJECT),
], check=True)
subprocess.run([
    os.environ.get("LLVM_OBJCOPY", "llvm-objcopy"), "-O", "binary",
    str(ELF), str(BINARY),
], check=True)
rom = BINARY.read_bytes()
OUTPUT.write_bytes(rom.ljust(max(0x4000, len(rom)), b"\0"))
print(OUTPUT)

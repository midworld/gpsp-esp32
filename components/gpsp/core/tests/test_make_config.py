#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check standalone configuration without invoking a cross compiler."""

import os
from pathlib import Path
import shlex
import subprocess

CORE = Path(__file__).resolve().parent.parent
MAKE = os.environ.get("MAKE", "make")
BASE_ENV = {k: v for k, v in os.environ.items()
            if not k.startswith(("GPSP_", "RV32_", "MAKE"))
            and k not in ("ROM_BUFFER_SIZE", "CFLAGS", "CXXFLAGS", "ASFLAGS")}


def build(env=None, args=(), succeeds=True):
    result = subprocess.run(
        [MAKE, "-nB", "platform=rv32", *args], cwd=CORE,
        env=dict(BASE_ENV, **(env or {})), text=True, capture_output=True)
    assert (result.returncode == 0) == succeeds, result.stderr
    commands = [shlex.split(line) for line in result.stdout.splitlines()
                if " -c " in line]
    if succeeds:
        assert commands, result.stdout
    return commands


def contains(commands, flag, present=True):
    for command in commands:
        assert (flag in command) == present, (flag, command)


settings = {
    "GPSP_RV32_USE_ZBA": "1", "GPSP_RV32_USE_ZBB": "1",
    "GPSP_RV32_USE_ZBS": "1", "GPSP_RV32_USE_RVC": "1",
    "GPSP_RV32_USE_ZCB": "1",
    "GPSP_RV32_ROM_CACHE_KIB": "1024", "GPSP_RV32_RAM_CACHE_KIB": "512",
    "GPSP_RV32_CODE_CHUNK_KIB": "128",
    "ROM_BUFFER_SIZE": "4", "GPSP_BENCHMARK": "1",
    "GPSP_BENCHMARK_PHASE_WORK": "1", "GPSP_BENCHMARK_INSTRET": "1",
    "GPSP_BENCHMARK_JIT_DETAIL": "1", "GPSP_BENCHMARK_JIT_LOOKUP": "1",
    "GPSP_BENCHMARK_JIT_MISS_CLASS": "1", "GPSP_BENCHMARK_JIT_CODE_BYTES": "1",
    "GPSP_BENCHMARK_VIDEO_DETAIL": "1", "GPSP_BENCHMARK_RAM_CODE": "1",
    "GPSP_AFFINE_PIE": "1",
}
for commands in (build(env=settings), build(args=[f"{k}={v}" for k, v in settings.items()])):
    for name, value in settings.items():
        contains(commands, f"-D{name}={value}")
    assert any(command[-1].endswith("rv32_affine_pie.S") for command in commands)

commands = build(env={"GPSP_RV32_USE_RVC": "1"}, args=["GPSP_RV32_USE_RVC=0"])
contains(commands, "-DGPSP_RV32_USE_RVC=0")
contains(commands, "-DGPSP_RV32_USE_RVC=1", False)
commands = build()
contains(commands, "-DGPSP_RV32_JIT=1")
assert any("rv32_block_runtime.cc" in command[-1] for command in commands)

commands = build(env={"RV32_ARCH": "rv32im_zicsr_zifencei", "RV32_ABI": "ilp32",
                      "GPSP_BENCHMARK": "0", "GPSP_BENCHMARK_JIT_DETAIL": "0"})
contains(commands, "-march=rv32im_zicsr_zifencei")
contains(commands, "-mabi=ilp32")
contains(commands, "-DGPSP_BENCHMARK=1", False)
contains(commands, "-DGPSP_BENCHMARK_JIT_DETAIL=0", False)
for args in (["GPSP_RV32_USE_RVC=yes"], ["GPSP_RV32_USE_ZBB=1 0"],
             ["GPSP_BENCHMARK=0", "GPSP_BENCHMARK_JIT_DETAIL=1"]):
    build(args=args, succeeds=False)

# Exported target settings must not introduce target sources into host fixtures.
host_settings = dict(settings, HAVE_DYNAREC="1", platform="rv32")
for suite in ("game-switch", "core-regressions"):
    for env, args in ((host_settings, []),
                      ({}, [f"{k}={v}" for k, v in host_settings.items()])):
        result = subprocess.run(
            [MAKE, "-nB", "-C", str(CORE / "tests" / suite), "test", *args],
            env=dict(BASE_ENV, **env), text=True, capture_output=True)
        assert result.returncode == 0, result.stderr
        commands = [shlex.split(line) for line in result.stdout.splitlines()
                    if " -c " in line]
        assert commands
        for command in commands:
            assert not any("/riscv/" in arg or "cpu_threaded.c" in arg
                           or "gpsp_benchmark_timing.c" in arg for arg in command), command
        contains(commands, "-DGPSP_RV32_JIT=1", False)
print("make configuration: PASS (environment, arguments, precedence, sources, validation)")

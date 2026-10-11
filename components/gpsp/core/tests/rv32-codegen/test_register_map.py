#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check register-map constraints in C and assembler preprocessing."""

from pathlib import Path
import re
import shlex
import subprocess
import sys


def main():
    compiler = shlex.split(sys.argv[1])
    header = Path(sys.argv[2]).read_text()
    # Constraint cases below change the production map.
    start = header.index("#define GPSP_RV32_REG_STATE")
    end = header.index("#define GPSP_RV32_REG_BIT", start)
    pattern = re.compile(r"^(#define (GPSP_RV32_REG_\w+)\s+)(\d+)", re.M)
    registers = {m[2]: int(m[3])
                 for m in pattern.finditer(header, start, end)}
    assert len(registers) == 24
    prefix = "GPSP_RV32_REG_"

    def swap(first, second):
        return {first: registers[prefix + second],
                second: registers[prefix + first]}

    def check(name, changes, error=None, defines=()):
        changes = {prefix + key: value for key, value in changes.items()}
        assert changes.keys() <= registers.keys()
        source = header[:start] + pattern.sub(
            lambda m: m[1] + str(changes.get(m[2], registers[m[2]])),
            header[start:end]) + header[end:]
        for language in ("c", "assembler-with-cpp"):
            options = (["-fsyntax-only"] if language == "c" else ["-E"])
            tail = ("\nint use(void) { return RV_PC + guest_reg_map[0]; }\n"
                    if language == "c" else "\n")
            result = subprocess.run(
                compiler + ["-Wall", "-Wextra", "-Werror", "-x", language]
                + list(defines) + options + ["-"], input=source + tail, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if error is None:
                passed = result.returncode == 0
            else:
                passed = result.returncode != 0 and error in result.stderr
            if not passed:
                raise AssertionError(
                    f"{name} ({language}): exit {result.returncode}\n"
                    + result.stderr)

    cases = [
        ("current", {}, None),
        ("tmp0_s7", swap("TMP0", "GUEST_R4"), None),
        ("tmp1_a3", swap("TMP1", "GUEST_R0"), None),
        ("overlap", {"GUEST_R0": registers[prefix + "TMP0"]},
         "JIT register roles overlap"),
        ("reserved", {"GUEST_R0": 12},
         "JIT register map uses a reserved register"),
        ("caller_pc", swap("PC", "TMP1"),
         "JIT state registers must survive C calls"),
        ("tmp0_a3", swap("TMP0", "GUEST_R0"),
         "JIT helper target must not use a3"),
    ]
    for register in registers:
        role = register.removeprefix(prefix)
        for value in (-1, 32, 64):
            cases.append((f"{role}={value}", {role: value},
                          "JIT register numbers must be in 0..31"))
    for case in cases:
        check(*case)
    print(f"RV32 register map: {len(cases)} cases passed "
          "(C and assembler preprocessing)")


if __name__ == "__main__":
    main()

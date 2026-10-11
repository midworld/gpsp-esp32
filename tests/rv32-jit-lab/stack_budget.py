#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Read compiler stack frames for the fixed four-op baseline."""

import sys


def frames(paths):
    result = {}
    for path in paths:
        with open(path, encoding="utf-8") as source:
            for line in source:
                location, size, kind = line.rstrip().split("\t")
                if kind == "static":
                    result[location.rsplit(":", 1)[-1]] = int(size)
    return result


def main():
    used = frames(sys.argv[1:])
    common = ("main", "translate_current", "gpsp_rv32_emit_block_instruction")
    for name in common + ("gpsp_rv32_emit_thumb",):
        if name not in used:
            raise SystemExit(f"missing compiler stack frame: {name}")
    paths = {
        "TST": common + ("gpsp_rv32_emit_thumb", "emit_nz_reg"),
        "Bcc": common + ("emit_branch_wait", "emit_absolute_load"),
        "MOVS": common + ("gpsp_rv32_emit_thumb",),
    }
    for name, path in paths.items():
        print(f"{name}_stack_path_bytes={sum(used.get(part, 0) for part in path)}")
        print("  " + ", ".join(f"{part}={used.get(part, 0)}" for part in path))
    print("stack_path_peak_bytes=" + str(max(
        sum(used.get(part, 0) for part in path) for path in paths.values())))


if __name__ == "__main__":
    main()

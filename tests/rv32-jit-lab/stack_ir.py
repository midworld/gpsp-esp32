#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Report host stack paths for the fixed Thumb IR experiment."""

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
    caller = ("main", "time_translations", "translate_ir")
    paths = {
        "IR emit and cold stub": caller +
            ("gpsp_rv32_thumb_ir_emit", "gpsp_rv32_emit_interpreter_step_stub"),
        "IR emit and side entry": caller +
            ("gpsp_rv32_thumb_ir_emit", "emit_side_entry", "emit_wait"),
        "IR append": caller +
            ("gpsp_rv32_thumb_ir_append_entry", "emit_side_entry", "emit_wait"),
    }
    for name, path in paths.items():
        print(f"{name}_stack_path_bytes={sum(used[part] for part in path)}")
        print("  " + ", ".join(f"{part}={used[part]}" for part in path))
    print("ir_stack_path_peak_bytes=" + str(max(
        sum(used[part] for part in path) for path in paths.values())))


if __name__ == "__main__":
    main()

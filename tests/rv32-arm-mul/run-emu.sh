#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

firmware=${1:-build/merged-binary.bin}
log_file=$(mktemp "${TMPDIR:-/tmp}/rv32-arm-mul.XXXXXX")
trap 'rm -f "$log_file"' EXIT

if ! esp-emu --chip esp32p4 --firmware "$firmware" --psram-size 32M \
        --exit-on 'RV32 ARM MUL COMPLETE' --timeout 60s \
        --log-color never > "$log_file" 2>&1; then
    cat "$log_file"
    exit 1
fi

cat "$log_file"
if grep -Fq 'Timeout reached' "$log_file" ||
   ! grep -Fq 'RV32 ARM MUL RESULT: PASS' "$log_file" ||
   ! grep -Fq "Exit-on string 'RV32 ARM MUL COMPLETE' found in output" "$log_file"; then
    echo 'RV32 ARM multiply test failed' >&2
    exit 1
fi

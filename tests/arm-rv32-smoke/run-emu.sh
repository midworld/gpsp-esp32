#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

firmware=${1:-build/merged-binary.bin}
log_file=$(mktemp "${TMPDIR:-/tmp}/arm-rv32-smoke.XXXXXX")
trap 'rm -f "$log_file"' EXIT

if ! esp-emu --chip esp32p4 --firmware "$firmware" --psram-size 32M \
        --exit-on 'ARM RV32 SMOKE COMPLETE' --timeout 60s \
        --log-color never > "$log_file" 2>&1; then
    cat "$log_file"
    exit 1
fi

cat "$log_file"
if grep -Fq 'Timeout reached' "$log_file" ||
   ! grep -Fq 'ARM RV32 SMOKE RESULT: PASS' "$log_file" ||
   ! grep -Fq "Exit-on string 'ARM RV32 SMOKE COMPLETE' found in output" "$log_file"; then
    echo 'ARM RV32 smoke test failed' >&2
    exit 1
fi

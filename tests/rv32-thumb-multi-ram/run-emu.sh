#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

firmware=${1:-build/merged-binary.bin}
log_file=$(mktemp "${TMPDIR:-/tmp}/rv32-thumb-multi-ram.XXXXXX")
trap 'rm -f "$log_file"' EXIT

esp-emu --chip esp32p4 --firmware "$firmware" \
    --exit-on 'RV32 THUMB MULTI RAM COMPLETE' --timeout 20s \
    --log-color never 2>&1 | awk '
    /RV32 THUMB MULTI RAM|Exit-on string|Timeout reached/ { print; next }
    /Bus fault|Illegal instruction|abort\(\)/ { if (errors++ < 8) print }
' > "$log_file"

cat "$log_file"
if grep -Fq 'Timeout reached' "$log_file" ||
   ! grep -Fq 'RV32 THUMB MULTI RAM RESULT: PASS' "$log_file" ||
   ! grep -Fq "Exit-on string 'RV32 THUMB MULTI RAM COMPLETE' found in output" "$log_file"; then
    echo 'RV32 Thumb multi RAM test failed' >&2
    exit 1
fi

#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

firmware=${1:-build/merged-binary.bin}
log_file=$(mktemp "${TMPDIR:-/tmp}/riscv-helper-abi.XXXXXX")
trap 'rm -f "$log_file"' EXIT

if ! esp-emu --chip esp32p4 --firmware "$firmware" \
        --exit-on 'RV32 HELPER ABI COMPLETE' --timeout 60s \
        --log-color never > "$log_file" 2>&1; then
    cat "$log_file"
    exit 1
fi

cat "$log_file"
if grep -Fq 'Timeout reached' "$log_file" ||
   ! grep -Fq 'RV32 HELPER ABI RESULT: PASS' "$log_file" ||
   ! grep -Fq "Exit-on string 'RV32 HELPER ABI COMPLETE' found in output" "$log_file"; then
    echo 'RV32 helper ABI test failed' >&2
    exit 1
fi

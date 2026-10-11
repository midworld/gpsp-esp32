#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

firmware=${1:-build/merged-binary.bin}
log_file=$(mktemp "${TMPDIR:-/tmp}/rv32-thumb-multi-emitter.XXXXXX")
trap 'rm -f "$log_file"' EXIT

esp-emu --chip esp32p4 --firmware "$firmware" --psram-size 32M \
    --exit-on 'RV32 THUMB MULTI EMITTER COMPLETE' --timeout 30s \
    --log-color never > "$log_file" 2>&1

grep -E 'RV32 THUMB MULTI EMITTER|GOLDEN |Exit-on string|Timeout reached|Bus fault|Illegal instruction' "$log_file" || true
if grep -Fq 'Timeout reached' "$log_file" ||
   ! grep -Fq 'RV32 THUMB MULTI EMITTER RESULT: PASS' "$log_file" ||
   ! grep -Fq "Exit-on string 'RV32 THUMB MULTI EMITTER COMPLETE' found in output" "$log_file"; then
    cat "$log_file"
    exit 1
fi

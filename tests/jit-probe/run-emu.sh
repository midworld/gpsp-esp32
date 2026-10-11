#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 FIRMWARE PASS|UNAVAILABLE [esp32p4|esp32s31]" >&2
    exit 2
fi

firmware=$1
expected=$2
chip=${3:-esp32p4}
exit_on='JIT PROBE COMPLETE'

case "$chip" in
    esp32p4|esp32s31) ;;
    *)
        echo "Chip must be esp32p4 or esp32s31" >&2
        exit 2
        ;;
esac

case "$expected" in
    PASS)
        expected_result='JIT PROBE RESULT: PASS (internal=PASS small=PASS full=PASS contiguous=PASS)'
        ;;
    UNAVAILABLE)
        expected_result='JIT PROBE RESULT: UNAVAILABLE (memory protection enabled)'
        ;;
    *)
        echo "Expected result must be PASS or UNAVAILABLE" >&2
        exit 2
        ;;
esac

log_file=$(mktemp "${TMPDIR:-/tmp}/esp32-jit-probe.XXXXXX")
trap 'rm -f "$log_file"' 0

if esp-emu --chip "$chip" --firmware "$firmware" --psram-size 32M \
        --exit-on "$exit_on" --timeout 60s --log-color never > "$log_file" 2>&1; then
    :
else
    result=$?
    cat "$log_file"
    exit "$result"
fi

cat "$log_file"
if grep -Fq 'Timeout reached' "$log_file"; then
    echo "ESP-EMU timed out before completing the probe" >&2
    exit 1
fi
if ! grep -Fq "Exit-on string '$exit_on' found in output" "$log_file"; then
    echo "ESP-EMU did not reach $exit_on" >&2
    exit 1
fi
if ! grep -Fq "$expected_result" "$log_file"; then
    echo "JIT probe did not report $expected_result" >&2
    exit 1
fi

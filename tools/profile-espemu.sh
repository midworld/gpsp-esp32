#!/usr/bin/env bash
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

# Capture one repeatable ESP-EMU run with firmware timings and a CPU trace.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
duration=""
run_dir=""
build_dir="$repo_dir/build"

usage() {
    printf 'Usage: %s [--build-dir DIR] [DURATION] [OUTPUT_DIR]\n' "$0"
    printf 'DIR is relative to the repository (default: build).\n'
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --build-dir)
            if [[ "$#" -lt 2 || -z "$2" ]]; then
                printf '%s requires a directory.\n' "$1" >&2
                exit 2
            fi
            build_dir="$2"
            shift 2
            ;;
        --build-dir=*)
            build_dir="${1#*=}"
            if [[ -z "$build_dir" ]]; then
                printf '%s requires a directory.\n' "--build-dir" >&2
                exit 2
            fi
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -z "$duration" ]]; then
                duration="$1"
            elif [[ -z "$run_dir" ]]; then
                run_dir="$1"
            else
                usage >&2
                exit 2
            fi
            shift
            ;;
    esac
done

duration="${duration:-30s}"
run_dir="${run_dir:-$repo_dir/profiles/$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ "$build_dir" != /* ]]; then
    build_dir="$repo_dir/$build_dir"
fi
if [[ -d "$build_dir" ]]; then
    build_dir="$(cd "$build_dir" && pwd)"
fi

firmware="$build_dir/merged-binary.bin"
elf="$build_dir/gpsp_esp32.elf"
storage="$build_dir/storage.bin"
cmake_cache="$build_dir/CMakeCache.txt"
build_config="$build_dir/config/sdkconfig.h"
viewer="$repo_dir/build/espemu-host/gpsp-espemu-host"
emu="${ESP_EMU_BIN:-esp-emu}"
viewer_pid=""

if [[ ! -f "$firmware" || ! -f "$elf" ]]; then
    printf 'Build the firmware in %s with idf.py ... merge-bin first.\n' \
        "$build_dir" >&2
    exit 1
fi

if [[ ! -f "$build_config" ]] ||
        ! grep -qx '#define CONFIG_ESPEMU 1' "$build_config"; then
    printf 'Build %s with CONFIG_ESPEMU=y first.\n' "$build_dir" >&2
    exit 1
fi

for image_part in "$elf" "$storage" "$build_config" \
    "$build_dir/bootloader/bootloader.bin" \
    "$build_dir/partition_table/partition-table.bin"; do
    if [[ -f "$image_part" && "$image_part" -nt "$firmware" ]]; then
        printf 'Merged firmware is older than %s. Run idf.py ... merge-bin again.\n' \
            "$image_part" >&2
        exit 1
    fi
done

if [[ ! -x "$viewer" ]]; then
    printf 'Build the viewer with cmake --build build/espemu-host first.\n' >&2
    exit 1
fi

if ! command -v "$emu" >/dev/null 2>&1; then
    printf 'esp-emu is not available; set ESP_EMU_BIN to its path.\n' >&2
    exit 1
fi

sdkconfig="$repo_dir/sdkconfig"
if [[ -f "$cmake_cache" ]]; then
    configured_sdkconfig="$(sed -n 's/^SDKCONFIG:[^=]*=//p' "$cmake_cache")"
    if [[ -n "$configured_sdkconfig" ]]; then
        sdkconfig="$configured_sdkconfig"
        if [[ "$sdkconfig" != /* ]]; then
            sdkconfig="$repo_dir/$sdkconfig"
        fi
    fi
fi

if [[ -f "$sdkconfig" && "$sdkconfig" -nt "$firmware" ]]; then
    printf 'Merged firmware is older than %s. Run idf.py ... merge-bin again.\n' \
        "$sdkconfig" >&2
    exit 1
fi

jit_mode="rv32-jit"

mkdir -p "$run_dir"
run_dir="$(cd "$run_dir" && pwd)"

{
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'duration=%s\n' "$duration"
    printf 'build_dir=%s\n' "$build_dir"
    printf 'sdkconfig=%s\n' "$sdkconfig"
    printf 'build_config=%s\n' "$build_config"
    printf 'jit_mode=%s\n' "$jit_mode"
    printf 'chip=esp32p4\npsram_size=32M\n'
    printf 'git_commit=%s\n' "$(git -C "$repo_dir" rev-parse HEAD)"
    printf 'git_dirty=%s\n' "$(git -C "$repo_dir" status --porcelain | wc -l | tr -d ' ')"
    shasum -a 256 "$firmware" "$elf"
    if [[ -f "$sdkconfig" ]]; then
        shasum -a 256 "$sdkconfig"
        grep -E '^CONFIG_COMPILER_OPTIMIZATION_(DEBUG|SIZE|PERF|NONE)=y$|^CONFIG_GPSP_ESPEMU_UNTHROTTLED=y$|^# CONFIG_GPSP_ESPEMU_UNTHROTTLED is not set$' "$sdkconfig" || true
    fi
    if [[ -f "$build_config" ]]; then
        shasum -a 256 "$build_config"
    fi
    if [[ -f "$cmake_cache" ]]; then
        grep '^GPSP_ROM_PATH:FILEPATH=' "$cmake_cache" || true
    fi
    if [[ -f "$storage" ]]; then
        shasum -a 256 "$storage"
    fi
} > "$run_dir/metadata.txt"

cleanup() {
    if [[ -n "$viewer_pid" ]]; then
        kill "$viewer_pid" 2>/dev/null || true
        wait "$viewer_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

printf 'Profile run: %s\n' "$run_dir"
"$viewer" 127.0.0.1 24680 "$run_dir/metrics.csv" \
    > "$run_dir/viewer.log" 2>&1 &
viewer_pid=$!

"$emu" --chip esp32p4 --firmware "$firmware" --elf "$elf" \
    --psram-size 32M --net user --timeout "$duration" \
    --trace "$run_dir/trace.json" \
    > "$run_dir/firmware.log" 2>&1

sleep 1
if [[ ! -f "$run_dir/metrics.csv" ]] ||
   [[ "$(wc -l < "$run_dir/metrics.csv")" -lt 2 ]]; then
    printf 'No profile interval arrived; check viewer.log and firmware.log.\n' >&2
    exit 1
fi

printf 'Saved metrics.csv, trace.json, firmware.log, viewer.log, metadata.txt\n'

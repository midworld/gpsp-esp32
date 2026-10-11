#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

# Run JIT tests with no Zb extension, each one alone, and all of them, with
# RVC=0 and Zcb off; rvc adds RVC=1 and Zcb, as on P4, and rvc-nozcb RVC=1
# alone. zcb-nozbb checks that the emitted Zcb forms do not need Zbb.
# zcb-rvc0 adds Zcb to all and must match its golden dump.
# Golden dumps cover RVC=0 only.
# Usage: ./rv32-zb-matrix.sh [host|emu]...  (default: host emu)
# emu needs an active ESP-IDF environment and esp-emu.
# host uses RV_TOOL_PREFIX, defaulting to riscv32-esp-elf- from the active SDK.
# Generated-code dumps (rv32-golden.py): RV32_GOLDEN_SAVE=DIR keeps each emu
# log and ELF; RV32_GOLDEN_BASE=DIR compares against a saved set.

set -eu
if [ "$#" -eq 0 ]; then
    set -- host emu
fi
for part in "$@"; do
    case "$part" in
        host|emu) ;;
        *) echo "Usage: $0 [host|emu]..." >&2; exit 2 ;;
    esac
done
cd "$(dirname "$0")"

emu_tests="arm-rv32-smoke rv32-arm-memory rv32-thumb-multi-emitter
    rv32-thumb-ram-helper riscv-stub-abi riscv-helper-abi
    rv32-thumb-multi-ram rv32-arm-mul"
golden_tests="arm-rv32-smoke rv32-arm-memory rv32-thumb-multi-emitter"
variants="none ZBA ZBB ZBS all rvc rvc-nozcb
    zcb-rvc0 zcb-nozbb"

flags() {
    case $1 in
        none) echo "" ;;
        zcb-nozbb) echo "ZBA ZBS" ;;
        all|rvc|rvc-nozcb|zcb-rvc0)
            echo "ZBA ZBB ZBS" ;;
        *) echo "$1" ;;
    esac
}

if [ -n "${RV32_GOLDEN_BASE:-}" ]; then
    python3 rv32-golden.py selftest
fi
for part in "$@"; do
    if [ "$part" = host ]; then
        make -C ../components/gpsp/core/tests check-matrix \
            RV_TOOL_PREFIX="${RV_TOOL_PREFIX:-riscv32-esp-elf-}"
        continue
    fi
    for variant in $variants; do
        zb=$(flags "$variant")
        rvc=0
        zcb=0
        golden=$variant
        case $variant in
            rvc|zcb-nozbb) rvc=1; zcb=1 ;;
            rvc-nozcb) rvc=1 ;;
            zcb-rvc0) zcb=1; golden=all ;;
        esac
        for test in $emu_tests; do
            echo "== $test $variant"
            defs="-D GPSP_RV32_USE_RVC=$rvc -D GPSP_RV32_USE_ZCB=$zcb"
            for ext in ZBA ZBB ZBS; do
                case " $zb " in
                    *" $ext "*) value=1 ;;
                    *) value=0 ;;
                esac
                defs="$defs -D GPSP_RV32_USE_$ext=$value"
            done
            defs="$defs -D GPSP_TEST_GOLDEN=ON"
            out="build/zb-$variant"
            mkdir -p "$test/build"
            # shellcheck disable=SC2086
            if ! (cd "$test" &&
                    idf.py -B "$out" $defs build merge-bin \
                        > "$out.log" 2>&1); then
                tail -30 "$test/$out.log"
                exit 1
            fi
            if ! (cd "$test" && ./run-emu.sh "$out/merged-binary.bin" \
                    > "$out-emu.log" 2>&1); then
                tail -30 "$test/$out-emu.log"
                exit 1
            fi
            grep -E 'RESULT' "$test/$out-emu.log" | tail -1
            if [ "$rvc" = 1 ]; then
                continue
            fi
            case " $(echo $golden_tests) " in
                *" $test "*)
                    if ! grep -q '^GOLDEN ' "$test/$out-emu.log"; then
                        echo "$test: no GOLDEN lines"
                        exit 1
                    fi ;;
            esac
            if grep -q '^GOLDEN ' "$test/$out-emu.log"; then
                elf=$(ls "$test/$out"/*.elf | head -1)
                if [ -n "${RV32_GOLDEN_SAVE:-}" ] &&
                        [ "$golden" = "$variant" ]; then
                    mkdir -p "$RV32_GOLDEN_SAVE/$test"
                    cp "$test/$out-emu.log" \
                        "$RV32_GOLDEN_SAVE/$test/$variant.log"
                    cp "$elf" "$RV32_GOLDEN_SAVE/$test/$variant.elf"
                fi
                if [ -n "${RV32_GOLDEN_BASE:-}" ]; then
                    python3 rv32-golden.py compare \
                        "$RV32_GOLDEN_BASE/$test/$golden.log" \
                        "$RV32_GOLDEN_BASE/$test/$golden.elf" \
                        "$test/$out-emu.log" "$elf" || exit 1
                fi
            fi
        done
    done
done
echo "Zb matrix: PASS"

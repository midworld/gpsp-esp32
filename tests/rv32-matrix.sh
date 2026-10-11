#!/bin/sh
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-2.0-or-later

# Run the production register map across generated ISA options.
set -eu
cd "$(dirname "$0")"
for variant in none ZBA ZBB ZBS all rvc rvc-nozcb zcb-rvc0 zcb-nozbb; do
    zb= rvc=0 zcb=0
    case "$variant" in
        ZBA|ZBB|ZBS) zb=$variant ;;
        zcb-nozbb) zb="ZBA ZBS" ;;
        none) ;;
        *) zb="ZBA ZBB ZBS" ;;
    esac
    case "$variant" in
        rvc|zcb-nozbb) rvc=1; zcb=1 ;;
        rvc-nozcb) rvc=1 ;;
        zcb-rvc0) zcb=1 ;;
    esac
    echo "RV32 variant: $variant"
    "${MAKE:-make}" check-rv32 ZB="$zb" RVC=$rvc ZCB=$zcb
done

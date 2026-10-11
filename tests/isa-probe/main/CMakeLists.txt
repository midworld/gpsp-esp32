# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "probe_main.c" "probe_runtime.c"
        "probe_zcb.c" "probe_zcmp.c" "probe_zcmt.c"
        "probe_zb.c" "zb_snippets.S"
        "probe_core.c" "core_snippets.S"
        "probe_af.c" "af_snippets.S"
        "probe_xesploop.c" "xesploop_snippets.S"
        "probe_xespv.c" "xespv_snippets.S"
        "guarded_call.S"
        "zcmp_snippets.S" "zcmt_snippets.S"
    REQUIRES
        esp_mm
)

set(ISA_PROBE_PIE_CASE 0 CACHE STRING "PIE probe: 0=all, 1=vector, 2=flash addx2, 3-6=RAM scalar")
target_compile_definitions(${COMPONENT_LIB} PRIVATE ISA_PROBE_PIE_CASE=${ISA_PROBE_PIE_CASE})

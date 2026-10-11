# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "simd_smoke.c" "simd_ops.S"
        "compressed_smoke.c" "compressed_smoke.S"
        "benchmark.c" "benchmark_ops.S"
        "memory_bench.c"
        "zcmp_characterize.c" "zcmp_characterize.S"
        "zcmp_interrupt.c" "zcmp_interrupt.S"
        "../../isa-probe/main/probe_runtime.c"
        "../../isa-probe/main/probe_core.c"
        "../../isa-probe/main/core_snippets.S"
        "../../isa-probe/main/probe_zb.c"
        "../../isa-probe/main/zb_snippets.S"
        "../../isa-probe/main/probe_zcb.c"
        "../../isa-probe/main/probe_zcmp.c"
        "../../isa-probe/main/zcmp_snippets.S"
        "../../isa-probe/main/probe_zcmt.c"
        "../../isa-probe/main/zcmt_snippets.S"
        "../../isa-probe/main/guarded_call.S"
    INCLUDE_DIRS
        "../../isa-probe/main"
    REQUIRES
        esp_mm esp_psram esp_timer
)

set_source_files_properties(benchmark.c PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(memory_bench.c PROPERTIES COMPILE_OPTIONS "-O2")

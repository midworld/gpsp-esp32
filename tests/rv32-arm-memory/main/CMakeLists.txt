# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "test.c"
        "../../../components/gpsp/core/riscv/rv32_arm_flow_full.c"
        "../../../components/gpsp/core/riscv/rv32_arm_flow_alu.c"
        "../../../components/gpsp/core/riscv/rv32_arm_alu.c"
        "../../../components/gpsp/core/riscv/rv32_arm_mul.c"
        "../../../components/gpsp/core/riscv/rv32_flow_timing.c"
        "../../../components/gpsp/core/riscv/rv32_block_link.c"
        "../../../components/gpsp/core/riscv/rv32_fallback.c"
        "../../../components/gpsp/core/riscv/rv32_arm_memory.c"
        "../../../components/gpsp/core/riscv/rv32_arm_io.S"
        "../../../components/gpsp/core/riscv/rv32_arm_palette.S"
        "../../../components/gpsp/core/riscv/rv32_arm_branch.c"
        "../../../components/gpsp/core/riscv/rv32_codegen.c"
        "../../../components/gpsp/core/riscv/rv32_rom_load.S"
        "../../../components/gpsp/core/riscv/rv32_stub.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_exit_shared.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_memory.c"
    INCLUDE_DIRS
        "../../../components/gpsp/core/riscv"
        "../../../components/gpsp/core"
        "../../../components/gpsp/core/libretro/libretro-common/include"
    PRIV_REQUIRES
        esp_mm esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE INLINE=inline
    GPSP_RV32_TEST_HOOKS=1)

include(${CMAKE_CURRENT_LIST_DIR}/../../rv32-zb.cmake)
gpsp_rv32_zb_flags(${COMPONENT_LIB})

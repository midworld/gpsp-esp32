# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "test.c"
        "../../../components/gpsp/core/riscv/rv32_arm_alu.c"
        "../../../components/gpsp/core/riscv/rv32_arm_branch.c"
        "../../../components/gpsp/core/riscv/rv32_arm_mul.c"
        "../../../components/gpsp/core/riscv/rv32_block_emit.c"
        "../../../components/gpsp/core/riscv/rv32_block_link.c"
        "../../../components/gpsp/core/riscv/rv32_codegen.c"
        "../../../components/gpsp/core/riscv/rv32_fallback.c"
        "../../../components/gpsp/core/riscv/rv32_interpreter_stub.S"
        "../../../components/gpsp/core/riscv/rv32_rom_load.S"
        "../../../components/gpsp/core/riscv/rv32_stub.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_alu.c"
        "../../../components/gpsp/core/riscv/rv32_thumb_exit_shared.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_multi_ram.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_ram_helper.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_ram_store.S"
    INCLUDE_DIRS
        "../../../components/gpsp/core/riscv"
    PRIV_REQUIRES
        esp_mm esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_RV32_TEST_HOOKS=1)

include(${CMAKE_CURRENT_LIST_DIR}/../../rv32-zb.cmake)
gpsp_rv32_zb_flags(${COMPONENT_LIB})

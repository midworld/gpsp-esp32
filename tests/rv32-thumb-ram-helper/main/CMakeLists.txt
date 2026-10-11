# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "test.c"
        "test_block.S"
        "../../../components/gpsp/core/riscv/rv32_codegen.c"
        "../../../components/gpsp/core/riscv/rv32_rom_load.S"
        "../../../components/gpsp/core/riscv/rv32_stub.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_ram_helper.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_ram_store.S"
    INCLUDE_DIRS
        "../../../components/gpsp/core/riscv"
    PRIV_REQUIRES
        esp_mm esp_psram
)

include(${CMAKE_CURRENT_LIST_DIR}/../../rv32-zb.cmake)
gpsp_rv32_zb_flags(${COMPONENT_LIB})

# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "test.c"
        "../../../components/gpsp/core/riscv/rv32_codegen.c"
        "../../../components/gpsp/core/riscv/rv32_arm_mul.c"
        "../../../components/gpsp/core/riscv/rv32_stub.S"
    INCLUDE_DIRS
        "../../../components/gpsp/core/tests/rv32-arm-mul"
        "../../../components/gpsp/core/riscv"
    PRIV_REQUIRES esp_mm esp_psram
)

include(${CMAKE_CURRENT_LIST_DIR}/../../rv32-zb.cmake)
gpsp_rv32_zb_flags(${COMPONENT_LIB})

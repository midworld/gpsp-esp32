# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "test.c"
        "test_bl.c"
        "test_budget.c"
        "test_block.S"
        "../../../components/gpsp/core/riscv/rv32_block_link.c"
        "../../../components/gpsp/core/riscv/rv32_codegen.c"
        "../../../components/gpsp/core/riscv/rv32_flow_timing.c"
        "../../../components/gpsp/core/riscv/rv32_stub.S"
        "../../../components/gpsp/core/riscv/rv32_thumb_alu.c"
        "../../../components/gpsp/core/riscv/rv32_thumb_exit_shared.S"
    INCLUDE_DIRS
        "../../../components/gpsp/core/riscv"
    PRIV_REQUIRES
        esp_mm
)

# The firmware builds the stub with block-JIT budget guards; OFF tests the
# plain guard return instead.
option(GPSP_TEST_STUB_BUDGET "Test the block-JIT budget path" ON)
if(GPSP_TEST_STUB_BUDGET)
    set_source_files_properties(
        "../../../components/gpsp/core/riscv/rv32_stub.S"
        PROPERTIES COMPILE_DEFINITIONS GPSP_RV32_JIT=1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_STUB_BUDGET=1)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../rv32-zb.cmake)
gpsp_rv32_zb_flags(${COMPONENT_LIB})

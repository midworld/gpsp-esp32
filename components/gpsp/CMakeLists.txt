# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set(gpsp_core_sources
    "core/bios_data.S"
    "core/cheats.c"
    "core/cpu.cc"
    "core/gba_cc_lut.c"
    "core/gba_memory.c"
    "core/gba_rom_scan.c"
    "core/gbp.c"
    "core/input.c"
    "core/libretro/libretro.c"
    "core/libretro/libretro-common/compat/compat_posix_string.c"
    "core/libretro/libretro-common/compat/compat_strl.c"
    "core/libretro/libretro-common/compat/fopen_utf8.c"
    "core/libretro/libretro-common/encodings/encoding_utf.c"
    "core/libretro/libretro-common/file/file_path.c"
    "core/libretro/libretro-common/file/file_path_io.c"
    "core/libretro/libretro-common/streams/file_stream.c"
    "core/libretro/libretro-common/string/stdstring.c"
    "core/libretro/libretro-common/time/rtime.c"
    "core/libretro/libretro-common/vfs/vfs_implementation.c"
    "core/main.c"
    "core/memmap.c"
    "core/rfu.c"
    "core/savestate.c"
    "core/serial.c"
    "core/serial_proto.c"
    "core/sound.c"
    "core/video.cc"
    "core/riscv/rv32_arm_alu.c"
    "core/riscv/rv32_arm_branch.c"
    "core/riscv/rv32_arm_mul.c"
    "core/riscv/rv32_codegen.c"
    "core/riscv/rv32_fallback.c"
    "core/riscv/rv32_platform.c"
    "core/riscv/rv32_stub.S"
    "core/riscv/rv32_thumb_alu.c"
    "core/cpu_threaded.c"
    "core/riscv/rv32_arm_memory.c"
    "core/riscv/rv32_arm_io.S"
    "core/riscv/rv32_arm_palette.S"
    "core/riscv/rv32_arm_system.c"
    "core/riscv/rv32_block_emit.c"
    "core/riscv/rv32_block_link.c"
    "core/riscv/rv32_block_runtime.cc"
    "core/riscv/rv32_interpreter_stub.S"
    "core/riscv/rv32_ram_units.c"
    "core/riscv/rv32_rom_load.S"
    "core/riscv/rv32_thumb_memory.c"
    "core/riscv/rv32_thumb_multi_ram.S"
    "core/riscv/rv32_thumb_ram_helper.S"
    "core/riscv/rv32_thumb_ram_store.S"
    "core/riscv/rv32_flow_timing.c"
    "core/riscv/rv32_arm_flow_alu.c"
    "core/riscv/rv32_arm_flow_full.c"
    "core/riscv/rv32_thumb_flow_full.c"
    "core/riscv/rv32_thumb_exit_shared.S"
)

if(CONFIG_GPSP_BENCHMARK)
    list(APPEND gpsp_core_sources "core/gpsp_benchmark_timing.c")
endif()

if(CONFIG_GPSP_AFFINE_PIE)
    list(APPEND gpsp_core_sources "core/riscv/rv32_affine_pie.S")
endif()

idf_component_register(
    SRCS
        ${gpsp_core_sources}
    INCLUDE_DIRS
        "core/libretro/libretro-common/include"
        "core/include"
    PRIV_INCLUDE_DIRS
        "core"
        "core/libretro"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FRONTEND_SUPPORTS_RGB565
    HAVE_INTTYPES_H
    HAVE_STDINT_H
    HAVE_STRINGS_H
    INLINE=inline
    __LIBRETRO__
)

if(CONFIG_GPSP_BENCHMARK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK=1)
endif()

# RAM code diagnostics are shared with the benchmark frontend.
if(CONFIG_GPSP_BENCHMARK)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GPSP_BENCHMARK_RAM_CODE=1)
endif()

option(GPSP_TEST_RAM_SMC "Enable RV32 RAM SMC regression hooks" OFF)
if(GPSP_TEST_RAM_SMC)
    if(NOT CONFIG_ESPEMU OR
            NOT CONFIG_GPSP_BENCHMARK)
        message(FATAL_ERROR "GPSP_TEST_RAM_SMC requires an ESP-EMU block JIT benchmark")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_RV32_TEST_RAM_SMC=1)
endif()

# 0 disables sync faults; 1 fails once; 2 fails every later sync.
set(GPSP_TEST_SYNC_FAULT "0" CACHE STRING "RV32 RAM SMC sync fault mode")
set_property(CACHE GPSP_TEST_SYNC_FAULT PROPERTY STRINGS 0 1 2)
if(NOT GPSP_TEST_SYNC_FAULT MATCHES "^[012]$")
    message(FATAL_ERROR "GPSP_TEST_SYNC_FAULT must be 0, 1 or 2")
endif()
if(NOT GPSP_TEST_SYNC_FAULT STREQUAL "0")
    if(NOT GPSP_TEST_RAM_SMC)
        message(FATAL_ERROR "GPSP_TEST_SYNC_FAULT requires GPSP_TEST_RAM_SMC=ON")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GPSP_RV32_TEST_SYNC_FAULT=${GPSP_TEST_SYNC_FAULT})
endif()

if(CONFIG_GPSP_AFFINE_PIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_AFFINE_PIE=1)
endif()

if(CONFIG_GPSP_BENCHMARK_PHASE_WORK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK_PHASE_WORK=1)
endif()

if(CONFIG_GPSP_BENCHMARK_INSTRET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK_INSTRET=1)
endif()

if(CONFIG_GPSP_BENCHMARK_JIT_DETAIL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK_JIT_DETAIL=1)
endif()

if(CONFIG_GPSP_BENCHMARK_JIT_LOOKUP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK_JIT_LOOKUP=1)
endif()

if(CONFIG_GPSP_BENCHMARK_JIT_MISS_CLASS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GPSP_BENCHMARK_JIT_MISS_CLASS=1)
endif()

if(CONFIG_GPSP_BENCHMARK_JIT_CODE_BYTES)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GPSP_BENCHMARK_JIT_CODE_BYTES=1)
endif()

if(CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GPSP_BENCHMARK_VIDEO_DETAIL=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GPSP_RV32_JIT=1
    GPSP_RV32_ROM_CACHE_KIB=${CONFIG_GPSP_RV32_ROM_CACHE_KIB}
    GPSP_RV32_RAM_CACHE_KIB=${CONFIG_GPSP_RV32_RAM_CACHE_KIB}
    GPSP_RV32_CODE_CHUNK_KIB=${CONFIG_GPSP_RV32_CODE_CHUNK_KIB}
)

set(gpsp_rv32_extensions_default 0)
if(CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S31)
    set(gpsp_rv32_extensions_default 1)
endif()

if(CONFIG_IDF_TARGET_ESP32P4)
    set(gpsp_rv32_isa
        "rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbs_zcb")
    if(NOT CONFIG_ESPEMU)
        string(APPEND gpsp_rv32_isa "_xesploop")
    endif()
    if(CONFIG_GPSP_AFFINE_PIE)
        string(APPEND gpsp_rv32_isa "_xespv")
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE "-march=${gpsp_rv32_isa}")
endif()

# JIT instruction selection is independent of the compiler ISA.
foreach(ext ZBA ZBB ZBS RVC ZCB)
    set(GPSP_RV32_USE_${ext} "${gpsp_rv32_extensions_default}" CACHE STRING
        "Use ${ext} instructions in RV32 JIT code")
    set_property(CACHE GPSP_RV32_USE_${ext} PROPERTY STRINGS 0 1)
    if(NOT GPSP_RV32_USE_${ext} MATCHES "^[01]$")
        message(FATAL_ERROR "GPSP_RV32_USE_${ext} must be 0 or 1")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GPSP_RV32_USE_${ext}=${GPSP_RV32_USE_${ext}})
endforeach()

# Match the C++ settings used by upstream gpSP.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
)

# Keep known upstream warnings visible without failing ESP-IDF's -Werror build.
set_source_files_properties(
    "core/cheats.c"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-error=maybe-uninitialized"
)

set_source_files_properties(
    "core/rfu.c"
    PROPERTIES
        COMPILE_OPTIONS
            "-Wno-error=implicit-fallthrough;-Wno-error=missing-field-initializers"
)

set_source_files_properties(
    "core/serial_proto.c"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-error=empty-body"
)

# Use -O3 for the scanline renderer's hot loops.
set_source_files_properties(
    "core/video.cc"
    PROPERTIES
        COMPILE_OPTIONS "-O3"
)

# RV32 core

## Standalone build

- Interpreter: `make HAVE_DYNAREC=0` on the host.
- RV32 JIT: `make platform=rv32 CROSS_COMPILE=riscv32-unknown-elf-`.
- The RV32 target produces `libgpsp.a`. The frontend provides libretro
  callbacks, file access and executable-memory operations.
- Clean before changing build options; object names are shared between builds.

## Options

- Options accept environment variables or `make NAME=value` arguments.
  Command-line assignments take precedence. Boolean options accept `0` or `1`.
- `RV32_ARCH` and `RV32_ABI` select compiler/assembler output. Defaults:
  `rv32imac_zicsr_zifencei` and `ilp32`.
- `GPSP_RV32_USE_ZBA`, `GPSP_RV32_USE_ZBB`, `GPSP_RV32_USE_ZBS`,
  `GPSP_RV32_USE_RVC` and `GPSP_RV32_USE_ZCB` select
  generated-code features separately. Defaults are zero. Enable only features
  supported by the target; Zcb forms also require RVC, and some require Zbb.
- Block-cache sizes, in KiB: `GPSP_RV32_ROM_CACHE_KIB=2048`,
  `GPSP_RV32_RAM_CACHE_KIB=2048`, `GPSP_RV32_CODE_CHUNK_KIB=256`.
  Use positive cache sizes that are multiples of the chunk size. Chunks must
  be powers of two; each arena supports at most 64 chunks.
- `ROM_BUFFER_SIZE=32`: maximum ROM buffer in MiB, from 1 to 32.
- `GPSP_BENCHMARK=1`: timing counters. Optional detail switches:
  `GPSP_BENCHMARK_PHASE_WORK`, `GPSP_BENCHMARK_INSTRET`,
  `GPSP_BENCHMARK_JIT_DETAIL`, `GPSP_BENCHMARK_JIT_LOOKUP`,
  `GPSP_BENCHMARK_JIT_MISS_CLASS`, `GPSP_BENCHMARK_JIT_CODE_BYTES`,
  `GPSP_BENCHMARK_VIDEO_DETAIL`, `GPSP_BENCHMARK_RAM_CODE`.
  Detail switches require `GPSP_BENCHMARK=1`.
- `GPSP_AFFINE_PIE=1`: optional XespV affine helper. Requires a supporting
  assembler and CPU, plus frontend task/context preparation.

## Frontend contract

- `gpsp_set_rom_buffer_limit(mib)` sets a runtime ROM buffer limit before
  `retro_init()`. Values must be within 1..`ROM_BUFFER_SIZE`.
- Register `gpsp_rv32_platform_ops` before loading a game. Keep the operations
  alive and unchanged until the JIT caches have been released.
- Allocate executable memory with the requested alignment and free it through
  the same provider. Publish each written range before instruction fetch.
- Allocation failure can reduce the cache or select the interpreter fallback.
- RV32 paging and benchmark counters read cycle CSRs. The execution environment
  must allow these reads; instruction counters also need `instret` access.
- See [tests](../tests/README.md) for native and assembler checks.

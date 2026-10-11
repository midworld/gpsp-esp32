<!-- Copyright (C) 2026 Midworld Kim -->

# RV32 code chunk retry

- The build uses 64 KiB code chunks.
- Seven ARM fill blocks use part of the first RAM chunk.
- A large ARM block then reaches the chunk limit and is translated again in
  the next chunk.
  - The verifier checks that the first try had at least 32 KiB free and
    that the block needs more than that space minus 2 KiB. The older
    32 KiB reserve kept such a block in the chunk and flushed both caches.
- 96 Thumb copies fill later chunks. All must be translated; at least one
  must be retried.
- Pixel 0 checks the ARM result; pixel 1 checks every Thumb result.
  A red pixel stays red.
- The verifier reads frames until `BENCH_DONE` and one quiet second.
  More frames than configured fail. The build must stop after the report.
  - `BENCH_ENGINE` and `BENCH_JIT_CACHE` windows must cover all frames once.
  - No window may flush a code cache or stop the JIT, and no block lookup
    may fail. `chunk_retries` must cover the logged RAM retries.

Build and run on ESP32-P4 ESP-EMU:

```sh
# Source the ESP-IDF environment first.
python3 tests/rv32-chunk-retry/build_rom.py /tmp/chunk-retry.gba
GPSP_BUILD_ESPEMU=1 idf.py -B build-chunk-retry \
  -D SDKCONFIG=build-chunk-retry/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;tests/rv32-chunk-retry/sdkconfig.defaults' \
  -D GPSP_TEST_RAM_SMC=ON \
  -D GPSP_ROM_PATH=/tmp/chunk-retry.gba build merge-bin
python3 tests/rv32-chunk-retry/verify_emu.py \
  --build-dir build-chunk-retry --emu /path/to/esp-emu \
  --log /tmp/chunk-retry.log
```

- `build_rom.py` needs `clang`, `lld` and `llvm-objcopy`; set `ARM_CLANG`,
  `LLD` and `LLVM_OBJCOPY` to override them.
- Add `-D GPSP_RV32_USE_RVC=1` to check compressed code.

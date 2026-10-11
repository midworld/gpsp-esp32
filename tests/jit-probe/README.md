<!-- Copyright (C) 2026 Midworld Kim -->

# ESP32 JIT memory probe

This independent ESP-IDF firmware checks whether generated RISC-V code can run
from allocated memory on ESP32-P4 or ESP32-S31, in ESP-EMU or on a physical
board. It does not load a ROM or modify gpSP's firmware. The cache sizes are
reference values from gpSP's `gpsp_config.h` as of September 23, 2026; update
them if the planned dynarec cache sizes change.

With memory protection disabled, the firmware:

- Allocates 256 bytes of internal `MALLOC_CAP_EXEC` RAM and executes a generated
  two-instruction function there.
- Allocates two PSRAM caches at the same time: 2 MiB ROM + 384 KiB RAM, then
  10 MiB ROM + 512 KiB RAM.
- Allocates one contiguous 10.5 MiB PSRAM block for the larger ROM and RAM
  caches and checks both sides of their shared boundary.
- Writes and executes code at the start, middle, and last aligned 256-byte
  region of each cache. Each call must turn 41 into 42; patching the code at
  the same address must then turn 41 into 43. The firmware uses
  `esp_cache_msync()` for data writeback and instruction invalidation, followed
  by `fence.i`, on the calling core.
- Reports the largest free PSRAM block, the largest `EXEC|SPIRAM` block, and a
  PASS/FAIL result, then releases the test allocations.

`EXEC|SPIRAM largest=0` reports the heap's allocation capabilities. A
successful PSRAM function call in ESP-EMU is a separate observation; the
firmware does not set an executable flag on an allocation.

## ESP-EMU run

Activate ESP-IDF v6.1, then run from this directory. The default target is
ESP32-P4:

```sh
idf.py build merge-bin
./run-emu.sh build/merged-binary.bin PASS
```

For ESP32-S31, select the preview target and rebuild:

```sh
idf.py --preview set-target esp32s31
idf.py --preview build merge-bin
./run-emu.sh build/merged-binary.bin PASS esp32s31
```

Use `idf.py set-target esp32p4` to switch back. The shared
`sdkconfig.defaults` and per-target `sdkconfig.defaults.*` files hold the
emulator baseline settings for each chip.

The default `sdkconfig.defaults` disables `CONFIG_ESP_SYSTEM_MEMPROT` for this
experiment. The runner waits for a completion marker after the result, checks
the full PASS result, and exits with an error for FAIL, UNAVAILABLE, or a
timeout. ESP-EMU itself exits with code 0 on a timeout, so its exit code alone
cannot verify this test.

For a separate build with memory protection enabled:

```sh
idf.py -B build-protected -D SDKCONFIG="$PWD/sdkconfig.protected" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.protected.defaults build merge-bin
./run-emu.sh build-protected/merged-binary.bin UNAVAILABLE
```

Under ESP-IDF v6.1 with this setting, `MALLOC_CAP_EXEC` is not defined and the
firmware reports `UNAVAILABLE`. This checks the API/configuration gate; it does
not branch into protected writable memory to provoke a fault.

On September 23, 2026, ESP-EMU v0.43.0 with ESP-IDF v6.1 and 32 MiB PSRAM
reported PASS for ESP32-P4 internal allocation, both cache pairs, and the
contiguous cache. The P4 protected build reported UNAVAILABLE. The same
positive checks passed on ESP32-S31 in ESP-EMU on September 24, 2026.

## Physical board

For an ESP32-P4 or ESP32-S31 board, select its target as above. Check the
resulting configuration against the board's flash and PSRAM wiring and
capacity, then build and flash this project from this directory:

```sh
idf.py build
idf.py -p PORT flash monitor
```

Record the individual results and the final `JIT PROBE RESULT` line. A `PASS`
means that one run exercised every allocation, execution location, and code
patch listed above on that board and configuration. An allocation failure can
instead mean the board lacks enough free contiguous PSRAM for the requested
cache size. The `run-emu.sh` script is for ESP-EMU only.

ESP32-S31 needs a separate build from P4. Both emulator configurations have
passed, but neither target has been validated on a physical board.

These results establish the observed emulator behavior for an otherwise idle
firmware. They do not establish how much contiguous memory remains after gpSP
loads a ROM, physical board cache behavior, dynarec correctness or speed, or
cross-core code publication. Repeat the execution and patch test on the final
board before relying on executable PSRAM.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

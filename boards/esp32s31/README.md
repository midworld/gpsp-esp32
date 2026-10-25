# ESP32-S31

- Experimental physical-board build for core profiling.
- Defaults: CPU 320 MHz, octal PSRAM 200 MHz, flash 16 MiB.
- Console: UART0, 115200 baud.
- No display, audio output, network, SD card, or gamepad support.
- The core still renders frames and generates audio samples.
- Embed a local ROM in the flash storage partition.
- Requires ESP-IDF v6.1 with preview targets enabled.
- Physical check, 2026-10-24: 16 MiB PSRAM, memory test passed, and
  5,120 Metroid Fusion frames completed with PSRAM JIT code.

## Build

```sh
IDF_COMPONENT_MANAGER=0 idf.py --preview -B build-s31 \
  -D GPSP_BOARD=esp32s31 -D SDKCONFIG=build-s31/sdkconfig \
  -D GPSP_ROM_PATH=/path/to/game.gba build
IDF_COMPONENT_MANAGER=0 idf.py --preview -B build-s31 -p PORT flash
```

- Append `sdkconfig.benchmark.defaults`, `sdkconfig.jit.defaults`, and local
  measurement settings to `SDKCONFIG_DEFAULTS` for a benchmark build.
- Use `tools/bench-p4.py` to reset, capture, and validate a physical S31 run.
- The benchmark stops at `BENCH_DONE` when the stop option is enabled.

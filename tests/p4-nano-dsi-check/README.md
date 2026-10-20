# DSI check

- Requires ESP-IDF 6.1 and a supported DSI board, or ESP-EMU in model mode.
- Checks game 3x, UI 2x, landscape rotation, black borders, partial UI
  updates, frame hand-off, shared I2C ownership, and shutdown/restart.
- Model mode also checks recovery after a panel initialization failure.
- Checks render-buffer recovery across game switches and held menu frames.
- Checks exact nearest pixels, grid placement, color-profile samples,
  unchanged UI colors, menu visibility, left/right cycling, and settings
  save/reload with an in-memory config store.
- Checks Stats Off/Simple/Full, legacy `on` settings, the FPS/speed-only
  top-right box, unchanged pixels outside it, immediate switching, and UI exclusion.
- Checks virtual pad game/UI bounds, pressed states, border cleanup after Off,
  static-menu refresh, and setting persistence. Touch contacts are synthetic;
  physical touch needs a separate check. Pad timing excludes initial cache creation
  and reports p50/p95/p99/max for steady and changing button states.
- Runs display checks on CPU1; UI retains PPA processing.
- Previous OSPTEK P4 v3.2 hardware checks passed with PIE enabled:
  exact nearest/grid pixels,
  GBA color samples, UI 2x pixels, menu/settings, and shutdown/restart.
- OSPTEK checks use PSRAM at 250 MHz and a 256 KiB L2 cache. PIE shading
  checks all 65,536 RGB565 colors, including rounded grid corners.
- DMA2D checks compare complete output frames and time direct PSRAM
  expansion against SRAM tiles. Timings exclude game execution and STAT.
- Model DMA checks delay and reorder completion, detect source reuse,
  and inject submission failures with queued transfers.
- OSPTEK game profiling verified native pixels and filter output checksums
  for the tile queue, fused grid, and CPU rotation paths.
  See [Asterix measurements](../../docs/benchmark.md#asterix-state-0).
- Model timings do not represent hardware performance.
- Bilinear checks inspect solid-color interiors. Panel appearance and
  sustained frame rate need separate checks.
- ESP-EMU 0.43.0 did not complete the native PPA probe: the first request
  entered the panic handler from `dma2d_connect`.
- Enabling PIE on ESP-EMU 0.43.0 stops at `esp.orq` with an illegal
  instruction exception. Model defaults disable PIE and PSRAM XIP.
- Model mode replaces display/PPA calls inside the test firmware only.
  It checks CPU effects, layout, and lifecycle, not PPA bilinear filtering,
  DSI timing, panel compatibility, or hardware frame rate. The model uses
  nearest sampling; bilinear checks inspect solid-color interiors only.

## OSPTEK hardware

```sh
cd tests/p4-nano-dsi-check
IDF_COMPONENT_MANAGER=0 idf.py -B build-osptek \
  -D GPSP_BOARD=osptek_p4c5 -D SDKCONFIG=build-osptek/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.osptek.defaults' build
idf.py -B build-osptek -p <OSPTEK-port> flash monitor
```

## Peripheral model

```sh
cd tests/p4-nano-dsi-check
IDF_COMPONENT_MANAGER=0 idf.py -B build-model \
  -D GPSP_BOARD=osptek_p4c5 -D SDKCONFIG=build-model/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.osptek.defaults;sdkconfig.model.defaults' \
  build merge-bin
esp-emu --chip esp32p4 --firmware build-model/merged-binary.bin \
  --psram-size 32M --timeout 120s --exit-on 'DSI_CHECK PASS'
```

- Success requires `DSI_CHECK PASS` in the output; a timeout is a failure.
- For the native peripheral probe, omit `sdkconfig.model.defaults` and
  use a separate build directory and sdkconfig.
- `gpSP LCD` selects the panel and opposite rotation.
- Model defaults use 8-row tiles, DMA2D, and CPU rotation. Select 12/16 rows
  or disable `CONFIG_GPSP_LCD_DSI_CPU_ROTATE` for variant checks.
  PIE remains disabled in model mode.

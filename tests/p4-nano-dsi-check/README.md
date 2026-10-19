# DSI check

- Requires ESP-IDF 6.1 and a supported DSI board, or ESP-EMU in model mode.
- Checks game 3x, UI 2x, landscape rotation, black borders, partial UI
  updates, frame hand-off, shared I2C ownership, and shutdown/restart.
- Model mode also checks recovery after a panel initialization failure.
- Checks exact nearest pixels, grid placement, color-profile samples,
  unchanged UI colors, menu visibility, left/right cycling, and settings
  save/reload with an in-memory config store.
- Runs checks on CPU1, including source-sized PPA rotation and UI processing.
- OSPTEK P4 v3.2 hardware passes with PIE enabled: exact nearest/grid pixels,
  GBA color samples, UI 2x pixels, menu/settings, and shutdown/restart.
- Bilinear checks inspect solid-color interiors. Panel appearance and
  sustained frame rate need separate checks.
- ESP-EMU 0.43.0 did not complete the native PPA probe: the first request
  entered the panic handler from `dma2d_connect`.
- Enabling PIE on ESP-EMU 0.43.0 stops at `esp.orq` with an illegal
  instruction exception. Model defaults disable PIE.
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
  -D SDKCONFIG=build-model/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.model.defaults' \
  build merge-bin
esp-emu --chip esp32p4 --firmware build-model/merged-binary.bin \
  --psram-size 32M --timeout 120s --exit-on 'DSI_CHECK PASS'
```

- Success requires `DSI_CHECK PASS` in the output; a timeout is a failure.
- For the native peripheral probe, omit `sdkconfig.model.defaults` and
  use a separate build directory and sdkconfig.
- `gpSP LCD` selects the panel and opposite rotation.

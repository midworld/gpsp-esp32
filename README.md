# gpSP ESP32

- Game Boy Advance emulator for ESP32, based on
  [gpSP](https://github.com/midworld/gpsp) with an RV32 JIT.
- P4 Nano: LCD, speaker, USB gamepads, SD card games and save states,
  and Wi-Fi file transfer.

![In-game menu and game list](docs/images/menu.png)

## Build

- Requires ESP-IDF v6.1.
- LCD, audio, and USB gamepad support are enabled by default on P4 Nano.
- Put `.gba` files in `roms/` on a FAT-formatted SD card.

```sh
idf.py -B build-p4 -D IDF_TARGET=esp32p4 \
  -D SDKCONFIG=build-p4/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.p4_nano.defaults' \
  menuconfig
idf.py -B build-p4 build
idf.py -B build-p4 -p PORT flash monitor
```

- Set `CONFIG_*` options in `menuconfig`; pass `GPSP_RV32_USE_*` as
  CMake flags, for example `-D GPSP_RV32_USE_ZCB=0`.

| Flag | Purpose | Default |
| --- | --- | --- |
| `CONFIG_GPSP_LCD` | LCD output | On for P4 Nano |
| `CONFIG_GPSP_MENU` | In-game menu | On when LCD is enabled, except in benchmark builds |
| `CONFIG_GPSP_AUDIO` | Speaker output | On for P4 Nano |
| `CONFIG_GPSP_USB_PAD` | USB gamepad input | On for P4 Nano, except in benchmark builds |
| `CONFIG_GPSP_CARTRIDGE_AUTOSAVE` | Cartridge save loading and periodic saving | Off |
| `GPSP_RV32_USE_RVC`, `GPSP_RV32_USE_ZBA`, `GPSP_RV32_USE_ZBB`, `GPSP_RV32_USE_ZBS`, `GPSP_RV32_USE_ZCB` | JIT instruction extensions, individually selectable | All `1` on P4 and S31 |
| `CONFIG_GPSP_AFFINE_PIE` | XespV affine rendering | On for physical P4; off for virtual builds |
| `CONFIG_GPSP_RV32_ROM_CACHE_KIB`, `CONFIG_GPSP_RV32_RAM_CACHE_KIB` | ROM/RAM JIT cache limits | 2048 KiB each |
| `CONFIG_GPSP_RV32_CODE_CHUNK_KIB` | JIT cache allocation chunk | 256 KiB |
| `CONFIG_GPSP_ROM_BUFFER_SIZE` | ROM data buffer limit, in MiB | `0`: 4 MiB for PSRAM ≤16 MiB; 8 MiB above 16 MiB |
| `CONFIG_GPSP_BENCHMARK` | Fixed-frame benchmark logging | Off |

## Performance

- P4 Nano at 400 MHz, 32 MiB PSRAM at 200 MHz,
  LCD and speaker on, Wi-Fi and frameskip off.
- JIT cache: 256 KiB chunks and 2 MiB limits each for ROM/RAM;
  ROM data buffer: 8 MiB.

| Game | Mean frame (ms) | Worst frame (ms) | FPS | One-core load | ROM cache (KiB) | RAM cache (KiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| DOOM II | 13.07 | 23.98 | 59.73 | 78.1% | 293.3 / 512 | 674.2 / 768 |
| Asterix & Obelix XXL | 12.26 | 20.47 | 60.02 | 73.6% | 461.1 / 512 | 1295.7 / 1536 |
| Metroid Fusion | 13.17 | 14.23 | 59.78 | 78.7% | 63.9 / 256 | 34.5 / 256 |
| Mario Kart: Super Circuit | 10.50 | 11.53 | 59.84 | 62.8% | 74.1 / 256 | 11.8 / 256 |

- Frame time excludes pacing waits; FPS includes them. Cache values are
  JIT code **used / allocated**.

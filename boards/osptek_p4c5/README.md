# OSPTEK P4C5

- ESP32-P4 v3.x, 16 MiB flash, 32 MiB PSRAM, ESP32-C5.
- CPU clock: 400 MHz; PSRAM clock: 250 MHz.
- Speaker: 300 Hz second-order Butterworth high-pass at 32,768 Hz.
  `CONFIG_GPSP_AUDIO_HIGHPASS_HZ` sets the cutoff: 0 (off) or 100 to
  1000 Hz.
- Light measurement is the default; LCD Stats retains all indicators.
- `CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS` enables frame checksums, periodic
  JIT/status logs and slow-frame details. DSI phase timing also requires
  `CONFIG_GPSP_LCD_DSI_DIAGNOSTICS`.
- USB health summaries remain enabled, every 5 seconds while active.
- Carrier reference: V1.3; panel: YDP430BT009-V1, ST7102.
- `board.c`: wiring and available hardware.
- `sdkconfig.defaults`: default panel and enabled services.
- Display: native 480x800, landscape 800x480; game 3x, UI 2x.
- Nearest and LCD Grid use two SRAM tiles and DMA2D, with PIE scaling
  and fused grid shading. A tile waits only before reuse.
- `CONFIG_GPSP_LCD_DSI_TILE_8` is the default. Tile choices:

  | Source rows | Tile SRAM total | Copies per frame |
  | --- | --- | --- |
  | 8 | 45 KiB | 30 |
  | 12 | 67.5 KiB | 20 |
  | 16 | 90 KiB | 15 |

- `CONFIG_GPSP_LCD_DSI_CPU_ROTATE` replaces integer-mode PPA rotation
  with CPU1 PIE transposes. OSPTEK enables it with 8-row tiles by default.
  It reuses tile storage; bilinear and UI retain PPA.
- Asterix state 0 measurements: [integer scaling](../../docs/benchmark.md#asterix-state-0).
- ST7123 touch: shared I2C, address 0x55, CPU1 polling every 8 ms.
- Virtual Pad: Off/On in Settings; double-tap the screen while Off to enable.
  D-pad, A/B, L/R, Start, Select, and Menu also operate the existing menus.
  Physical control mappings do not change the touch button functions.
- Controls use cached PSRAM tiles and PPA blending before Stats.
  Pad Opacity: 25% (default), 50%, or 100%, applied by PPA.
  One tile per control uses 0.5 MiB on first use; a press changes only
  the fill color of its part.
  Menus blend above the unchanged controls. Cached menu layers use 1.76 MiB of PSRAM.
  The touch worker uses a 3 KiB stack; there are no new SRAM framebuffers.
  DSI diagnostics include blend and cache-build times under `PAD profile`.
- Camera input is not implemented.
- Ethernet is disabled; Wi-Fi uses the existing ESP-Hosted C5 firmware.
- `CONFIG_GPSP_USB_BT` enables an external USB HCI host on demand.
  The Bluedroid host has model checks only; pairing and input are not yet
  verified on this board.

| Function | P4 GPIO |
| --- | --- |
| I2C SDA / SCL | 7 / 8 |
| ES8311 MCLK / BCLK / WS / DOUT | 13 / 12 / 10 / 9 |
| Speaker enable | 53 |
| DSI reset / backlight enable | 5 / 20 |
| SD D0-D3 / CLK / CMD | 39-42 / 43 / 44 |
| C5 SDIO D0-D3 / CLK / CMD / reset | 14-17 / 18 / 19 / 54 |

- SD card power is supplied by the carrier's 3.3 V rail.
- SD writes use single-block commands; multi-block writes time out now and then.
- A timed-out SD read or write resets the controller, initializes the card
  again, and runs once more. If that fails, the card is unavailable until a
  restart.
- A failed Game List ROM switch retries after a restart.
- Use the UART connector with the serial switch set to P4 for flashing.
- For SD builds, set `-D GPSP_ROM_PATH=` to omit the flash ROM image.
- Use `app-flash` for code updates; include `bootloader-flash` when needed.
- The USB-A host shares a hardware USB mux with the USB data Type-C port.
  Leave the data Type-C port disconnected when using a gamepad.
- Board and display selection are separate. Choose the connected DSI
  model in `menuconfig` if the bundled panel is replaced.
- Hardware validation: [DSI pixel and menu checks](../../tests/p4-nano-dsi-check/README.md).

## References

- [Carrier and schematic](https://github.com/osptek/esp32-p4c5-module-dev-board).
- [Core board and C5 wiring](https://github.com/osptek/esp32-p4c5-core-board).
- [Panel initialization and timing](https://github.com/osptek/tft-4.3-480x800-mipi-st7102/tree/601868ee4a83271281689be94cb4164c1270076c/versions/YDP430BT009-V1).

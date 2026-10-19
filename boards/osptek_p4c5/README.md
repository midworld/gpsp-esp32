# OSPTEK P4C5

- ESP32-P4 v3.x, 16 MiB flash, 32 MiB PSRAM, ESP32-C5.
- PSRAM clock: 200 MHz.
- USB and DSI diagnostic summaries: every 5 seconds while active.
- Carrier reference: V1.3; panel: YDP430BT009-V1, ST7102.
- `board.c`: wiring and available hardware.
- `sdkconfig.defaults`: default panel and enabled services.
- Display: native 480x800, landscape 800x480; game 3x, UI 2x.
- Touch and camera input are not implemented.
- Ethernet is disabled; Wi-Fi uses the existing ESP-Hosted C5 firmware.

| Function | P4 GPIO |
| --- | --- |
| I2C SDA / SCL | 7 / 8 |
| ES8311 MCLK / BCLK / WS / DOUT | 13 / 12 / 10 / 9 |
| Speaker enable | 53 |
| DSI reset / backlight enable | 5 / 20 |
| SD D0-D3 / CLK / CMD | 39-42 / 43 / 44 |
| C5 SDIO D0-D3 / CLK / CMD / reset | 14-17 / 18 / 19 / 54 |

- SD card power is supplied by the carrier's 3.3 V rail.
- Use the UART connector with the serial switch set to P4 for flashing.
- The USB-A host shares a hardware USB mux with the USB data Type-C port.
  Leave the data Type-C port disconnected when using a gamepad.
- Board and display selection are separate. Choose the connected DSI
  model in `menuconfig` if the bundled panel is replaced.
- Hardware validation: [DSI pixel and menu checks](../../tests/p4-nano-dsi-check/README.md).

## References

- [Carrier and schematic](https://github.com/osptek/esp32-p4c5-module-dev-board).
- [Core board and C5 wiring](https://github.com/osptek/esp32-p4c5-core-board).
- [Panel initialization and timing](https://github.com/osptek/tft-4.3-480x800-mipi-st7102/tree/601868ee4a83271281689be94cb4164c1270076c/versions/YDP430BT009-V1).

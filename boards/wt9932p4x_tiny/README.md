# WT9932P4X-TINY

- ESP32-P4 v3.x, 16 MiB flash, 32 MiB PSRAM. No radio.
- CPU clock: 400 MHz; PSRAM clock: 250 MHz; L2 cache 256 KiB.
- No display, audio codec, or USB pad in the defaults. The game runs headless.
- The menu needs a display, so it is off.
- Console: USB Serial/JTAG on the USB-C debug port. Baud rate does not apply.
- Serial input, ROM names, and screenshots use the same port.
- The board has no SD slot. Wire a slot to the slot 0 pins below.
- SD I/O power: on-chip LDO channel 4.

| Function | P4 GPIO |
| --- | --- |
| SD D0-D3 / CLK / CMD | 39-42 / 43 / 44 |
| I2C SDA / SCL (DSI, touch, camera) | 7 / 8 |
| DSI reset / backlight | 27 / 26 |
| Boot button | 35 |
| RGB LED (WS2812) | 51 |

## Build

```sh
idf.py -B build-wt9932p4x -D GPSP_BOARD=wt9932p4x_tiny \
  -D SDKCONFIG=build-wt9932p4x/sdkconfig -D GPSP_ROM_PATH= build
idf.py -B build-wt9932p4x -p <port> flash
```

- Choose the ROM with `rom=<file name>` in `gpsp.cfg`, or send the file name
  and a newline within 2 seconds of `ROM name?`.

## Notes

- The bundled DSI panel (ST7102, 480x640) is not supported.
- ESP-Hosted is linked and starts at boot without a radio. It is unused
  while `CONFIG_GPSP_SERVER_HOST` is empty.
- A screenshot pauses the game while it prints: 0.15-0.46 s per
  240x160 PNG (9-44 KB).
- UHS-I SD modes failed on a wired slot; the default HS (40 MHz) works.
  See [board check](../../tests/p4-nano-board-check/README.md).

## References

- [Board support package](https://github.com/Wireless-TAG-Maker/WT_BSP)

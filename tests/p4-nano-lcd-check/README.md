<!-- Copyright (C) 2026 Midworld Kim -->

# P4 Nano SPI LCD check

- Board: ESP32-P4-NANO-WIFI6-DB, revision v3.1; ESP-IDF v6.1.
- Panel: GMT020-06PS / ST7789, 240x320, 4-wire SPI.
- Wiring: VDD=3.3V, GND=GND, SCL=GPIO20, SDA=GPIO21,
  RES=GPIO25, DC=GPIO26, CS=GPIO5, BL=GPIO32.
- Connect the adapter with USB power removed.

## Run

```sh
cd tests/p4-nano-lcd-check
idf.py build
idf.py -p PORT flash monitor
```

- Use the USB-C programming port for `PORT`. Exit the monitor with `Ctrl+]`.
- The test replaces the running app. It uses the project partition layout
  and does not flash ROM or save data. Run `idf.py app-flash` from the project
  root to restore the main app after testing.
- No SD card, ROM, network, or external display library is needed.

## Output

- Landscape: 320x240, black background, centered 240x160 test area at (40,40).
- Red, green, blue, white, black, yellow, cyan, and magenta bars, left to right.
- A moving square, fine checkerboard, and frame-number bits expose artifacts.
- The displayed label shows the requested SPI clock.
- Two 76,800-byte DMA buffers use internal SRAM. One is prepared while the
  other is transmitted; reuse waits for the DMA completion callback.
- 10, 20, and 40 MHz run for 10 seconds each, then repeat.
- `menuconfig` > `P4 Nano LCD check`: duration, RGB/BGR, inversion, rotation.

## Results

- `LCD_FPS`: completed frame transfers per second.
- `LCD_RESULT`: average FPS, whether it reaches 60, submit-to-DMA-completion
  time (average/minimum/maximum), and average pattern preparation time.
- Pixel-only limits for 240x160 RGB565: 16.28 / 32.55 / 65.10 FPS.
- Commands, scheduling, rendering, and logging reduce the measured rate.
- This is a standalone transfer test. It does not measure game performance
  or the panel scan rate. Double buffering does not synchronize panel scans.
- Inspect the screen at each clock: write-only SPI cannot detect panel-side
  corruption automatically. Check stable colors, borders, and fine detail.
- For full 320x240 updates, the pixel-only limit at 40 MHz is 32.55 FPS.

## Board result (2026-10-02)

- P4 revision v3.1, centered 240x160 RGB565, two internal DMA buffers.
- Each speed ran for 10 seconds. The user reported normal pattern output.
- Captured logs had no DMA timeout or panic.

| SPI clock | Completed transfers/s | Average submit-to-completion |
| --- | ---: | ---: |
| 10 MHz | 16.24 | 61.573 ms |
| 20 MHz | 32.41 | 30.847 ms |
| 40 MHz | 64.54 | 15.485 ms |

- Pattern preparation averaged 0.308 ms and overlapped DMA transfer.
- 40 MHz exceeded 60 transfers/s in this standalone test.
- Console: UART0 at 115200 baud through the board USB-UART port.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

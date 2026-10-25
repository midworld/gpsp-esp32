<!-- Copyright (C) 2026 Midworld Kim -->

# P4 board check

- Boards: ESP32-P4-NANO-WIFI6-DB (default) and WT9932P4X-TINY.
- Checks the P4X chip, 32 MiB PSRAM, 16 MiB flash, and the SD card.
- The Nano build also checks the ESP32-C5 radio.

## Results

### ESP32-P4-NANO-WIFI6-DB

- Chip: ESP32-P4 revision v3.1 (P4X generation).
- PSRAM: 32 MiB; 1 MiB read/write PASS (earlier test version).
- Flash: 16 MiB; JEDEC ID read PASS.
- Radio: ESP32-C5 over SDIO; Wi-Fi scan PASS with an antenna.
- iperf3 TCP: RX 31.4 Mb/s, TX 21.0 Mb/s; 10 seconds in each direction.

### WT9932P4X-TINY

- Chip: ESP32-P4 revision v3.2. No radio.
- Flash: 16 MiB, JEDEC ID `0x464018`.
- PSRAM: 32 MiB; 31 MiB pattern test PASS at 200 and 250 MHz.

| PSRAM clock | Write | Verify |
| --- | --- | --- |
| 200 MHz | 83 MiB/s | 89 MiB/s |
| 250 MHz | 94 MiB/s | 93 MiB/s |

- SD: the board has no SD slot. An external slot was wired to the slot 0 pins.
- 64 GB card, 32 MiB FAT file:

| Mode | Clock | Write | Read |
| --- | --- | --- | --- |
| DS | 20 MHz | 6.8-7.0 MB/s | 8.9-9.0 MB/s |
| HS | 40 MHz | 6.7-11.1 MB/s | 16.1-16.2 MB/s |
| SDR50, DDR50, SDR104 | - | FAIL | FAIL |

- UHS-I modes time out at `all_send_cid` after the 1.8 V switch.

## Run

- Use ESP-IDF v6.1: `idf.py build`, then `idf.py -p PORT flash monitor`.
- `PORT` is the USB-C programming port. Press `Ctrl+]` to exit the monitor.
- Scan and speed tests are off by default. Attach the antenna and enable them in `idf.py menuconfig`.
- Speed test: enter Wi-Fi SSID, password, and server address; run `iperf3 -s -p 5201` on the server.

WT9932P4X-TINY, with PSRAM at 250 MHz:

```sh
idf.py -B build-wt9932p4x -D SDKCONFIG=build-wt9932p4x/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wt9932p4x.defaults;sdkconfig.psram250.defaults' build
idf.py -B build-wt9932p4x -p PORT flash monitor
```

- Omit `sdkconfig.psram250.defaults` for 200 MHz.
- Add `sdkconfig.psram250.defaults` to the Nano build for 250 MHz.

## PSRAM test

- Writes and verifies the largest free PSRAM block (31 MiB) with two
  address-dependent patterns.
- Reports the configured clock and write/verify throughput.
- The block is larger than the cache, so verify reads come from PSRAM.

## SD test

- Uses SDMMC slot 0, 4-bit, internal pull-ups, and on-chip LDO channel 4.
- Runs DS (20 MHz), HS (40 MHz), then one UHS-I mode (default SDR104).
- Each mode writes, reads back, verifies, and deletes `SDTEST.BIN`.
  An existing `SDTEST.BIN` is preserved and the test fails. Other files
  on the card are not changed.
- Times only `write()`, `fsync()`, and `read()`. Results are FAT file throughput.
- Needs a FAT32 card with free space. exFAT is not supported.
- A mode that the card lacks is reported as SKIP.
- `menuconfig` > `P4 Nano board check`: test size, LDO channel, UHS-I mode.
- A card switched to 1.8 V keeps that voltage until power is removed.

## Host checks

- Run `python3 tests/p4-nano-board-check/run-sd-tests.py` from the repository root.
- Checks existing-file preservation, data verification, failed I/O,
  mode fallback, and cleanup with a host model. It does not measure SD hardware.

## Notes

- Check the `ESP32-P4NRW32X` and `ESP32-C5-MINI-1U` markings by eye.
- Git ignores `sdkconfig` and `build/`. Speed-test firmware contains Wi-Fi credentials.
- ESP-Hosted starts at boot in every build and owns the SDMMC controller.
  The SD test shares it; IDF logs a refused controller delete on unmount.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

## Third-party sources

- [iperf3](https://github.com/esnet/iperf), tag 3.15,
  commit `917d2f02188f6f4cdc443df7923a4bde72017d92`.
  See [LICENSE](components/iperf3/LICENSE).
- Local changes cover control-cookie entropy, stream buffers, socket
  options, and POSIX compatibility. `CMakeLists.txt`, `iperf_config.h`,
  and `version.h` are local build files.
- TCP transfer loops adapt `iperf/iperf.c` from
  [espressif/iperf-cmd](https://github.com/espressif/iperf-cmd), tag v1.0.2,
  commit `bfaa6540b17bb0573ba15dbbbccf35bd1f633731`.
  `esp_iperf_io.c` and `.h` retain Espressif's notices;
  `iperf_tcp.c` handles counters and partial sends, and `iperf_api.c`
  initializes the buffer. See [Apache 2.0 license](components/iperf3/LICENSE.espressif).

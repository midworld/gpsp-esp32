<!-- Copyright (C) 2026 Midworld Kim -->

# ESP32-P4-NANO-WIFI6-DB board check

## Result on this board

- Chip: ESP32-P4 revision v3.1 (P4X generation).
- PSRAM: 32 MiB; 1 MiB read/write PASS.
- Flash: 16 MiB; JEDEC ID read PASS.
- Radio: ESP32-C5 over SDIO; Wi-Fi scan PASS with an antenna.
- iperf3 TCP: RX 31.4 Mb/s, TX 21.0 Mb/s; 10 seconds in each direction.

## Run

- Use ESP-IDF v6.1: `idf.py build`, then `idf.py -p PORT flash monitor`.
- `PORT` is the USB-C programming port. Press `Ctrl+]` to exit the monitor.
- Scan and speed tests are off by default. Attach the antenna and enable them in `idf.py menuconfig`.
- Speed test: enter Wi-Fi SSID, password, and server address; run `iperf3 -s -p 5201` on the server.

## Notes

- Check the `ESP32-P4NRW32X` and `ESP32-C5-MINI-1U` markings by eye.
- Git ignores `sdkconfig` and `build/`. Speed-test firmware contains Wi-Fi credentials.

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

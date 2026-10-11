# Board lifecycle test

- Run `python3 tests/gpsp-board/run.py` from the repository root.
- Requires Python 3 and a host C compiler; set `HOST_CC` and `CFLAGS` as needed.
- Compiles the board backend with mock peripheral services.
- Checks fatal initialization failure, retry, optional peripheral failure,
  serial shutdown order, and final menu Wi-Fi cleanup.
- Checks automatic ROM limits around 16 MiB PSRAM and a fixed build override.
- Uses GPL-3.0-or-later; see [COPYING](../../COPYING).

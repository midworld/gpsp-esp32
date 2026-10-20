# Board lifecycle test

- Run `python3 tests/gpsp-board/run.py` from the repository root.
- Requires Python 3 and a host C compiler; set `HOST_CC` and `CFLAGS` as needed.
- Compiles the board backend with mock peripheral services.
- Checks fatal initialization failure, retry, optional peripheral failure,
  serial shutdown order, and final menu Wi-Fi cleanup.
- Checks automatic ROM limits around 16 MiB PSRAM and a fixed build override.
- Compiles the production video callbacks and LCD queue operations with a
  controlled worker. Checks buffer exhaustion, dropped frames, recovery,
  skipped frames, and 32 game unloads in normal and benchmark builds.
- Video ownership checks use AddressSanitizer and UndefinedBehaviorSanitizer.
- ESP-EMU menu: the unchanged menu picture goes again to a new or
  reconnected viewer and after a failed send, and stops when the menu closes.
- Uses GPL-3.0-or-later; see [COPYING](../../COPYING).

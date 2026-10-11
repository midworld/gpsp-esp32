<!-- Copyright (C) 2026 Midworld Kim -->

# gpSP Host Viewer

- Shows video from a P4 Nano or ESP-EMU.
- Sends keyboard and game controller input. F5 saves and F8 loads full state.
- Runs on macOS, Linux, and Windows.
- Uses GPL-3.0-or-later; see [COPYING](COPYING).

## Build

- Requires CMake and SDL2.

- macOS:

```sh
brew install cmake sdl2
```

- Debian or Ubuntu:

```sh
sudo apt install cmake libsdl2-dev
```

- Windows with vcpkg:

```powershell
vcpkg install sdl2:x64-windows
cmake -S tools/espemu-host -B build/espemu-host `
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/espemu-host --config Release
```

- Build on macOS or Linux:

```sh
cmake -S tools/espemu-host -B build/espemu-host
cmake --build build/espemu-host
```

## Connect

- ESP-EMU: the guest reaches the host at `192.168.4.1:24680`. The viewer
  listens on `127.0.0.1:24680`.
- Terminal 1:

```sh
build/espemu-host/gpsp-espemu-host
```

- Terminal 2:

```sh
esp-emu --chip esp32p4 --firmware build-espemu/merged-binary.bin \
  --psram-size 32M --net user --save-state
```

- P4 Nano: bind to the host's LAN address. Set `CONFIG_GPSP_SERVER_HOST` to
  the same address in the firmware's local `sdkconfig`.

```sh
build/espemu-host/gpsp-espemu-host HOST_IP 24680
```

- ESP-EMU keeps F5 states across runs only with `--save-state`. This option
  writes flash changes back to the merged firmware image.

## Serial Input

- P4 Nano with an LCD: send buttons over the USB serial port instead of the
  network. No video is sent; the window only takes keyboard focus.

```sh
build/espemu-host/gpsp-espemu-host --serial /dev/cu.usbmodemXXXX \
  --rom "Game.gba"
```

- `--rom NAME` resets the board and answers the boot ROM prompt.
  `--reset` resets without a name. With neither, the running game keeps
  going.
- After a reset, input starts when the board logs `Serial input ready`.
- The device log prints in the terminal. The title shows the input bits,
  the F5/F8 result, and the speaker volume. F2 prints the firmware's task
  table.
- The window has four buttons: volume down, volume up, home, and close
  game. Keys do the same: `-`, `=` (or keypad `-`/`+`), Q, and F10.
  - Home presses the pad's home button: it opens the menu.
  - Volume moves in 3 dB steps between -60 and 0 dB until the next reset.
  - Close game writes the cartridge save (with autosave), unloads the
    game, and turns the LCD and speaker off. `--rom` or a reset starts
    again.
- The firmware needs `CONFIG_GPSP_SERIAL_INPUT=y` (default, off in benchmark
  builds). Buttons release 0.5 seconds after the last message.
- Close other serial monitors first. macOS and Linux only for now.

## Profile

- The viewer also accepts a CSV path:

```sh
build/espemu-host/gpsp-espemu-host 127.0.0.1 24680 profile.csv
```

- Graph lanes: frame execution (red), pacing wait (blue), frame copy
  (yellow), and TCP send (green).
- The title and `FPS` overlay show executed frames per second from the
  device's profile counts.
- CSV rows include timing, frame counts, drops, and guest timestamps.
- TCP send time measures socket calls, not display delivery.

## Controls

| Control | Keyboard |
| --- | --- |
| D-pad | Arrow keys |
| A | X |
| B | Z |
| L | A |
| R | S |
| Start | Return |
| Select | Backspace |
| Pad X / Y (turbo A / B by default) | V / C |
| Pad L2 / R2 (R2: fast forward by default) | D / F |
| Save full state | F5 |
| Load full state | F8 |
| Volume down / up (serial) | `-` / `=` |
| Home (serial) | Q |
| Close game (serial) | F10 |
| Quit | Escape |

- The title reports F5/F8 success or failure. F8 never runs at boot.
- Game controllers use the D-pad, A/B, shoulders, Start, and Back.
- Input repeats every 100 ms. Lost connections release held buttons.
- Normal runs replace a waiting frame when the host falls behind. Benchmark
  runs use a larger frame queue.

# Host input checks

- Requires SDL2 2.0.18 and a C compiler.
- Uses a virtual controller; no board or serial port is opened.
- Checks RetroPad A/B positions, Home command edges, held buttons across
  connection changes, and controller removal.

```sh
mkdir -p output/host-input
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  $(pkg-config --cflags sdl2) -I protocol -I tools/espemu-host/src \
  tests/host-input/test_input.c tools/espemu-host/src/net.c \
  tools/espemu-host/src/serial.c $(pkg-config --libs sdl2) \
  -o output/host-input/check
SDL_VIDEODRIVER=dummy output/host-input/check
```

- With sdl2-compat on macOS, set `DYLD_LIBRARY_PATH` to the installed
  SDL3 library directory if it is outside the library search path.

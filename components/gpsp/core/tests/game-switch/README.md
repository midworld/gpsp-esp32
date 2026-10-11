# Game Switch Tests

- Run `make test` in this directory.
- Builds the core for the host with the interpreter.
- Add `SANITIZE=1` for AddressSanitizer and UndefinedBehaviorSanitizer.
- Each case loads game B in a new process, then game A followed by B in
  another. Both runs of B must match in:
  - the first pixel of each of its first two frames, which must arrive;
  - the converted palette, ROM word 0xC4 and pending serial cycles.
- Cases:
  - palette: A sets a palette color that B never writes.
  - RTC then mini ROM: A has an RTC; B is a 1MB mirrored ROM.
  - serial transfer: A leaves a slow transfer pending; B starts a fast one.
  - sprite list: A shows objects; B turns objects on without writing OAM.
  - BG2 reference point: A moves BG2; B draws in mode 3.
- The ROMs are synthetic and written to `build/roms`. Hooks write guest
  memory where a ROM program would.

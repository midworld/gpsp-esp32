# Display pixel checks

- Run from the repository root; requires a C compiler.
- Checks 8/12/16-row tiles and full-frame fallback, both rotations,
  nearest/grid, color lookup, unaligned input, overlap, and bounds.
- Uses the portable path. PIE is checked by the DSI hardware test.

```sh
mkdir -p output/display-test
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/gpsp_display/include \
  components/gpsp_display/gpsp_display.c \
  tests/gpsp-display/display_test.c -lm -o output/display-test/check
output/display-test/check
```

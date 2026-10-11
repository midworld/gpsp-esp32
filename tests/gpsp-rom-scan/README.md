# ROM Signature Tests

- Run `make test` in this directory.
- Uses the production scanner with mocked file reads and allocations.
- Covers all backup signatures, resident and file boundaries, exact EOF,
  short reads, allocation fallback, allocation failure, seek failure,
  read failure, and premature EOF.
- The `0xFF79CC` case is synthetic; it contains no game data.
- Sanitizers: `make clean test HOST_CFLAGS='-O1 -g -fsanitize=address,undefined'`.

# Console checks

- Run `python3 tests/gpsp-console/run.py` from the repository root.
- Compiles the production serial input and console line reader with host drivers.
- Checks UART/USB selection, ROM bounds and timeouts, commands, startup failures,
  driver cleanup, and input reset after restart.
- Uses AddressSanitizer and UndefinedBehaviorSanitizer. No board is accessed.

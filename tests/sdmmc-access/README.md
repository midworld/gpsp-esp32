# SD access checks

- Run `python3 tests/sdmmc-access/run.py` with a host C compiler.
- Checks read retries, single-block write fallback, and buffer offsets.
- Checks failed recovery and a second timeout disable further card access.
- Checks both single-block defaults and disk status/control results.
- Uses production access functions with mock card I/O and recovery.
- Uses address and undefined-behavior sanitizers; no card or board is opened.
- Controller registers, interrupts, and concurrent SDIO need hardware checks.

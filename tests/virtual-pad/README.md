# Virtual pad checks

- Run `python3 tests/virtual-pad/run.py` with a host C compiler.
- Checks double-tap timing, movement, multi-finger rejection, and time wrap.
- Checks fixed logical buttons, diagonals, A/B sliding, group capture,
  multiple contacts, cancellation, and release before re-entry.
- Checks ST7123 report bounds, seven-byte contact stride, and both rotations.
- Uses address and undefined-behavior sanitizers.
- Native PPA bounds and border cleanup: `tests/p4-nano-dsi-check`.

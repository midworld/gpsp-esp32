<!-- Copyright (C) 2026 Midworld Kim -->

# ARM to RV32 smoke test

## Scope

- Executes output from `rv32_arm_alu.c` through the fixed-register RV32 stub on ESP-EMU.
- Checks 69 single-instruction cases and two six-instruction blocks.
- Runs the cases the flow ALU emitter supports through it as well.
- Covers carry, borrow, flags, shifts, conditions, PC operands, register flow, PC updates, and test-controlled cycles.
- Shares 15 expected states with `core/tests/arm_alu_interpreter.cc`; other cases use hand-checked ARM7 results.
- Does not run gpSP's block scanner, memory map, or scheduler.

## Run

- Activate ESP-IDF v6.1. From this directory:

```sh
idf.py build merge-bin
./run-emu.sh
```

- Expect `ARM RV32 SMOKE RESULT: PASS`.
- Zba/Zbb/Zbs flags default to 1, as in the P4 firmware; turn one off with `-D GPSP_RV32_USE_ZBB=0`.
- `GPSP_RV32_USE_RVC` defaults to 1; disable it with `-D GPSP_RV32_USE_RVC=0`.
- `GPSP_RV32_USE_ZCB` defaults to 1, as in the P4 firmware; Zcb forms appear only with RVC on.
- With Zbb enabled, BIC and ROR cases execute `andn`, `ror`, and `rori`.
- `../rv32-zb-matrix.sh` runs the JIT tests with each extension set.
- This test disables system memory protection and uses executable internal RAM with cache synchronization.
- An ESP-EMU pass does not prove executable memory works on a physical board.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

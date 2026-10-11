<!-- Copyright (C) 2026 Midworld Kim -->

# ARM multiply to RV32M test

## Scope

- Tests `MUL`, `MLA`, `UMULL`, `UMLAL`, `SMULL`, and `SMLAL` from `rv32_arm_mul.c`.
- Host fixtures compare 27 directed cases and 256 condition/flag cases with gpSP's one-step interpreter and a 64-bit arithmetic reference.
- ESP-EMU executes generated code and checks products, accumulate carry, N/Z, aliases, emitted sizes, failed conditions, and atomic buffer failure.
- The harness advances guest PC and cycles; gpSP's scanner and cycle model are not connected.

## Run

- From the repository root, run the host fixture:

```sh
make -C components/gpsp/core/tests check-cpu
```

- Activate ESP-IDF v6.1. From this directory, run ESP-EMU:

```sh
idf.py build merge-bin
./run-emu.sh
```

- This test disables system memory protection. Physical-board executable memory remains untested.
- ARM7 leaves some operand overlaps unpredictable; the emitter follows gpSP's interpreter for them.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

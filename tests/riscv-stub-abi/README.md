<!-- Copyright (C) 2026 Midworld Kim -->

# RV32 block stub ABI test

## Scope

- Runs a static RV32 block through `gpsp_rv32_run_block`.
- Checks the guest register map, PC, NZCV merge, cycles left, guard continuation,
  and preserved host registers.
- Checks Thumb fetch alignment on cache hits and raw targets on misses or
  exhausted budgets.
- Checks all 16 x 16 NZCV unpack/pack pairs and a pending lazy ADD at exit.
- Builds the stub with block-JIT budget guards, as in the firmware. Checks the
  lean `update_gba` path and the C update path, resuming or reloading state.
  Checks changed NZCV and pending lazy ADD flags before each update path.
  `-D GPSP_TEST_STUB_BUDGET=OFF` tests the plain guard return instead.
- Checks that idle matching uses the raw branch target.
- Checks that the ARM PC and system exits read CPSR.T into the cache key.
- Runs a generated BL second half through a cache hit into a fused BL pair.
  Checks LR, PC, flags, and cycles for near and far targets.
- Uses executable RAM for the BL test; does not use gpSP's block scanner.

## Run

- Activate ESP-IDF v6.1. From this directory:

```sh
idf.py build merge-bin
./run-emu.sh
```

- Expect `RV32 STUB ABI RESULT: PASS`.
- See `../arm-rv32-smoke` for a generated ARM opcode test using this stub.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

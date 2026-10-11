<!-- Copyright (C) 2026 Midworld Kim -->

# rv32-thumb-multi-emitter

## Scope

- Execute generated Thumb blocks in ESP-EMU: ALU, memory, branches and helpers.
- Check BIC, register ROR with C live or dead, immediate SUBS with only V live,
  and C from register and immediate LSL/LSR/ASR.
- Check results, preserved flags, registers and cycle counts.

## Run

- `idf.py build merge-bin && ./run-emu.sh`
- Zba, Zbb and Zbs default to 1. Override with `idf.py -D GPSP_RV32_USE_ZBB=0 build merge-bin`.
- RVC defaults to 1. Override with `-D GPSP_RV32_USE_RVC=0`.
- Zcb defaults to 1. Override with `-D GPSP_RV32_USE_ZCB=0`.
- Run the extension combinations with `../rv32-zb-matrix.sh emu`.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

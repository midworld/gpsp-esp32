<!-- Copyright (C) 2026 Midworld Kim -->

# ESP32 ISA probe

- Standalone ESP-IDF firmware; no ROM required
- Executable RAM tests, selected flash comparisons, and exception logs
- Selected RV32I/M/A/F/C, Zcb/Zcmp/Zcmt, Zba/Zbb/Zbs, XespLoop and XespV instructions

## Run

- ESP-IDF v6.1; run from this directory
- P4: `idf.py set-target esp32p4`
- S31: `idf.py --preview set-target esp32s31`
- Emulator PIE mode: `cmake -S . -B build -DISA_PROBE_PIE_CASE=1`
- Build: `idf.py build merge-bin` (S31: `idf.py --preview build merge-bin`)
- Execute: `esp-emu --chip CHIP --firmware build/merged-binary.bin --exit-on 'ISA PROBE COMPLETE' --timeout 20s` (`CHIP`: `esp32p4` or `esp32s31`)
- Board: check the chip revision (P4 default build requires v3.1 or later); set mode `0` with `cmake -S . -B build -DISA_PROBE_PIE_CASE=0`, then `idf.py -p PORT flash monitor` (S31: add `--preview`)
- Reset with the monitor connected to capture the full run
- Console: UART0 primary, USB Serial/JTAG CDC secondary when that board port is connected
- PIE modes (`ISA_PROBE_PIE_CASE`):
  - `0`: all non-breakpoint cases; default for physical boards
  - `1`: vector and memory cases
  - `2`: flash `esp.addx2` alone
  - `3`–`6`: RAM `esp.addx2`, `addx4`, `subx2`, `subx4`, one per build
- Mode change: rerun the `cmake` command with the new number, then rebuild
- All modes skip `ebreak`/`c.ebreak`

## Interpret

- `PASS`: expected result; `UNAVAILABLE`: trapped or resource unavailable; `FAIL`: wrong result or setup failure
- `BLOCKED`: prerequisite unavailable; `SKIPPED`: case not run
- `PUSH_REVERSED`: tested Zcmp frame has `ra`/`s1` slots swapped
- `ISA PROBE COMPLETE`: run finished; does not mean all tests passed
- Zcmt prerequisite: 64-byte-aligned jump table base in `jvt` (CSR `0x017`)
- PIE prerequisite: guarded CSR check, then task claim through IDF's lazy context path; S31 PIE on CPU1
- Emulator CPU fault: process stops without a guest trap result

## ESP-IDF v6.1 / ESP-EMU v0.43.0 · 2026-09-24

- P4 v3.1 / S31 v0.0, CPU0 RAM: RV32I 45/45, M 12/12, C integer 28/28, A 3/3, F 4/4
- Zcb 23/23, Zcmp 6/6, Zba 3/3, Zbb 25/25, Zbs 12/12
- Zcmt: `jvt` readback zero; `cm.jt 0` traps in RAM and flash; three remaining forms blocked
- XespLoop: `setup`, `starti`, `count` pass in RAM; `setupi`, `endi`, `counti` trap in RAM and flash
- XespV mode 1: 128-bit load/store copy passes from RAM and flash; six SIMD arithmetic/logic instructions trap from RAM; `xorq`/`vadd.u8` also trap from flash
- Bare `xorq`: RAM and flash both trap with PIE state `DIRTY` (`3`) after context claim
- XespV modes 2–6: emulator CPU fault at each scalar instruction
- P4 v1.x: matching firmware did not reach the app in ESP-EMU v0.43.0; no ISA result

## ESP-IDF v6.1 / P4 v3.1 board · 2026-10-07

- PIE mode 0
- RV32I 45/45, M 12/12, C integer 28/28, A 3/3, F 4/4; Zcb 23/23, Zba 3/3, Zbb 25/25, Zbs 12/12
- Zcmp `{ra,s0-s1}`: `PUSH_REVERSED`; pop forms pass with the reversed frame; matched push/pop and register moves pass
- Zcmt: RAM 4/4; flash `cm.jt 0` passes
- XespLoop: RAM 6/6, flash 3/3; an old loop end matching the instruction after `esp.lp.endi` still loops
- XespV: smoke 8/8, RAM 10/10, scalar flash 1/1

## References

- [RISC-V Zc](https://docs.riscv.org/reference/isa/v20260120/unpriv/zc.html) · [RISC-V Zb](https://docs.riscv.org/reference/isa/v20260120/unpriv/b-st-ext.html)
- [ESP32-P4 TRM](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf) · [Espressif PIE instruction table](https://github.com/espressif/esp-dl/blob/master/tools/agents/skills/esp32p4-pie-simd/references/instructions.md)
- [ESP-EMU distribution](https://github.com/espressif/esp-emulator)

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

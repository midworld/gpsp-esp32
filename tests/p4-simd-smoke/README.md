<!-- Copyright (C) 2026 Midworld Kim -->

# ESP32-P4 ISA test

- Board: Waveshare ESP32-P4-NANO-WIFI6-DB, chip revision v3.1; ESP-IDF v6.1.
- Run: `idf.py build`, then `idf.py -p PORT flash monitor`.

## Instruction results

- PIE SIMD: 14/14 pass; Zca: 28/28; Zba/Zbb/Zbs: 3/3, 25/25, 12/12; Zcb: 23/23; Zcmt: 4/4 RAM cases and flash `cm.jt 0` pass.
- Mixed 16/32-bit code: 2/2 pass from flash and RAM.
- `c.addi` vs `addi`: 16 adds take 18 cycles/loop each in flash and RAM; bodies are 32/64 bytes. Empty loop: 3 cycles. 100,000 loops, best of nine.
- Zcmp: 5/9 specification probes pass. For `{ra,s0-s1}`, the RISC-V stack order is `ra,s0,s1`; this chip stores `s1,s0,ra` at increasing addresses.
- Matching `cm.push`/`cm.pop` pairs restore registers. Tested `cm.popret` and `cm.popretz` cases pass, but no fix is documented. Keep ESP-IDF's Zcmp workarounds.
- Save/restore: manual 10 cycles and 16 code bytes; Zcmp 11 cycles and 4 code bytes. Zcmp cut code size and ran one cycle slower in this test.
- 16-byte SIMD kernels: copy 4 cycles versus scalar 27; XOR/AND/OR 7 versus 41; unsigned byte saturating add and max/min 7 versus 198–212. These include loads and stores; scalar C uses `-O2` and volatile accesses.

## Memory results

- Setup: 400 MHz CPU, 128 KiB L2, 32 MiB PSRAM at 200 MHz. Best of five runs; repeated transfers process at least 4 MiB. Read uses volatile 32-bit loads; write/copy use `memset`/`memcpy`.
- One buffer: PSRAM matches SRAM through 128 KiB (read 469, write 717 MiB/s). At 144 KiB, read falls to 118 MiB/s; at 160 KiB, write falls to 86 MiB/s. An evicted first read is about 118 MiB/s.
- Two-buffer copy: PSRAM is 402 MiB/s at 64 KiB each, 128 at 80 KiB each, and 57 at 128 KiB each. SRAM is 402 MiB/s at 128 KiB each.

## License

- Test code uses GPL-3.0-or-later; see [COPYING](COPYING).
- Reused gpSP sources and third-party files keep their original licenses.

<!-- Copyright (C) 2026 Midworld Kim -->

# RV32 RAM cache regression

- The ROM copies Thumb and ARM blocks to IWRAM.
- The first call links the source block to a target that returns 1.
- A generated `STRH` changes the tagged target to return 2.
- The next call must return 2. Green pixel 0 passes; red fails.
- A second pass patches a block containing only IWRAM offset 0.
- After its flush, green pixel 1 confirms the stale tag was cleared.
- Green pixel 2 checks an independent entry inside a translated Thumb block.
- Green pixel 3 checks a mirrored interior entry's PC value.
- Green pixel 4 checks that a write clears an interior entry alias.
- Green pixel 5 checks an ARM interior entry overlapping Thumb code and its
  invalidation after a write to the upper halfword.
- Green pixels 6 and 7 check a ROM Thumb memory region and entry after a load.
- Green pixel 8 checks ARM immediates changed by later writes and by the
  block itself, and a B moved to another target.
- Green pixel 9 checks a write to a word covered by 65 RAM units and a
  dormant unit that runs again when its bytes come back. The verifier also
  requires a test log showing at least 65 live units over that write.
- Green pixel 10 checks one STM that changes code at the end of EWRAM and,
  through the mirror, at its start. Both calls must change their results.
- Green pixel 11 checks a learned switch slot after a flag write that ends
  the previous flow chunk; the slot then becomes a B over the next CMP.
  - Markers bound the calls after learning. The head is translated once,
    while the slot is B; the next three slot writes must patch it.
- Green pixel 12 uses a separate block with 32 slot candidates and three B.
  - The verifier checks that learned slots plus the three B exceed the
    reported exit limit, and that a block under that pressure splits
    without exceeding the limit. Early splits may reserve exit space.
  - Too few learned slots fail the coverage check; a failed translation
    stops the JIT.
  - Slot writes make no call while the slot is B, so each new slot
    forces a fresh translation of the head.
- Green pixel 13 checks 32 NOP/B cycles in a scanned tail after BX.
  - The test uses IWRAM head `0x03006000` and slot `0x03006014`.
  - Marker calls bound the test and its warmup in generic RAM event logs.
  - The verifier allows one or two translations and any restore count.
    RAM code must not grow after the first NOP/B cycle, including when
    no translation or restore occurs in the remaining cycles.
  - Both switch tests require unchanged cumulative RAM flush counts.
    A flush cannot hide allocations by resetting the code pointer.
  - The slot is learned once; all 64 writes patch the same native code.
    This also checks the effect of replacing a pre-learning NOP unit.
- Green pixels 14 (ARM) and 15 (Thumb) check self-write cuts.
  - Each function alternates two code templates over 16 calls.
  - The ARM case pretranslates both bodies to check rejection of uncut units.
  - After four warmup calls, the writer must not be translated or revived.
    RAM code usage and flush counts must stay unchanged.
- Green pixel 16 checks dynamic immediates.
  - Two ops at `0x03006600` (`MOV`, `AND`) get a new imm8 by `STRB` in each
    of 16 calls. Odd calls enter through the mirror `0x0300e600`.
  - After eight warmup calls, the next 16 immediate writes must cause no
    SMC handling, dirty ranges, translation, restore, patch, flush or
    code growth.
    Logs record handled SMC alerts and coalesced dirty ranges.
  - ROM code at `0x08006600` has the same low address bits and must keep
    its own immediate.
  - Then `AND` becomes `EOR` by `STR`; the result must follow it.
- Green pixel 17 checks slots translated as B. The slot table is full, so
  both functions reuse addresses learned by pixel 12.
  - Slot A is B at the first translation. After slot A becomes NOP and
    slot B becomes B, the result needs the flags of the CMP before slot A.
  - A backward B to its own unit is translated once; markers bound four
    NOP/B writes, and each must patch the slot.
- The ROM then calls the patched IWRAM block through the warm window; the
  verifier requires IWRAM JIT dispatches.
- Each received frame after startup must keep all 18 pixels green.
- Host dirty-range checks: run `make -C components/gpsp/core/tests/rv32-ram-dirty test`.

Build and run on ESP32-P4 ESP-EMU:

```sh
# Source the ESP-IDF environment first.
python3 tests/rv32-ram-smc/build_rom.py /tmp/ram-smc.gba
GPSP_BUILD_ESPEMU=1 idf.py -B build-ram-smc \
  -D SDKCONFIG=build-ram-smc/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;tests/rv32-ram-smc/sdkconfig.defaults' \
  -D GPSP_TEST_RAM_SMC=ON \
  -D GPSP_ROM_PATH=/tmp/ram-smc.gba build merge-bin
python3 tests/rv32-ram-smc/verify_emu.py \
  --build-dir build-ram-smc --emu /path/to/esp-emu \
  --log /tmp/ram-smc.log
```

- The verifier also accepts `--chip esp32s31` and `--psram-size 16M`.
  Build the matching target before running it.

- Fault builds require `-D GPSP_TEST_RAM_SMC=ON`. Configure with
  `-D GPSP_TEST_SYNC_FAULT=1` (one failed sync
  in the first restore) or `=2` (all later syncs fail), and run the
  verifier with `--expect-fault once` or `always`.
  - `once`: the JIT recovers and keeps running.
  - `always`: the JIT gives way to the interpreter.
  - `0` disables faults. Other values are rejected.

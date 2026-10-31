<!-- Copyright (C) 2026 Midworld Kim -->

# Benchmark

## Configure

- Set `CONFIG_GPSP_BENCHMARK=y`.
- Set `CONFIG_GPSP_BENCHMARK_FRAMES` to the number of frames to measure.
  The default is 5120; the allowed range is 64–5120.
- Set `CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT=y` to stop at that frame.
  Otherwise the game continues and reports later 64-frame windows.
- Enable the JIT, phase-work, and video-detail options needed for the run.
  Use the same options when comparing builds.
- Use a fresh build directory when changing defaults. An existing
  `sdkconfig` overrides `sdkconfig.defaults`.
- Set `CONFIG_GPSP_BENCHMARK_FRESH_SAVE=y` for a fresh cartridge save.
- Set `CONFIG_GPSP_BENCHMARK_HOLD_BUTTONS` to hold RetroPad buttons on
  every frame, e.g. `0x40` for left. They combine with live input;
  the default `0x0` adds no buttons.
- The scanline renderer (`components/gpsp/core/video.cc`) is built with
  `-O3` in the ESP-IDF project.
- `CONFIG_GPSP_AFFINE_PIE` enables PIE address generation in rotated
  affine tile backgrounds without mosaic. Pixel reads remain scalar.
  It defaults to on for physical P4 and off for S31.

## Run in ESP-EMU

- Build with `sdkconfig.benchmark.defaults` and
  `sdkconfig.espemu.defaults` in a fresh build directory. Set
  `GPSP_ROM_PATH` to the ROM file and `GPSP_BUILD_ESPEMU=1` in the shell.
- Run `idf.py -B BUILD_DIR build merge-bin`.
- Run `python3 tools/bench-espemu.py --build-dir NAME=BUILD_DIR`.
  The runner reads the frame count from the built configuration.
- Find each 64-frame window in `slices.csv` and all detail records in
  `records.json` under the run directory.
- Repeated runs must have the same frame checksum for each window.
- The ESP-EMU runner checks that every measured frame reached its receiver.

## Run on a P4 Nano

- Keep LCD and speaker output enabled for physical-device measurements.
- Use an ESP-IDF v6.1 shell, a P4 Nano, and a local ROM. Replace the `ROM` and
  `PORT` assignments with your ROM path and the board's serial port. Use a new
  build directory for each configuration.
- Copy the ROM to `roms/` on the SD card. Set `rom=<file name>` in the card's
  `gpsp.cfg`, preserving any other settings. Use the same file for `--rom`.
  Changing games does not require rebuilding or flashing the firmware.
- This example measures 5120 frames with the flow JIT, 256 KiB code cache
  chunks, 2 MiB limits, guest-clock pacing, and JIT, phase, and video detail.
  The empty receiver address gives neutral input and no network frame stream.

```sh
ROM=/absolute/path/to/game.gba
PORT=/dev/cu.usbmodem...
BUILD=build-bench-p4
DEFAULTS="sdkconfig.defaults;sdkconfig.p4_nano.defaults"
DEFAULTS="$DEFAULTS;sdkconfig.benchmark.defaults;sdkconfig.jit.defaults;$BUILD.defaults"
cat > "$BUILD.defaults" <<'EOF'
CONFIG_GPSP_SD_CARD=y
CONFIG_GPSP_SD_ROM_WAIT_MS=0
CONFIG_GPSP_LCD=y
CONFIG_GPSP_AUDIO=y
CONFIG_GPSP_RV32_CODE_CHUNK_KIB=256
CONFIG_GPSP_RV32_ROM_CACHE_KIB=2048
CONFIG_GPSP_RV32_RAM_CACHE_KIB=2048
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y
CONFIG_GPSP_BENCHMARK_FRAMES=5120
CONFIG_GPSP_BENCHMARK_FRESH_SAVE=y
CONFIG_GPSP_BENCHMARK_JIT_DETAIL=y
CONFIG_GPSP_BENCHMARK_JIT_LOOKUP=y
CONFIG_GPSP_BENCHMARK_JIT_MISS_CLASS=y
CONFIG_GPSP_BENCHMARK_JIT_CODE_BYTES=y
CONFIG_GPSP_BENCHMARK_PHASE_WORK=y
CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL=y
CONFIG_GPSP_SERVER_HOST=""
EOF
idf.py -B "$BUILD" -D IDF_TARGET=esp32p4 \
  -D SDKCONFIG="$BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="$DEFAULTS" \
  -D GPSP_ROM_PATH= build
idf.py -B "$BUILD" -p "$PORT" flash
python3 tools/bench-p4.py --build-dir "$BUILD" --port "$PORT" \
  --rom-source sd --rom "$ROM" --output-dir profiles/p4-first
```

- The empty `GPSP_ROM_PATH` skips the flash ROM image. Use `flash` for the
  initial setup and `app-flash` for later app changes with the same partitions.
  Keep `CONFIG_FATFS_VOLUME_COUNT=3` from `sdkconfig.defaults` for SD support.
- For cache-flush tests, append `sdkconfig.benchmark-cache64.defaults` to
  `SDKCONFIG_DEFAULTS`. It uses 64 KiB chunks and 512 KiB limits for both caches.
- `--rom-source sd` rejects a fallback to the flash ROM. The runner checks
  the loaded size when reported and records the local `--rom` file's hash.
  Flash ROM runs still work with `GPSP_ROM_PATH="$ROM"` and `--rom-source flash`.
- The runner resets the board, stops at `BENCH_DONE`, and checks every window,
  thumbnail row, checksum format, and configured detail record. It saves the
  raw `firmware.log` even when capture times out or validation fails.
- `result.json` contains the loaded ROM path and source (`sd` or `flash`),
  firmware, ROM, and generated config header hashes,
  measurement conditions, and the warm average for frames 65 onward.
  `slices.csv` and `records.json` contain all windows and detail records.
  Keep these files with the build's `sdkconfig`. Hashes identify local files;
  the runner cannot verify which image was flashed or which ROM is on the board.
- Repeat with a new output directory. To compare against the first run:

```sh
python3 tools/bench-p4.py --build-dir "$BUILD" --port "$PORT" \
  --rom-source sd --rom "$ROM" --output-dir profiles/p4-second \
  --compare-with profiles/p4-first
```

- `comparison.json` lists windows with different checksums and the change in
  warm `run` time in ms/frame; a positive change is slower. The runner requires
  the same ROM and source, frame count, clock, pacing, receiver, held buttons,
  status, and probe settings.
  For a different build, use its build directory. A 5120-frame run has 80
  windows.
- To recheck a saved serial log without resetting the board, replace `--port`
  with `--log-file path/to/firmware.log` and use a new output directory. Pass
  the build directory and ROM from that capture.

## Repeat a later interval

- Set `CONFIG_GPSP_BENCHMARK_CAPTURE_STATE_FRAME=N` and stop after frame N.
  Run ESP-EMU on a copy of the merged image with `--save-state` and
  `--exit-on "BENCH_DONE frames=N"`. Check `BENCH_STATE_CAPTURED frame=N`.
- Build the replay with `CONFIG_GPSP_BENCHMARK_LOAD_STATE=y`. Copy only the
  storage partition from the captured image into a copy of the replay image.
  Check `BENCH_STATE_LOADED` and compare frame checksums.
- A save state excludes generated JIT code. Treat the first replay window as
  cold cache; warm up before comparing later windows. Keep the full run as a
  reference for cache history.

## Read the log

- The first window is frames 1–64. Each later window covers 64 frames.
  If the configured count is not a multiple of 64, the final window is shorter.
- `BENCH_SLICE` contains `retro_run()` time, its maximum, core time,
  pacing wait, video output counts, and the final frame checksum.
- `BENCH_CPU_SLICE` and `BENCH_WORK_SLICE` contain CPU phase timing and work.
- `BENCH_VIDEO_SLICE` and `BENCH_VIDEO_LINES_SLICE` contain scanline detail.
- `BENCH_JIT_*` records contain JIT counters for the same `first` and `last`
  window when their options are enabled. `BENCH_JIT_BIOS_TOP` and
  `BENCH_JIT_FALLBACK_TOP` show totals through `last`.
- `BENCH_JIT_CACHE` shows ROM/RAM code use at both window boundaries,
  capacity, flushes, and discarded bytes. ROM discarded bytes exclude pinned
  BIOS code.
  - `chunk_retries`: moves to the next chunk after a block reached the chunk
    limit. ROM and RAM caches combined.
  - `chunk_tail_bytes`: chunk space left behind at every move to the next
    chunk, including space used by a failed try. ROM and RAM combined.
- `BENCH_JIT_PHASE` includes engine updates. Lookup time includes translation;
  `BENCH_JIT_DIAG` measures classification inside those times.
- `BENCH_JIT_TRANSLATE_PHASE` `emit_cycles` excludes a try that failed while
  emitting, such as one retried in the next chunk. Lookup time includes it.
- Block time ends when generated code returns. Region block cycles and sampled
  hot PCs assign a linked chain to the PC where `run_block` entered.
- `BENCH_JIT_LOOKUP_TOP` includes lazy side-entry creation in hit time and
  reports that creation time separately.
- `BENCH_JIT_SIDE_COST` splits side creation into page lookup, code emission,
  and pending-link publication. The pending sum is per window; its maximum is
  a cumulative high-water mark.
- `BENCH_JIT_ALIAS` reports active side metadata; `BENCH_JIT_SLOW_POOL` reports
  native slow-body reuse and table memory. Its saved bytes exclude table memory.
- `BENCH_JIT_TRANSLATE code_bytes` excludes lazy side adapters. Their bytes
  appear in `BENCH_JIT_ALIAS`; cache use also includes block headers and BIOS.
- `BENCH_JIT_HOT` samples one in 64 block dispatches and ranks entry PCs by
  sampled inclusive block cycles.
- `BENCH_IMAGE` contains a 30×20 RGB565 thumbnail of each window's last
  frame, one row per line.
- When a P4 receiver is configured, it gets frames 64 and the configured
  final frame.
- Records are printed after the configured measurement ends, so serial output
  does not interrupt measured frames. `BENCH_DONE` appears only when the stop
  option is enabled.

## Compare

- Use the same ROM, input, clock, pacing, transport, and probe options.
- `bench-p4.py` also checks the chip target, PSRAM clock, and LCD/audio/USB
  pad settings before comparing results.
- `run_us` excludes the pacing wait. Divide it by `frames` for time per frame.
- Cycle counters use the configured CPU clock. Timing probes add overhead.
- `execute_us` includes game execution and scanline rendering; it is not a
  CPU-only value.
- Compare thumbnails by image similarity when pixel-exact output differs.

## DSI display cost

- Enable `CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS`,
  `CONFIG_GPSP_LCD_DSI_DIAGNOSTICS` and `CONFIG_GPSP_STATUS`.
  OSPTEK uses light measurement by default.
- `STAT profile` samples one in 16 game frames. Every 10 seconds, the
  status worker logs separate `mode=0` (Off), `mode=1` (Simple), and
  `mode=2` (Full) summaries.
  Timings are elapsed times, including preemption and driver waits.
- `ppa_avg_us`: PPA scale/rotation call. `expand_avg_us`: integer scaling,
  including SRAM tile transfer when `CONFIG_GPSP_LCD_DSI_DMA_EXPAND` is on.
- `dma_wait_avg_us`: tile submission and completion wait within expansion.
- `blend_avg_us` and `blend_max_us`: the active Stats overlay, including driver
  calls and completion waits. The maximum is from sampled frames.
- `present_avg_us`: frame submission and scanout wait, separate from blend.
- `STAT cache`: average/maximum text-cache build time per update. The
  status worker rebuilds it every 2 seconds while Stats is enabled.
- `DSI CPU0`: frame execution and pacing-wait average/maximum every
  2 seconds, tagged with filter, color, and Stats mode (`stat=0/1/2`). Execution includes
  core work, callbacks, and JIT upkeep. It is elapsed time, including stalls.
  The frame window is independent of the network profile.
- `SLOW`: slow-frame count and average every 2 seconds, with details for
  the slowest frame. CPU0 records counters; the status worker prints them.
- Compare STAT Off/On with the same ROM, scene, filter, clocks, and input.
  Check FPS and speed as well as timings; spare scanout wait can hide cost.
- With DSI diagnostics off, phase probes and profiling logs are removed.

### OSPTEK integer scaling

- 2026-10-20: CPU 400 MHz, PSRAM 250 MHz, 2048 idle screen.
- STAT off, colors None, frameskip off; unchanged source checksum `6ba7d821`.
- Before: direct PSRAM writes and scalar grid shading. After: two SRAM
  tiles, DMA2D transfer, and PIE shading. Expansion includes DMA waits.

| Filter | Expansion before/after | CPU1 before/after | CPU0 run before/after |
| --- | --- | --- | --- |
| Nearest | 4.49 / 2.93 ms | 35.6 / 20.0% | 2.18 / 2.02 ms |
| LCD Grid | 9.13 / 2.94 ms | 62.0 / 20.6% | 2.47 / 2.03 ms |

- Both modes keep 59.7 fps. Bilinear remains about 3.75 ms on PPA and
  1.85 ms of CPU0 execution, with 4.2% CPU1 use.

#### Asterix state 0

- 2026-10-20: OSPTEK P4C5, CPU 400 MHz, PSRAM 250 MHz.
- Asterix and Obelix XXL, state 0, L held; STAT on, colors None,
  frameskip off, mute on. Normal audio and USB Bluetooth remain active.
- Each run: 3,072 frames; discard the first 960. Keep later stalls.
  Percentiles use individual frames and nearest rank.
- Previous: 8-row SRAM tiles with PPA rotation. New: queued DMA2D copies,
  fused PIE grid shading, and optional CPU1 rotation.
- Times below are CPU0 execution p95 / p99 / worst, in milliseconds.
  Grid samples for Previous and New 8-row PPA pool two runs (4,224 frames).
  Other entries use 2,112 frames.

| Path | Tile SRAM | Nearest | LCD Grid |
| --- | --- | --- | --- |
| Previous 8-row PPA | 45 KiB | 17.19 / 19.20 / 27.43 | 17.17 / 19.06 / 25.38 |
| New 8-row PPA | 45 KiB | 17.06 / 18.95 / 21.16 | 17.05 / 18.73 / 39.52 |
| New 8-row CPU1 | 45 KiB | 16.68 / 18.53 / 24.37 | 16.74 / 18.52 / 22.75 |
| New 12-row PPA | 67.5 KiB | 17.00 / 18.55 / 20.83 | 16.96 / 18.90 / 29.14 |
| New 16-row PPA | 90 KiB | 16.79 / 18.50 / 36.38 | 17.02 / 18.76 / 21.62 |

- OSPTEK defaults to 8-row CPU1 rotation. It retains 45 KiB of tile SRAM.
- Total display preparation p95: Nearest 6.12 to 5.22 ms; Grid 6.13 to
  5.48 ms. This includes rotation, expansion, and STAT before presentation.
  CPU rotation moves rotation into the expansion timing bucket.
- All game-frame checksums match (`c48ef011`); framebuffer checksums match
  per filter, excluding STAT. Native PIE pixels match scalar references.
- CPU0 INFO output cost in a separate two-run check: at most 0.244 ms
  per frame inside `esp_log`. Output off barely changes p95 (16.99 to
  16.98 ms); checksum argument evaluation remains outside this timing.
  The earlier 39.52 ms stall remains unexplained.
- Presentation gaps are usually 16.70 ms. Worst gaps are about 33.4 ms,
  except 16-row Nearest at 50.07 ms; these are not panel refresh measurements.

#### Doom 2 state 0

- 2026-10-21: OSPTEK P4C5 v3.2, CPU 400 MHz, PSRAM 250 MHz, L2 256 KiB.
- Doom 2, state 0, Left held; virtual pad 25%, Full Stats, colors None,
  frameskip off. Audio unchanged: volume -30, mute off.
- Prime 120 frames per boot. Reload state for each 3,072-frame run;
  discard the first 960 frames and keep every later stall.
- Nearest pools two runs per path (4,224 retained frames). Other filters
  use 2,112 frames. All final game checksums match (`1f1e8fb5`).
- One diagnostic image selects interrupt core and Stats path at runtime.
  Full JIT benchmarking and periodic performance logs are disabled.
- Times below are CPU0 mean / p99 / worst, in milliseconds. They exclude
  pacing waits and include interrupts and memory stalls.

| Filter | Baseline | CPU1 interrupts | CPU1 interrupts + SRAM Stats |
| --- | --- | --- | --- |
| Nearest | 16.352 / 20.475 / 23.093 | 15.890 / 20.083 / 22.301 | 15.577 / 19.647 / 21.583 |
| Bilinear | 15.657 / 19.497 / 20.525 | 15.434 / 19.492 / 22.473 | 15.159 / 18.876 / 19.846 |
| LCD Grid | 16.363 / 20.472 / 22.571 | 15.904 / 19.993 / 21.930 | 15.597 / 19.703 / 21.651 |

- DMA2D/PPA completion interrupts move to CPU1; DSI bridge stays on CPU0.
- SRAM Stats reuses the 45 KiB scaling allocation after queued copies drain.
  Span caches add 219.375 KiB PSRAM; ARGB caches remain for PPA fallback.
- Nearest Stats mean: 1.740 to 1.394 ms. Native Full/Simple output matches
  PPA pixels for CPU/DMA backgrounds, including old dirty cache lines.
- Combined presented FPS: Nearest 59.181 to 59.507, Bilinear 59.521 to
  59.719, LCD Grid 59.181 to 59.578. Game speed remains about 100%.
- CPU0 p99 still exceeds the 16.743 ms frame budget. Panel/game rate
  mismatch also causes some 33 ms presentation gaps.

## ROM paging checks

- Run `make -C components/gpsp/core/tests/gpsp-rom-scan test` for signature boundaries and errors.
- On P4, enable cache autoload and connect the USB pad. Use a 16 MiB ROM
  with a 1 MiB ROM buffer, then check the default 8 MiB configuration.
  Check cold-boot SD mounting, game execution, cartridge saves, and state
  loading.
- For Clock checks, verify slot/page mappings, ROM mirrors, sticky pages,
  and remaps without file reads. For JIT checks, invalidate or replace the
  current page during translation and verify the next opcode's source.
- Record the ROM header code and SHA-256, source revision and patch,
  generated config, firmware hash, state slot, input, scene, and interval.
- Record hardware results separately from host checks. A successful build
  does not validate DMA/cache behavior on the board.

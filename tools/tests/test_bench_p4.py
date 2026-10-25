#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "bench-p4.py"
SPEC = importlib.util.spec_from_file_location("bench_p4", SCRIPT)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


def startup(path, size=6):
    return (f"ROM ready: {path} ({size} bytes)\n"
            f"Execution started: configured_mode=rv32-jit ROM={path}\n")


class ConfigTests(unittest.TestCase):
    def test_physical_targets(self):
        parser = BENCH.load_parser()
        for target, emulated, accepted in (("ESP32P4", False, True),
                                           ("ESP32S31", False, True),
                                           ("ESP32S31", True, False),
                                           ("ESP32S3", False, False)):
            with self.subTest(target=target, emulated=emulated):
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "sdkconfig.h"
                    path.write_text(
                        "#define CONFIG_GPSP_BENCHMARK 1\n"
                        "#define CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT 1\n"
                        "#define CONFIG_GPSP_BENCHMARK_FRAMES 64\n"
                        f"#define CONFIG_IDF_TARGET_{target} 1\n" +
                        ("#define CONFIG_ESPEMU 1\n" if emulated else ""))
                    if accepted:
                        self.assertEqual(BENCH.read_config(path, parser)[1], 64)
                    else:
                        with self.assertRaises(ValueError):
                            BENCH.read_config(path, parser)


class RomTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        folder = Path(self.directory.name)
        self.rom = folder / "Doom II.gba"
        self.rom.write_bytes(b"sd-rom")
        self.copied_rom = folder / "game.gba"
        self.copied_rom.write_bytes(b"old-rom")

    def test_sd_uses_selected_rom_not_built_flash_rom(self):
        path = "/sdcard/roms/Doom II.gba"
        log = "\x1b[32m" + startup(path) + "\x1b[0m"
        result = BENCH.rom_info(log, self.rom, self.copied_rom, "sd")
        self.assertEqual(result["loaded_rom_path"], path)
        self.assertEqual(result["rom_source"], "sd")
        self.assertEqual(result["loaded_rom_bytes"], 6)
        self.assertEqual(result["rom_sha256"], BENCH.sha256(self.rom))

    def test_sd_requires_local_rom_even_with_built_flash_rom(self):
        with self.assertRaisesRegex(ValueError, "require --rom"):
            BENCH.rom_info(startup("/sdcard/game.gba"), None, self.copied_rom)

    def test_sd_requirement_rejects_flash_fallback(self):
        with self.assertRaisesRegex(ValueError, "expected ROM source sd, got flash"):
            BENCH.rom_info(startup("/storage/game.gba", 7), self.rom,
                           self.copied_rom, "sd")

    def test_sd_size_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "loaded ROM size differs"):
            BENCH.rom_info(startup("/sdcard/game.gba", 7), self.rom,
                           self.copied_rom)

    def test_ready_and_execution_paths_must_match(self):
        log = startup("/sdcard/game.gba").replace(
            "ROM ready: /sdcard/game.gba", "ROM ready: /storage/game.gba")
        with self.assertRaisesRegex(ValueError, "paths differ"):
            BENCH.rom_info(log, self.rom, self.copied_rom)

    def test_flash_still_checks_built_rom(self):
        with self.assertRaisesRegex(ValueError, "differs from built storage image"):
            BENCH.rom_info(startup("/storage/game.gba"), self.rom,
                           self.copied_rom)
        result = BENCH.rom_info(startup("/storage/game.gba", 7), None,
                                self.copied_rom)
        self.assertEqual(result["rom_source"], "flash")
        self.assertEqual(result["rom_sha256"], BENCH.sha256(self.copied_rom))

    def test_legacy_log_without_ready_line_still_works(self):
        result = BENCH.rom_info(
            "Execution started: configured_mode=rv32-jit ROM=/storage/game.gba\n",
            None, self.copied_rom)
        self.assertIsNone(result["loaded_rom_bytes"])

    def test_multiple_startups_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "expected one ROM startup"):
            BENCH.rom_info(startup("/sdcard/a.gba") + startup("/sdcard/b.gba"),
                           self.rom, self.copied_rom)

    def test_sd_log_file_cli_records_actual_source(self):
        folder = Path(self.directory.name)
        build = folder / "build"
        (build / "config").mkdir(parents=True)
        (build / "config" / "sdkconfig.h").write_text(
            "#define CONFIG_GPSP_BENCHMARK 1\n"
            "#define CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT 1\n"
            "#define CONFIG_GPSP_BENCHMARK_FRAMES 64\n"
            "#define CONFIG_IDF_TARGET_ESP32P4 1\n"
            "#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 400\n"
            "#define CONFIG_GPSP_SD_CARD 1\n")
        (build / "gpsp_esp32.bin").write_bytes(b"firmware")
        storage = build / "esp-idf" / "main" / "storage"
        storage.mkdir(parents=True)
        (storage / "game.gba").write_bytes(self.copied_rom.read_bytes())
        path = "/sdcard/roms/Doom II.gba"
        lines = [startup(path),
                 "BENCH_SLICE version=1 first=1 last=64 frames=64 run_us=64000 "
                 "video_frames=64 submitted=64 sent=64 dropped=0 checksum=12345678",
                 "BENCH_CPU_SLICE version=1 first=1 last=64 sched_cycles=1"]
        for name in ("BENCH_ENGINE", "BENCH_JIT_PHASE", "BENCH_JIT_TRANSLATE",
                     "BENCH_JIT_CACHE", "BENCH_JIT_SPANS", "BENCH_JIT_SITES",
                     "BENCH_JIT_FALLBACK", "BENCH_JIT_REGIONS",
                     "BENCH_JIT_FALLBACK_TOP"):
            for _ in range(8 if name == "BENCH_JIT_FALLBACK_TOP" else 1):
                lines.append(f"{name} version=1 first=1 last=64 ready=true blocks=1")
        lines.extend(f"BENCH_IMAGE version=1 first=1 last=64 row={row} "
                     f"rgb565={'0000' * 30}" for row in range(20))
        lines.append("BENCH_DONE frames=64 submitted=64 sent=64 dropped=0")
        log = folder / "serial.log"
        log.write_text("\n".join(lines))
        output = folder / "result"
        argv = [str(SCRIPT), "--build-dir", str(build), "--log-file", str(log),
                "--rom", str(self.rom), "--rom-source", "sd",
                "--output-dir", str(output)]
        with patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
            self.assertEqual(BENCH.main(), 0)
        result = json.loads((output / "result.json").read_text())
        self.assertEqual(result["loaded_rom_path"], path)
        self.assertEqual(result["rom_source"], "sd")
        self.assertEqual(result["rom_sha256"], BENCH.sha256(self.rom))

        # Keep the same build and ROM, but make the board fall back to flash.
        log.write_text(log.read_text().replace(path, "/storage/game.gba"))
        output = folder / "fallback"
        argv[-1] = str(output)
        with (patch.object(sys, "argv", argv), redirect_stderr(io.StringIO()) as errors):
            self.assertEqual(BENCH.main(), 1)
        self.assertIn("expected ROM source sd, got flash", errors.getvalue())
        self.assertTrue((output / "firmware.log").is_file())
        self.assertFalse((output / "result.json").exists())


class CompareTests(unittest.TestCase):
    def test_memory_clock_and_peripherals_must_match(self):
        base = {"CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ": "400",
                "CONFIG_SPIRAM_SPEED": "200"}
        for key, value, condition in (
                ("CONFIG_SPIRAM_SPEED", "250", "psram_mhz"),
                ("CONFIG_GPSP_LCD", "1", "lcd"),
                ("CONFIG_GPSP_AUDIO", "1", "audio"),
                ("CONFIG_GPSP_USB_PAD", "1", "usb_pad")):
            with self.subTest(condition=condition):
                with self.assertRaisesRegex(ValueError, condition):
                    self.run_compare(BENCH.conditions(base),
                                     BENCH.conditions({**base, key: value}))

    def test_comparison_requires_same_target(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "result.json").write_text('{"target": "esp32p4"}')
            with self.assertRaisesRegex(ValueError, "chip targets differ"):
                BENCH.compare({"target": "esp32s31"}, path)

    def run_compare(self, previous_conditions, current_conditions,
                    previous_source=None, current_source=None):
        result = {"rom_sha256": "rom", "frames": 64, "checksums": [],
                  "warm_run_ms_per_frame": None}
        previous = {**result, "conditions": previous_conditions}
        current = {**result, "conditions": current_conditions}
        if previous_source is not None:
            previous["rom_source"] = previous_source
        if current_source is not None:
            current["rom_source"] = current_source
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "result.json").write_text(json.dumps(previous))
            return BENCH.compare(current, path)

    def button_conditions(self, mask="0x0", status=False):
        options = {"CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ": "400",
                   "CONFIG_GPSP_BENCHMARK_HOLD_BUTTONS": mask}
        if status:
            options["CONFIG_GPSP_STATUS"] = "1"
        return BENCH.conditions(options)

    def test_held_buttons_must_match(self):
        with self.assertRaisesRegex(ValueError, "held_buttons"):
            self.run_compare(self.button_conditions(),
                             self.button_conditions("0x40"))

    def test_equivalent_button_masks_can_be_compared(self):
        self.run_compare(self.button_conditions("0x0040"),
                         self.button_conditions("0x40"))

    def test_status_setting_must_match(self):
        with self.assertRaisesRegex(ValueError, "status"):
            self.run_compare(self.button_conditions(),
                             self.button_conditions(status=True))

    def test_missing_probe_counts_as_off(self):
        self.run_compare({"cpu_mhz": 400}, {"cpu_mhz": 400, "instret": False})

    def test_missing_probe_rejects_enabled(self):
        with self.assertRaisesRegex(ValueError, "instret"):
            self.run_compare({"cpu_mhz": 400},
                             {"cpu_mhz": 400, "instret": True})

    def test_legacy_source_counts_as_flash(self):
        self.run_compare({}, {}, current_source="flash")

    def test_comparison_requires_same_rom_source(self):
        self.run_compare({}, {}, previous_source="sd", current_source="sd")
        with self.assertRaisesRegex(ValueError, "ROM sources differ"):
            self.run_compare({}, {}, previous_source="flash", current_source="sd")
        with self.assertRaisesRegex(ValueError, "ROM sources differ"):
            self.run_compare({}, {}, current_source="sd")


if __name__ == "__main__":
    unittest.main()

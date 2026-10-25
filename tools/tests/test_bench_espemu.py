# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Configured benchmark window and detail validation."""

import importlib.util
import unittest
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "bench-espemu.py"
SPEC = importlib.util.spec_from_file_location("bench_espemu", SCRIPT)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


def merged_image(path, rom):
    image = bytearray(b"\xff" * 0x40000)
    entry = bytearray(32)
    entry[0:4] = b"\xaa\x50\x01\x81"
    entry[4:8] = (0x20000).to_bytes(4, "little")
    entry[8:12] = (0x20000).to_bytes(4, "little")
    entry[12:19] = b"storage"
    image[0x8000:0x8020] = entry

    boot = 0x21000
    image[boot:boot + 512] = b"\0" * 512
    image[boot + 11:boot + 13] = (512).to_bytes(2, "little")
    image[boot + 13] = 1  # sectors per cluster
    image[boot + 14:boot + 16] = (1).to_bytes(2, "little")
    image[boot + 16] = 1  # number of FATs
    image[boot + 17:boot + 19] = (16).to_bytes(2, "little")
    image[boot + 19:boot + 21] = (100).to_bytes(2, "little")
    image[boot + 22:boot + 24] = (1).to_bytes(2, "little")
    image[boot + 54:boot + 62] = b"FAT12   "
    image[boot + 510:boot + 512] = b"\x55\xaa"

    root = boot + 1024
    image[root:root + 32] = b"\0" * 32
    image[root:root + 11] = b"GAME    GBA"
    image[root + 11] = 0x20
    image[root + 26:root + 28] = (2).to_bytes(2, "little")
    image[root + 28:root + 32] = len(rom).to_bytes(4, "little")
    image[boot + 1536:boot + 1536 + len(rom)] = rom
    path.write_bytes(image)


def log_for(frames, detail=False):
    lines = []
    for first in range(1, frames + 1, 64):
        last = min(first + 63, frames)
        count = last - first + 1
        lines.append(f"BENCH_SLICE version=1 first={first} last={last} "
                     f"frames={count} run_us={count * 1000} "
                     f"video_frames={count} submitted={count} sent={count} "
                     "dropped=0 checksum=00000000")
        lines.append(f"BENCH_CPU_SLICE version=1 first={first} last={last} "
                     "sched_cycles=1")
        if detail:
            lines.append(f"BENCH_WORK_SLICE version=1 first={first} last={last} "
                         "sched=1")
        for name in ("BENCH_ENGINE", "BENCH_JIT_PHASE", "BENCH_JIT_TRANSLATE",
                     "BENCH_JIT_CACHE", "BENCH_JIT_SPANS", "BENCH_JIT_SITES",
                     "BENCH_JIT_FALLBACK", "BENCH_JIT_REGIONS",
                     "BENCH_JIT_FALLBACK_TOP"):
            for _ in range(BENCH.ROWS_PER_WINDOW.get(name, 1)):
                lines.append(f"{name} version=1 first={first} last={last} "
                             "ready=true blocks=1")
        for row in range(20):
            lines.append(f"BENCH_IMAGE version=1 first={first} last={last} "
                         f"row={row} rgb565={'0000' * 30}")
    lines.append(f"BENCH_DONE frames={frames} submitted={frames} "
                 f"sent={frames} dropped=0")
    return "\n".join(lines)


class ParseLogTest(unittest.TestCase):
    def test_embedded_rom_fingerprint(self):
        with tempfile.TemporaryDirectory() as folder:
            first, second = Path(folder) / "a.bin", Path(folder) / "b.bin"
            merged_image(first, b"A")
            merged_image(second, b"B")
            a, b = BENCH.embedded_rom_info(first), BENCH.embedded_rom_info(second)
            self.assertEqual(a["size"], 1)
            self.assertNotEqual(a["sha256"], b["sha256"])

    def test_missing_fat_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / "bad.bin"
            image.write_bytes(b"\xff" * 0x40000)
            with self.assertRaisesRegex(ValueError, "storage partition not found"):
                BENCH.embedded_rom_info(image)
            merged_image(image, b"A")
            contents = bytearray(image.read_bytes())
            contents[0x21000 + 510:0x21000 + 512] = b"\0\0"
            image.write_bytes(contents)
            with self.assertRaisesRegex(ValueError, "FAT boot sector not found"):
                BENCH.embedded_rom_info(image)

    def test_configured_windows(self):
        records, done = BENCH.parse_log(log_for(128), 128, {})
        self.assertEqual(len(records["BENCH_SLICE"]), 2)
        self.assertEqual(done["frames"], "128")

    def test_partial_last_window(self):
        records, _ = BENCH.parse_log(log_for(70), 70, {})
        self.assertEqual(records["BENCH_SLICE"][1]["frames"], "6")

    def test_shutdown_summaries_are_not_window_records(self):
        for name in BENCH.SUMMARY_RECORDS:
            with self.subTest(name=name):
                summary = f"{name} version=1 attempts=4"
                records, _ = BENCH.parse_log(log_for(128) + "\n" + summary,
                                              128, {})
                self.assertEqual(len(records[name]), 1)
                for invalid in (summary + "\n" + summary,
                                summary + " first=1 last=128"):
                    with self.assertRaisesRegex(ValueError, "invalid summary"):
                        BENCH.parse_log(log_for(128) + "\n" + invalid, 128, {})

    def test_configured_count_is_required(self):
        with self.assertRaisesRegex(ValueError, "BENCH_DONE"):
            BENCH.parse_log(log_for(192), 5120, {})

    def test_enabled_detail_is_required_each_window(self):
        with self.assertRaisesRegex(ValueError, "BENCH_WORK_SLICE"):
            BENCH.parse_log(log_for(128), 128,
                            {"CONFIG_GPSP_BENCHMARK_PHASE_WORK": "1"})

    def test_detail_is_checked_across_windows(self):
        records, _ = BENCH.parse_log(log_for(128, detail=True), 128,
                                     {"CONFIG_GPSP_BENCHMARK_PHASE_WORK": "1"})
        self.assertEqual(len(records["BENCH_WORK_SLICE"]), 2)

    def test_missing_or_corrupt_image_is_rejected(self):
        log = log_for(64)
        without_images = "\n".join(line for line in log.splitlines()
                                   if not line.startswith("BENCH_IMAGE "))
        with self.assertRaisesRegex(ValueError, "BENCH_IMAGE"):
            BENCH.parse_log(without_images, 64, {})
        with self.assertRaisesRegex(ValueError, "thumbnail data"):
            BENCH.parse_log(log.replace("rgb565=0000", "rgb565=zzzz", 1),
                            64, {})

    def test_missing_checksum_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "checksum"):
            BENCH.parse_log(log_for(64).replace(" checksum=00000000", ""),
                            64, {})

    def test_incomplete_detail_fields_are_rejected(self):
        log = log_for(128).replace(
            "BENCH_CPU_SLICE version=1 first=65 last=128 sched_cycles=1",
            "BENCH_CPU_SLICE version=1 first=65 last=128")
        with self.assertRaisesRegex(ValueError, "inconsistent record fields"):
            BENCH.parse_log(log, 128, {})

    def test_missing_video_output_is_rejected(self):
        log = log_for(128)
        BENCH.parse_log(log, 128, {}, full_video=True)
        lagged = log.replace("sent=64", "sent=63", 1)
        lagged = lagged.replace("sent=64", "sent=65", 1)
        BENCH.parse_log(lagged, 128, {}, full_video=True)
        with self.assertRaisesRegex(ValueError, "incomplete video output"):
            BENCH.parse_log(log.replace("submitted=64", "submitted=63", 1),
                            128, {},
                            full_video=True)
        with self.assertRaisesRegex(ValueError, "incomplete benchmark video"):
            BENCH.parse_log(log.replace("sent=128", "sent=127"), 128, {},
                            full_video=True)

    def test_jit_build_options_can_differ(self):
        first = {"label": "current", "rom": "rom", "frames": 128,
                 "port": 24680, "defines": {"CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ": "400"}}
        second = {**first, "label": "block", "defines": {
            **first["defines"],
            "CONFIG_GPSP_RV32_ROM_CACHE_KIB": "2048",
        }}
        BENCH.validate_builds([first, second])
        second["defines"]["CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ"] = "360"
        with self.assertRaisesRegex(ValueError, "non-JIT sdkconfig"):
            BENCH.validate_builds([first, second])

    def test_repeat_checksum_drift_is_rejected(self):
        first = {"label": "block", "checksums": {"1": "00000000"}}
        second = {"label": "block", "checksums": {"1": "00000001"}}
        BENCH.validate_repeat_checksums([first, first])
        with self.assertRaisesRegex(ValueError, "checksum drift"):
            BENCH.validate_repeat_checksums([first, second])

    def test_jit_execution_is_required(self):
        for before, after, error in (
                ("ready=true", "ready=false", "not ready"),
                ("blocks=1", "blocks=0", "did not execute")):
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                BENCH.parse_log(log_for(64).replace(before, after), 64, {})

    def test_new_jit_records_are_required(self):
        names = (
            "BENCH_JIT_DETAIL", "BENCH_JIT_EXITS", "BENCH_JIT_HOT",
            "BENCH_JIT_MEMORY", "BENCH_JIT_HELPERS", "BENCH_JIT_ARM_MEMORY",
            "BENCH_JIT_ROM", "BENCH_JIT_BIOS", "BENCH_JIT_BIOS_EDGE",
            "BENCH_JIT_BIOS_TOP", "BENCH_JIT_VIDEO_MEM",
            "BENCH_JIT_BIOS_VIDEO_STORE", "BENCH_JIT_LOOKUP_TOP",
            "BENCH_JIT_SIDE_COST", "BENCH_JIT_TRANSLATE_PHASE",
            "BENCH_JIT_LOOKUP", "BENCH_JIT_GUARD",
            "BENCH_JIT_DIAG", "BENCH_JIT_MISS_CLASS",
            "BENCH_JIT_MISS_REASON", "BENCH_JIT_CODE_BYTES",
            "BENCH_JIT_OVERLAP_BYTES", "BENCH_JIT_ALIAS",
            "BENCH_JIT_SLOW_POOL", "BENCH_JIT_MISS_ENTRY",
        )
        options = {key: "1" for key in BENCH.DETAIL_OPTIONS
                   if "JIT" in key}
        records = []
        for name in names:
            for _ in range(BENCH.ROWS_PER_WINDOW.get(name, 1)):
                records.append(f"{name} version=2 first=1 last=64")
        log = log_for(64) + "\n" + "\n".join(records)
        BENCH.parse_log(log, 64, options)
        for name in ("BENCH_JIT_EXITS", "BENCH_JIT_HOT", "BENCH_JIT_DIAG",
                     "BENCH_JIT_SIDE_COST", "BENCH_JIT_SLOW_POOL"):
            incomplete = "\n".join(line for line in log.splitlines()
                                   if not line.startswith(name + " "))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                BENCH.parse_log(incomplete, 64, options)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run configured 64-frame gpSP benchmarks in ESP-EMU."""

import argparse
import csv
import hashlib
import json
import queue
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RECORD = re.compile(r"\b(BENCH_[A-Z_]+)\s+(.*)")
FIELD = re.compile(r"([a-z][a-z_0-9]*)=([^\s]+)")
PANIC = re.compile(r"Guru Meditation|assert failed|abort\(\)|Backtrace:|"
                   r"task_wdt: Task watchdog|Load access fault|Store access fault",
                   re.IGNORECASE)
REQUIRED = ("BENCH_SLICE", "BENCH_CPU_SLICE")
DETAIL_OPTIONS = {
    "CONFIG_GPSP_BENCHMARK_PHASE_WORK": ("BENCH_WORK_SLICE",),
    "CONFIG_GPSP_BENCHMARK_INSTRET": ("BENCH_INSTRET_SLICE",),
    "CONFIG_GPSP_BENCHMARK_VIDEO_DETAIL":
        ("BENCH_VIDEO_SLICE", "BENCH_VIDEO_LINES_SLICE"),
    "CONFIG_GPSP_BENCHMARK_JIT_DETAIL":
        ("BENCH_JIT_DETAIL", "BENCH_JIT_EXITS", "BENCH_JIT_HOT",
         "BENCH_JIT_MEMORY", "BENCH_JIT_HELPERS",
         "BENCH_JIT_ARM_MEMORY", "BENCH_JIT_ROM", "BENCH_JIT_BIOS",
         "BENCH_JIT_BIOS_EDGE", "BENCH_JIT_BIOS_TOP",
         "BENCH_JIT_VIDEO_MEM", "BENCH_JIT_BIOS_VIDEO_STORE"),
    "CONFIG_GPSP_BENCHMARK_JIT_LOOKUP":
        ("BENCH_JIT_LOOKUP_TOP", "BENCH_JIT_SIDE_COST",
         "BENCH_JIT_TRANSLATE_PHASE", "BENCH_JIT_LOOKUP"),
    "CONFIG_GPSP_BENCHMARK_JIT_MISS_CLASS":
        ("BENCH_JIT_GUARD", "BENCH_JIT_DIAG", "BENCH_JIT_MISS_CLASS",
         "BENCH_JIT_MISS_REASON"),
    "CONFIG_GPSP_BENCHMARK_JIT_CODE_BYTES":
        ("BENCH_JIT_CODE_BYTES", "BENCH_JIT_OVERLAP_BYTES",
         "BENCH_JIT_ALIAS", "BENCH_JIT_SLOW_POOL", "BENCH_JIT_MISS_ENTRY"),
}
ROWS_PER_WINDOW = {
    "BENCH_IMAGE": 20,
    "BENCH_JIT_BIOS_TOP": 8,
    "BENCH_JIT_FALLBACK_TOP": 8,
    "BENCH_JIT_VIDEO_MEM": 18,
    "BENCH_JIT_BIOS_VIDEO_STORE": 3,
    "BENCH_JIT_LOOKUP": 3,
    "BENCH_JIT_MISS_CLASS": 3,
    "BENCH_JIT_MISS_REASON": 3,
    "BENCH_JIT_MISS_ENTRY": 3,
}
SUMMARY_RECORDS = ("BENCH_JIT_SHORT_BRANCH", "BENCH_JIT_SMC",
                   "BENCH_JIT_RAM_UNITS")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_tree_sha256():
    paths = subprocess.check_output([
        "git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"
    ], cwd=REPO).split(b"\0")
    digest = hashlib.sha256()
    for raw in sorted(set(paths)):
        if not raw:
            continue
        path = REPO / raw.decode("utf-8", "surrogateescape")
        if path.is_file():
            digest.update(raw + b"\0")
            digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def defines(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith("#define CONFIG_"):
            _, name, value = line.split(maxsplit=2)
            result[name] = value
    return result


def embedded_rom_info(path):
    """Read /storage/game.gba from the FAT volume in an ESP merged image."""
    image = path.read_bytes()
    partition = None
    for table in range(0x8000, min(len(image), 0x20000), 0x1000):
        if image[table:table + 2] != b"\xaa\x50":
            continue
        for entry in range(table, min(table + 0xC00, len(image)), 32):
            item = image[entry:entry + 32]
            if item[:2] != b"\xaa\x50":
                break
            if item[2:4] == b"\x01\x81" and item[12:28].split(b"\0", 1)[0] == b"storage":
                offset = int.from_bytes(item[4:8], "little")
                size = int.from_bytes(item[8:12], "little")
                if offset + size <= len(image):
                    partition = image[offset:offset + size]
                break
        if partition is not None:
            break
    if partition is None:
        raise ValueError(f"storage partition not found in merged firmware: {path}")

    # ESP-IDF's wear-levelled FAT image has a small prefix before the boot sector.
    boot = None
    for offset in range(0, min(len(partition), 0x10000), 512):
        sector = partition[offset:offset + 512]
        if (len(sector) == 512 and sector[510:512] == b"\x55\xaa" and
                sector[54:62].startswith(b"FAT")):
            boot = offset
            break
    if boot is None:
        raise ValueError(f"FAT boot sector not found in storage partition: {path}")

    volume = partition[boot:]
    sector_size = int.from_bytes(volume[11:13], "little")
    cluster_sectors = volume[13]
    reserved = int.from_bytes(volume[14:16], "little")
    fat_count = volume[16]
    root_entries = int.from_bytes(volume[17:19], "little")
    fat_sectors = int.from_bytes(volume[22:24], "little")
    sectors = int.from_bytes(volume[19:21], "little") or int.from_bytes(volume[32:36], "little")
    if (sector_size not in (512, 1024, 2048, 4096) or not cluster_sectors or
            not reserved or not fat_count or not root_entries or not fat_sectors or
            not sectors):
        raise ValueError(f"unsupported FAT geometry in merged firmware: {path}")
    root_sectors = (root_entries * 32 + sector_size - 1) // sector_size
    data_sectors = sectors - reserved - fat_count * fat_sectors - root_sectors
    if data_sectors <= 0:
        raise ValueError(f"invalid FAT geometry in merged firmware: {path}")
    clusters = data_sectors // cluster_sectors
    if clusters >= 65525:
        raise ValueError(f"FAT32 storage image is not supported: {path}")
    fat_bits = 12 if clusters < 4085 else 16
    fat_start = reserved * sector_size
    root_start = (reserved + fat_count * fat_sectors) * sector_size
    data_start = root_start + root_sectors * sector_size
    if data_start + data_sectors * sector_size > len(volume):
        raise ValueError(f"truncated FAT volume in merged firmware: {path}")

    rom_entry = None
    for offset in range(root_start, root_start + root_entries * 32, 32):
        entry = volume[offset:offset + 32]
        if entry[0] == 0:
            break
        if entry[:11] == b"GAME    GBA" and entry[11] & 0x18 == 0:
            rom_entry = entry
            break
    if rom_entry is None:
        raise ValueError(f"/storage/game.gba not found in merged firmware: {path}")

    remaining = int.from_bytes(rom_entry[28:32], "little")
    if remaining == 0:
        raise ValueError(f"/storage/game.gba is empty in merged firmware: {path}")
    rom_size = remaining
    cluster = int.from_bytes(rom_entry[26:28], "little")
    cluster_size = sector_size * cluster_sectors
    seen = set()
    digest = hashlib.sha256()
    while remaining:
        if cluster < 2 or cluster >= clusters + 2 or cluster in seen:
            raise ValueError(f"invalid game.gba cluster chain in merged firmware: {path}")
        seen.add(cluster)
        offset = data_start + (cluster - 2) * cluster_size
        amount = min(remaining, cluster_size)
        digest.update(volume[offset:offset + amount])
        remaining -= amount
        if not remaining:
            break
        fat_offset = fat_start + (cluster * 3 // 2 if fat_bits == 12 else cluster * 2)
        entry = int.from_bytes(volume[fat_offset:fat_offset + 2], "little")
        cluster = (entry >> 4 if cluster & 1 else entry & 0x0FFF) if fat_bits == 12 else entry
    return {"sha256": digest.hexdigest(), "size": rom_size}


def build_info(argument):
    label, sep, directory = argument.partition("=")
    if not sep:
        directory, label = label, ""
    build = Path(directory).expanduser().resolve()
    firmware = build / "merged-binary.bin"
    elf = build / "gpsp_esp32.elf"
    config = build / "config" / "sdkconfig.h"
    for path in (firmware, elf, config):
        if not path.is_file():
            raise ValueError(f"missing build artifact: {path}")
    config_values = defines(config)
    for name in ("CONFIG_ESPEMU", "CONFIG_GPSP_BENCHMARK",
                 "CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT",
                 "CONFIG_COMPILER_OPTIMIZATION_PERF"):
        if config_values.get(name) != "1":
            raise ValueError(f"{config}: expected {name}=1")
    if "CONFIG_GPSP_ESPEMU_UNTHROTTLED" in config_values:
        raise ValueError(f"{config}: benchmark requires guest-clock pacing")
    frames = int(config_values["CONFIG_GPSP_BENCHMARK_FRAMES"])
    if not 64 <= frames <= 5120:
        raise ValueError(f"{config}: frame count outside 64..5120")
    mode = "rv32-jit"
    label = label or mode
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", label):
        raise ValueError(f"invalid label: {label}")
    rom = embedded_rom_info(firmware)
    return {
        "label": label, "mode": mode, "frames": frames,
        "build_dir": str(build), "firmware": str(firmware),
        "firmware_sha256": sha256(firmware), "elf": str(elf),
        "elf_sha256": sha256(elf), "config_sha256": sha256(config),
        "rom": rom, "port": int(config_values.get("CONFIG_GPSP_ESPEMU_PORT", 24680)),
        "cpu_mhz": int(config_values["CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ"]),
        "defines": config_values,
    }


class NeutralReceiver:
    def __init__(self, port):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(1)
        self.listener.settimeout(0.2)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.frames = 0
        self.hello = 0
        self.error = None

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.listener.close()
        self.thread.join(timeout=3)

    def _serve(self):
        try:
            while not self.stop_event.is_set():
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                with client:
                    client.settimeout(0.2)
                    pending = bytearray()
                    while not self.stop_event.is_set():
                        try:
                            data = client.recv(131072)
                        except socket.timeout:
                            continue
                        if not data:
                            break
                        pending.extend(data)
                        while len(pending) >= 24:
                            magic, version, kind, size, _, _ = struct.unpack_from(
                                ">IHHIIQ", pending)
                            if magic != 0x47505350 or version != 1 or size > 100000:
                                raise ValueError("invalid video stream header")
                            if len(pending) < 24 + size:
                                break
                            payload = pending[24:24 + size]
                            if kind == 1:
                                if size != 12 or struct.unpack_from(">HHH", payload) != (240, 160, 1):
                                    raise ValueError("unexpected video format")
                                self.hello += 1
                                client.sendall(struct.pack(">IHHIIQHH", 0x47505350,
                                                           1, 3, 4, 0, 0, 0, 0))
                            elif kind == 2:
                                if size != 8 + 240 * 160 * 2:
                                    raise ValueError("invalid video frame")
                                self.frames += 1
                            del pending[:24 + size]
        except (OSError, ValueError) as error:
            self.error = str(error)


def parse_log(text, frame_count, options, full_video=False):
    records = {}
    done = []
    for line in text.splitlines():
        if PANIC.search(line):
            raise ValueError(f"firmware panic: {line.strip()}")
        match = RECORD.search(line)
        if not match:
            continue
        name, payload = match.groups()
        fields = dict(FIELD.findall(payload))
        if name == "BENCH_DONE":
            done.append(fields)
        else:
            records.setdefault(name, []).append(fields)
    if len(done) != 1 or int(done[0].get("frames", -1)) != frame_count:
        raise ValueError("missing or incorrect BENCH_DONE")
    if int(done[0].get("dropped", -1)) != 0:
        raise ValueError("video output dropped frames")
    if full_video and any(int(done[0].get(key, -1)) != frame_count
                          for key in ("submitted", "sent")):
        raise ValueError("incomplete benchmark video output")
    windows = [(first, min(first + 63, frame_count))
               for first in range(1, frame_count + 1, 64)]
    mandatory = [*REQUIRED, "BENCH_IMAGE"]
    for option, names in DETAIL_OPTIONS.items():
        if option in options:
            mandatory.extend(names)
    mandatory.extend(("BENCH_ENGINE", "BENCH_JIT_PHASE", "BENCH_JIT_TRANSLATE",
                      "BENCH_JIT_CACHE", "BENCH_JIT_SPANS",
                      "BENCH_JIT_SITES", "BENCH_JIT_FALLBACK",
                      "BENCH_JIT_REGIONS", "BENCH_JIT_FALLBACK_TOP"))
    for name in mandatory:
        rows = records.get(name, [])
        observed = [(int(row.get("first", -1)), int(row.get("last", -1)))
                    for row in rows]
        expected = [window for window in windows
                    for _ in range(ROWS_PER_WINDOW.get(name, 1))]
        if name in ROWS_PER_WINDOW:
            if sorted(observed) != sorted(expected):
                raise ValueError(f"{name}: expected complete window detail")
        elif observed != windows:
            raise ValueError(f"{name}: expected {len(windows)} contiguous windows")
        fields = set(rows[0])
        if (not {"version", "first", "last"} <= fields or
                any(set(row) != fields for row in rows[1:])):
            raise ValueError(f"{name}: inconsistent record fields")
    for name, rows in records.items():
        if name in SUMMARY_RECORDS:
            if (len(rows) != 1 or "version" not in rows[0] or
                    {"first", "last"} & set(rows[0])):
                raise ValueError(f"{name}: invalid summary record")
            continue
        if name == "BENCH_IMAGE":
            expected = [(first, last, row) for first, last in windows
                        for row in range(20)]
            observed = [(int(item.get("first", -1)),
                         int(item.get("last", -1)), int(item.get("row", -1)))
                        for item in rows]
            if observed != expected:
                raise ValueError("incomplete benchmark thumbnails")
            if any(not re.fullmatch(r"[0-9a-fA-F]{120}", row.get("rgb565", ""))
                   for row in rows):
                raise ValueError("invalid benchmark thumbnail data")
            continue
        if name.startswith("BENCH_JIT_") or name.endswith("_SLICE"):
            observed = [(int(item.get("first", -1)),
                         int(item.get("last", -1))) for item in rows]
            expected = [window for window in windows
                        for _ in range(ROWS_PER_WINDOW.get(name, 1))]
            if sorted(observed) != sorted(expected):
                raise ValueError(f"{name}: missing window detail")
    slices = records["BENCH_SLICE"]
    for (first, last), row in zip(windows, slices):
        if int(row.get("frames", -1)) != last - first + 1:
            raise ValueError(f"wrong frame count at {first}..{last}")
        if int(row.get("run_us", 0)) <= 0:
            raise ValueError(f"missing run time at {first}..{last}")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", row.get("checksum", "")):
            raise ValueError(f"invalid checksum at {first}..{last}")
        if int(row.get("dropped", 0)) != 0:
            raise ValueError(f"video output dropped in {first}..{last}")
        # TCP completion may cross a slice boundary; BENCH_DONE checks its total.
        if full_video and any(int(row.get(key, -1)) != last - first + 1
                              for key in ("video_frames", "submitted")):
            raise ValueError(f"incomplete video output at {first}..{last}")
    engines = records["BENCH_ENGINE"]
    if any(row.get("ready") != "true" for row in engines):
        raise ValueError("block JIT was not ready")
    if sum(int(row.get("blocks", 0)) for row in engines) == 0:
        raise ValueError("block JIT did not execute")
    return records, done[0]


def run_one(build, repeat, output, emulator, watchdog):
    output.mkdir(parents=True)
    receiver = NeutralReceiver(build["port"])
    receiver.start()
    command = [str(emulator), "--chip", "esp32p4", "--firmware",
               build["firmware"], "--elf", build["elf"], "--psram-size",
               "32M", "--net", "user", "--timeout", f"{watchdog}s"]
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True,
                               encoding="utf-8", errors="replace", bufsize=1)
    lines = queue.Queue()
    def read_output():
        with (output / "firmware.log").open("w") as log:
            for line in process.stdout:
                log.write(line)
                log.flush()
                lines.put(line)
        lines.put(None)
    worker = threading.Thread(target=read_output, daemon=True)
    worker.start()
    deadline = time.monotonic() + watchdog
    saw_done = False
    try:
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                if process.poll() is not None and not worker.is_alive():
                    break
                continue
            if line is None:
                break
            if "BENCH_DONE frames=" in line:
                saw_done = True
                break
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        worker.join(timeout=5)
        receiver.stop()
    if not saw_done:
        raise ValueError(f"{build['label']}: BENCH_DONE absent or timed out")
    text = (output / "firmware.log").read_text(errors="replace")
    records, done = parse_log(text, build["frames"], build["defines"],
                              full_video=True)
    if (receiver.error or receiver.hello != 1 or
            receiver.frames != build["frames"]):
        raise ValueError(f"video receiver failed: {receiver.error}, "
                         f"hello={receiver.hello}, frames={receiver.frames}")
    windows = records["BENCH_SLICE"]
    elapsed = sum(int(row["run_us"]) for row in windows)
    frames = sum(int(row["frames"]) for row in windows)
    result = {
        "label": build["label"], "repeat": repeat, "passed": True,
        "frames": frames, "run_us": elapsed,
        "run_us_per_frame": elapsed / frames,
        "capacity_fps": 1_000_000 * frames / elapsed,
        "received_frames": receiver.frames, "done": done,
        "checksums": {row["first"]: row["checksum"].lower()
                      for row in windows},
    }
    (output / "records.json").write_text(json.dumps(records, indent=2) + "\n")
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    with (output / "slices.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=windows[0].keys())
        writer.writeheader()
        writer.writerows(windows)
    return result


def validate_repeat_checksums(results):
    by_label = {}
    for result in results:
        checksums = result["checksums"]
        previous = by_label.setdefault(result["label"], checksums)
        if checksums != previous:
            raise ValueError(f"{result['label']}: video checksum drift across runs")


def validate_builds(builds):
    if len({build["label"] for build in builds}) != len(builds):
        raise ValueError("build labels must be unique")
    excluded = {
        "CONFIG_GPSP_RV32_ROM_CACHE_KIB",
        "CONFIG_GPSP_RV32_RAM_CACHE_KIB",
        "CONFIG_GPSP_RV32_CODE_CHUNK_KIB",
        *DETAIL_OPTIONS,
    }
    first = {key: value for key, value in builds[0]["defines"].items()
             if key not in excluded}
    for build in builds[1:]:
        if (build["rom"] != builds[0]["rom"] or
            build["frames"] != builds[0]["frames"] or
            build["port"] != builds[0]["port"]):
            raise ValueError("builds must use the same ROM, frame count and port")
        second = {key: value for key, value in build["defines"].items()
                  if key not in excluded}
        if first != second:
            raise ValueError("non-JIT sdkconfig values differ between builds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", required=True,
                        metavar="[LABEL=]DIR")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--watchdog", type=int, default=600)
    parser.add_argument("--emu", default="esp-emu")
    args = parser.parse_args()
    if args.runs < 1 or args.watchdog < 10:
        parser.error("runs must be positive and watchdog at least 10 seconds")
    emulator = shutil.which(args.emu)
    if not emulator:
        parser.error(f"emulator not found: {args.emu}")
    try:
        builds = [build_info(value) for value in args.build_dir]
        validate_builds(builds)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    output = (args.output_dir or REPO / "profiles" /
              datetime.now(timezone.utc).strftime("bench-%Y%m%dT%H%M%SZ")).resolve()
    if output.exists() and any(output.iterdir()):
        parser.error(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_copy = REPO / ".bench-source-copy.json"
    manifest = {
        "schema": 2, "created_utc": datetime.now(timezone.utc).isoformat(),
        "frames": builds[0]["frames"], "slice_frames": 64,
        "runs_per_build": args.runs, "emulator": str(Path(emulator).resolve()),
        "emulator_sha256": sha256(Path(emulator)),
        "git_commit": None if source_copy.is_file() else subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "source_tree_sha256": (json.loads(source_copy.read_text())["source_tree_sha256"]
                               if source_copy.is_file() else source_tree_sha256()),
        "source_copy": json.loads(source_copy.read_text()) if source_copy.is_file() else None,
        "builds": [{key: value for key, value in build.items() if key != "defines"}
                   for build in builds],
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    for repeat in range(1, args.runs + 1):
        offset = (repeat - 1) % len(builds)
        for build in builds[offset:] + builds[:offset]:
            destination = output / build["label"] / f"run-{repeat:02d}"
            print(f"{build['label']} run {repeat}/{args.runs}: {destination}", flush=True)
            try:
                results.append(run_one(build, repeat, destination,
                                       emulator, args.watchdog))
            except ValueError as error:
                print(str(error), file=sys.stderr)
                return 1
    try:
        validate_repeat_checksums(results)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    summary = {"passed": True, "frames": builds[0]["frames"], "builds": {}}
    for build in builds:
        rates = [row["run_us_per_frame"] for row in results
                 if row["label"] == build["label"]]
        median = statistics.median(rates)
        summary["builds"][build["label"]] = {
            "runs": len(rates), "run_us_per_frame_median": median,
            "capacity_fps": 1_000_000 / median,
        }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    with (output / "bench.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows({**row, "done": json.dumps(row["done"]),
                          "checksums": json.dumps(row["checksums"])}
                         for row in results)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

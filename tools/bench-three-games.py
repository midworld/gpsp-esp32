#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Build and benchmark three GBA ROMs in separate ESP-EMU runs."""

import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
GAMES = ("metroid", "mario_kart", "kirby")
DEFAULTS = ("sdkconfig.defaults", "sdkconfig.espemu.defaults",
            "sdkconfig.benchmark.defaults",
            "sdkconfig.jit.defaults")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_roms(values, parser):
    roms = {}
    for value in values:
        name, separator, filename = value.partition("=")
        if not separator or name not in GAMES or name in roms or not filename:
            parser.error(f"expected one NAME=FILE for each of: {', '.join(GAMES)}")
        rom = Path(filename).expanduser().resolve()
        if not rom.is_file():
            parser.error(f"ROM does not exist: {rom}")
        roms[name] = rom
    if set(roms) != set(GAMES):
        parser.error(f"expected one ROM for each of: {', '.join(GAMES)}")
    return roms


def build_command(idf, name, rom, output, defaults):
    build = output / name / "build"
    return [idf, "-B", str(build), "-D", "IDF_TARGET=esp32p4",
            "-D", f"SDKCONFIG={build / 'sdkconfig'}",
            "-D", f"SDKCONFIG_DEFAULTS={';'.join(map(str, defaults))}",
            "-D", f"GPSP_ROM_PATH={rom}", "build", "merge-bin"]


def bench_command(name, output, runs, emulator, watchdog):
    return [sys.executable, str(REPO / "tools" / "bench-espemu.py"),
            "--build-dir", f"{name}={output / name / 'build'}",
            "--runs", str(runs), "--output-dir", str(output / name / "profile"),
            "--emu", emulator, "--watchdog", str(watchdog)]


def run_logged(command, path, espemu_build=False):
    print(f"Running: {path}", flush=True)
    environment = os.environ.copy()
    if espemu_build:
        environment["GPSP_BUILD_ESPEMU"] = "1"
    with path.open("w", encoding="utf-8") as log:
        status = subprocess.run(command, cwd=REPO, stdout=log,
                                stderr=subprocess.STDOUT, check=False,
                                env=environment).returncode
    if status:
        print(f"Failed ({status}); see {path}", file=sys.stderr)
        print("\n".join(path.read_text(encoding="utf-8", errors="replace")
                        .splitlines()[-20:]), file=sys.stderr)
        raise SystemExit(status)


def write_summary_csv(output, suite):
    rows = []
    for name in GAMES:
        profile = output / name / "profile"
        summary = json.loads((profile / "summary.json").read_text(encoding="utf-8"))
        manifest = json.loads((profile / "manifest.json").read_text(encoding="utf-8"))
        result = summary["builds"][name]
        row = {
            "game": name, "created_utc": suite["created_utc"],
            "detail": suite["detail"], "runs": result["runs"],
            "frames": suite["frames"],
            "code_bytes": suite["code_bytes"],
            "phase_work": suite["phase_work"],
            "rom_sha256": suite["games"][name]["rom_sha256"],
            "git_commit": manifest["git_commit"],
            "source_tree_sha256": manifest["source_tree_sha256"],
            "run_us_per_frame_median": result["run_us_per_frame_median"],
            "capacity_fps": result["capacity_fps"],
        }
        rows.append(row)
    fields = (
        "game", "created_utc", "detail", "code_bytes", "phase_work",
        "runs", "frames", "rom_sha256", "git_commit", "source_tree_sha256",
        "run_us_per_frame_median", "capacity_fps",
    )
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", action="append", required=True,
                        metavar="NAME=FILE", help="repeat for metroid, mario_kart, kirby")
    parser.add_argument("--detail", choices=("none", "jit", "video"),
                        default="none", help="optional diagnostic probes")
    parser.add_argument("--lookup", action="store_true",
                        help="count JIT lookup hits, misses, and translation time")
    parser.add_argument("--code-bytes", action="store_true",
                        help="count translated ROM code bytes and overlapping translations")
    parser.add_argument("--phase-work", action="store_true",
                        help="count scheduler, DMA, and sound work per frame")
    parser.add_argument("--rom-cache-kib", type=int,
                        help="override the block JIT ROM cache size in KiB")
    parser.add_argument("--frames", type=int, default=5120,
                        help="frames per run (64..5120; default: 5120)")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--watchdog", type=int, default=600)
    parser.add_argument("--emu", default="esp-emu")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.runs < 1 or args.watchdog < 10:
        parser.error("--runs must be positive and --watchdog at least 10 seconds")
    if args.rom_cache_kib is not None and not 64 <= args.rom_cache_kib <= 2048:
        parser.error("--rom-cache-kib must be between 64 and 2048")
    if not 64 <= args.frames <= 5120:
        parser.error("--frames must be between 64 and 5120")
    roms = parse_roms(args.rom, parser)
    idf = shutil.which("idf.py")
    emulator = shutil.which(args.emu)
    if not idf or not emulator:
        parser.error("idf.py and esp-emu must be available in PATH (or set --emu)")
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output_dir or REPO / "profiles" / f"three-games-{timestamp}").resolve()
    if output.exists() and not output.is_dir():
        parser.error(f"output path is not a directory: {output}")
    if output.exists() and any(output.iterdir()):
        parser.error(f"output directory is not empty: {output}")
    defaults = [REPO / name for name in DEFAULTS]
    if any(not path.is_file() for path in defaults):
        parser.error("a benchmark config fragment is missing")

    output.mkdir(parents=True, exist_ok=True)
    frame_defaults = output / "frames.defaults"
    frame_defaults.write_text(
        f"CONFIG_GPSP_BENCHMARK_FRAMES={args.frames}\n", encoding="utf-8")
    defaults.append(frame_defaults)
    probe_defaults = output / "probes.defaults"
    probes = {
        "GPSP_BENCHMARK_JIT_DETAIL": args.detail == "jit",
        "GPSP_BENCHMARK_VIDEO_DETAIL": args.detail == "video",
        "GPSP_BENCHMARK_JIT_LOOKUP": args.lookup or args.code_bytes,
        "GPSP_BENCHMARK_JIT_MISS_CLASS": args.code_bytes,
        "GPSP_BENCHMARK_JIT_CODE_BYTES": args.code_bytes,
        "GPSP_BENCHMARK_PHASE_WORK": args.phase_work,
        "GPSP_BENCHMARK_INSTRET": False,
    }
    probe_defaults.write_text("".join(
        f"CONFIG_{name}={'y' if enabled else 'n'}\n"
        for name, enabled in probes.items()), encoding="utf-8")
    defaults.append(probe_defaults)
    if args.rom_cache_kib is not None:
        cache_defaults = output / "rom-cache.defaults"
        cache_defaults.write_text(
            f"CONFIG_GPSP_RV32_ROM_CACHE_KIB={args.rom_cache_kib}\n",
            encoding="utf-8")
        defaults.append(cache_defaults)
    suite = {
        "schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
        "detail": args.detail, "lookup": args.lookup,
        "code_bytes": args.code_bytes, "phase_work": args.phase_work,
        "rom_cache_kib": args.rom_cache_kib,
        "frames": args.frames, "runs": args.runs,
        "config_defaults": [str(path) for path in defaults], "games": {},
    }
    for name in GAMES:
        rom = roms[name]
        game = output / name
        game.mkdir()
        build = build_command(idf, name, rom, output, defaults)
        bench = bench_command(name, output, args.runs, emulator, args.watchdog)
        suite["games"][name] = {
            "rom_source": str(rom), "rom_sha256": sha256_file(rom),
            "build_command": build, "bench_command": bench,
            "build_dir": str(game / "build"),
            "profile_dir": str(game / "profile"),
        }
    (output / "suite.json").write_text(json.dumps(suite, indent=2) + "\n",
                                       encoding="utf-8")

    for name in GAMES:
        game = output / name
        print(f"{name}: build", flush=True)
        run_logged(suite["games"][name]["build_command"], game / "build.log",
                   espemu_build=True)
        config = game / "build" / "config" / "sdkconfig.h"
        if "#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 400" not in \
                config.read_text(encoding="utf-8").splitlines():
            raise SystemExit(f"{name}: expected a 400 MHz build; see {config}")
        print(f"{name}: benchmark", flush=True)
        run_logged(suite["games"][name]["bench_command"], game / "bench.log")
        summary = json.loads((game / "profile" / "summary.json").read_text())
        result = summary["builds"][name]
        print(f"{name}: {result['run_us_per_frame_median']:.1f} us/frame; "
              f"{result['capacity_fps']:.1f} capacity FPS", flush=True)
    write_summary_csv(output, suite)
    print(f"Saved suite and per-game manifests: {output}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

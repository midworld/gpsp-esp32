#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Capture and check a fixed-frame benchmark from a physical P4 board."""

import argparse
import csv
import hashlib
import importlib.util
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_parser():
    path = REPO / "tools" / "bench-espemu.py"
    spec = importlib.util.spec_from_file_location("bench_espemu", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_config(path, parser_module):
    values = parser_module.defines(path)
    for name in ("CONFIG_GPSP_BENCHMARK",
                 "CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT"):
        if values.get(name) != "1":
            raise ValueError(f"{path}: expected {name}=1")
    if values.get("CONFIG_ESPEMU") == "1":
        raise ValueError(f"{path}: expected a physical P4 build")
    if values.get("CONFIG_IDF_TARGET_ESP32P4") != "1":
        raise ValueError(f"{path}: expected ESP32-P4 target")
    frames = int(values["CONFIG_GPSP_BENCHMARK_FRAMES"])
    if not 64 <= frames <= 5120:
        raise ValueError(f"{path}: frame count outside 64..5120")
    return values, frames


def capture(port, baud, timeout, frames, path):
    try:
        import serial
    except ImportError as error:
        raise ValueError(
            "pyserial is required; run from the ESP-IDF environment") from error

    marker = re.compile(rb"\bBENCH_DONE frames=" + str(frames).encode() +
                        rb"\b[^\r\n]*(?:\r|\n)")
    start = time.monotonic()
    recent = b""
    with path.open("wb") as output, serial.Serial(port, baud, timeout=0.25) as device:
        device.reset_input_buffer()
        device.dtr = False
        device.rts = True
        time.sleep(0.15)
        device.rts = False
        print(f"Reset {port}; capturing {frames} frames to {path}", flush=True)
        last_report = start
        while time.monotonic() - start < timeout:
            data = device.read(4096)
            if data:
                output.write(data)
                candidate = recent + data
                recent = candidate[-512:]
                if marker.search(candidate):
                    output.flush()
                    return
            now = time.monotonic()
            if now - last_report >= 15:
                output.flush()
                print(f"Waiting {int(now - start)}s; {output.tell()} bytes captured",
                      flush=True)
                last_report = now
    raise ValueError(f"BENCH_DONE frames={frames} was not seen in {timeout}s")


def average_ms(windows, field):
    frames = sum(int(row["frames"]) for row in windows)
    return sum(int(row[field]) for row in windows) / frames / 1000 if frames else None


def rom_info(log_text, rom, copied_rom, expected_source=None):
    text = re.sub(r"\x1b\[[0-9;]*m", "", log_text)
    started = re.findall(r"\bExecution started: [^\r\n]*ROM=([^\r\n]+)", text)
    if len(started) != 1:
        raise ValueError("expected one ROM startup line in serial log")
    path = started[0]
    if path == "/storage/game.gba":
        source = "flash"
    elif path.startswith("/sdcard/"):
        source = "sd"
    else:
        raise ValueError(f"unsupported ROM path: {path}")
    if expected_source and source != expected_source:
        raise ValueError(f"expected ROM source {expected_source}, got {source}: {path}")
    if source == "sd" and rom is None:
        raise ValueError("SD runs require --rom for size check and provenance")

    loaded = re.findall(r"\bROM ready: ([^\r\n]+) \((\d+) bytes\)", text)
    size = None
    if loaded:
        if len(loaded) != 1 or loaded[0][0] != path:
            raise ValueError("ROM ready and startup paths differ or are repeated")
        size = int(loaded[0][1])
    selected = rom if rom is not None else copied_rom
    digest = sha256(selected) if selected.is_file() else None
    # An SD ROM is independent of an old storage image in the build directory.
    if source == "flash" and rom is not None and copied_rom.is_file():
        if digest != sha256(copied_rom):
            raise ValueError(f"ROM differs from built storage image: {copied_rom}")
    if size is not None and selected.is_file() and size != selected.stat().st_size:
        raise ValueError("loaded ROM size differs from the selected ROM")
    return {"rom_sha256": digest, "loaded_rom_path": path,
            "rom_source": source, "loaded_rom_bytes": size}


def conditions(options):
    host = options.get("CONFIG_GPSP_SERVER_HOST", '""')
    probes = ("JIT_DETAIL", "JIT_LOOKUP", "JIT_MISS_CLASS", "JIT_CODE_BYTES",
              "PHASE_WORK", "VIDEO_DETAIL", "INSTRET")
    result = {
        "cpu_mhz": int(options["CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ"]),
        "psram_mhz": int(options["CONFIG_SPIRAM_SPEED"], 0)
                     if "CONFIG_SPIRAM_SPEED" in options else None,
        "perf_optimization": options.get("CONFIG_COMPILER_OPTIMIZATION_PERF") == "1",
        "unthrottled": options.get("CONFIG_GPSP_ESPEMU_UNTHROTTLED") == "1",
        "fresh_save": options.get("CONFIG_GPSP_BENCHMARK_FRESH_SAVE") == "1",
        "held_buttons": int(options.get("CONFIG_GPSP_BENCHMARK_HOLD_BUTTONS", "0x0"), 0),
        "status": options.get("CONFIG_GPSP_STATUS") == "1",
        "lcd": options.get("CONFIG_GPSP_LCD") == "1",
        "audio": options.get("CONFIG_GPSP_AUDIO") == "1",
        "usb_pad": options.get("CONFIG_GPSP_USB_PAD") == "1",
        "receiver_enabled": host != '""',
        "receiver_host_sha256": hashlib.sha256(host.encode()).hexdigest(),
        "receiver_port": options.get("CONFIG_GPSP_SERVER_PORT"),
        "wifi_configured": options.get("CONFIG_GPSP_WIFI_SSID", '""') != '""',
    }
    result.update({probe.lower(): options.get("CONFIG_GPSP_BENCHMARK_" + probe) == "1"
                   for probe in probes})
    return result


def compare(current, previous_path):
    previous = json.loads((previous_path / "result.json").read_text())
    if not current["rom_sha256"] or not previous["rom_sha256"]:
        raise ValueError("comparison needs ROM hashes; pass --rom for each run")
    if current["rom_sha256"] != previous["rom_sha256"]:
        raise ValueError("comparison ROM hashes differ")
    if current["frames"] != previous["frames"]:
        raise ValueError("comparison frame counts differ")
    if current.get("rom_source", "flash") != previous.get("rom_source", "flash"):
        raise ValueError("comparison ROM sources differ")
    # Older results lack later on/off probes, which were off then.
    differences = [key for key, value in current["conditions"].items()
                   if value != previous["conditions"].get(
                       key, False if isinstance(value, bool) else None)]
    if differences:
        raise ValueError("comparison conditions differ: " + ", ".join(differences))
    checksums = [(a["first"], a["last"]) for a, b in
                 zip(current["checksums"], previous["checksums"])
                 if a != b]
    if len(current["checksums"]) != len(previous["checksums"]):
        raise ValueError("comparison window counts differ")
    return {
        "reference": str(previous_path.resolve()),
        "checksum_mismatch_windows": checksums,
        "warm_run_ms_per_frame_delta":
            current["warm_run_ms_per_frame"] - previous["warm_run_ms_per_frame"]
            if current["warm_run_ms_per_frame"] is not None and
               previous["warm_run_ms_per_frame"] is not None else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port; the board is reset before capture")
    source.add_argument("--log-file", type=Path, help="check an existing serial log")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--rom", type=Path,
                        help="ROM on the board, for size check and provenance")
    parser.add_argument("--rom-source", choices=("sd", "flash"),
                        help="require this ROM source; otherwise detect from the log")
    parser.add_argument("--compare-with", type=Path,
                        help="prior bench-p4 output directory")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.baud <= 0 or args.timeout <= 0:
        parser.error("baud and timeout must be positive")

    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    config = build / "config" / "sdkconfig.h"
    firmware = build / "gpsp_esp32.bin"
    copied_rom = build / "esp-idf" / "main" / "storage" / "game.gba"
    rom = args.rom.resolve() if args.rom else None
    for path in (config, firmware):
        if not path.is_file():
            parser.error(f"missing build artifact: {path}")
    if args.rom and not rom.is_file():
        parser.error(f"ROM not found: {rom}")
    if args.rom_source == "sd" and rom is None:
        parser.error("--rom-source sd requires --rom")
    parser_module = load_parser()
    try:
        options, frames = read_config(config, parser_module)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    if args.rom_source == "sd" and options.get("CONFIG_GPSP_SD_CARD") != "1":
        parser.error("--rom-source sd requires CONFIG_GPSP_SD_CARD=y")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    log = output / "firmware.log"
    try:
        if args.port:
            capture(args.port, args.baud, args.timeout, frames, log)
        else:
            log.write_bytes(args.log_file.read_bytes())
        log_text = log.read_text(errors="replace")
        loaded_rom = rom_info(log_text, rom, copied_rom, args.rom_source)
        records, done = parser_module.parse_log(
            log_text, frames, options, full_video=False)
        windows = records["BENCH_SLICE"]
        result = {
            "created_utc": datetime.now(timezone.utc).isoformat(),
            "build_dir": str(build),
            "frames": frames,
            **loaded_rom,
            "firmware_sha256": sha256(firmware),
            "sdkconfig_h_sha256": sha256(config),
            "conditions": conditions(options),
            "done": done,
            "checksums": [{"first": row["first"], "last": row["last"],
                           "checksum": row["checksum"].lower()} for row in windows],
            "run_ms_per_frame": average_ms(windows, "run_us"),
            "warm_run_ms_per_frame": average_ms(windows[1:], "run_us"),
        }
        (output / "records.json").write_text(json.dumps(records, indent=2) + "\n")
        with (output / "slices.csv").open("w", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=windows[0].keys())
            writer.writeheader()
            writer.writerows(windows)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"Validated {len(windows)} windows; warm run "
              f"{result['warm_run_ms_per_frame']} ms/frame")
        if args.compare_with:
            comparison = compare(result, args.compare_with)
            (output / "comparison.json").write_text(
                json.dumps(comparison, indent=2) + "\n")
            print(f"Checksum mismatches: "
                  f"{len(comparison['checksum_mismatch_windows'])}; "
                  f"warm delta: {comparison['warm_run_ms_per_frame_delta']} ms/frame")
    except (OSError, ValueError, KeyError) as error:
        print(f"Benchmark failed: {error}; raw log: {log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

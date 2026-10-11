#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check RAM blocks translated again in the next code chunk."""

import argparse
import re
import socket
import struct
import subprocess
import threading
import time
from collections import deque
from pathlib import Path

BIG_PC = 0x02000000
THUMB_FIRST, THUMB_COUNT, THUMB_STRIDE = 0x02010000, 96, 0x400
THUMB_END = THUMB_FIRST + THUMB_COUNT * THUMB_STRIDE
OLD_RESERVE = 32 * 1024
LIMIT_GAP = 2 * 1024


def translations(events, pc_range):
    return [i for i, (kind, data) in enumerate(events)
            if kind == "translate" and pc_range[0] <= data["pc"] < pc_range[1]]


def retried(events, index, chunk):
    """The retry event before a translation, or None."""
    data = events[index][1]
    previous = events[index - 1] if index else None
    if not previous or previous[0] != "retry":
        return None
    if data["native"] % chunk:
        raise RuntimeError(
            f"block {data['pc']:08x} was retried but starts at {data['native']}")
    return previous[1]


def check(events, chunk):
    if not events:
        raise RuntimeError("no RAM cache events")
    flushes = {data["flushes"] for kind, data in events if kind == "translate"}
    if len(flushes) != 1:
        raise RuntimeError(f"RAM cache flushed: counts {sorted(flushes)}")

    big = translations(events, (BIG_PC, BIG_PC + 4))
    if len(big) != 1:
        raise RuntimeError(f"expected one large ARM translation, got {len(big)}")
    data = events[big[0]][1]
    size = data["used"] - data["native"]
    retry = retried(events, big[0], chunk)
    if not retry:
        raise RuntimeError(f"the {size}-byte ARM block was not retried")
    # The 32 KiB reserve would have kept it in the chunk it overflowed.
    if retry["free"] < OLD_RESERVE or size <= retry["free"] - LIMIT_GAP:
        raise RuntimeError(
            f"fixture missed the chunk limit: {size} bytes, "
            f"{retry['free']} free at the first try")
    arm = f"{size}-byte ARM block retried with {retry['free']} bytes free"

    thumb = translations(events, (THUMB_FIRST, THUMB_END))
    pcs = {events[i][1]["pc"] for i in thumb}
    if pcs != set(range(THUMB_FIRST, THUMB_END, THUMB_STRIDE)):
        raise RuntimeError(f"{len(pcs)} of {THUMB_COUNT} Thumb blocks translated")
    thumb = [retry for retry in (retried(events, i, chunk) for i in thumb)
             if retry]
    if not thumb:
        raise RuntimeError("no Thumb block reached a chunk limit")
    retries = sum(kind == "retry" for kind, _ in events)
    return f"{arm}; {len(thumb)} Thumb retries", retries


def windows(lines, name, frames):
    """Report windows must cover frames 1..frames once, in order."""
    spans = [tuple(int(value) for value in
                   re.search(r"\bfirst=(\d+) last=(\d+)(?=\s)", line).groups())
             for line in lines]
    expected = 1
    for first, last in spans:
        if first != expected or last < first:
            raise RuntimeError(f"{name} windows {spans} do not cover 1-{frames}")
        expected = last + 1
    if expected != frames + 1:
        raise RuntimeError(f"{name} windows {spans} do not cover 1-{frames}")
    return spans


def check_reports(engines, caches, retries, frames):
    """Every benchmark window, through BENCH_DONE."""
    if windows(engines, "BENCH_ENGINE", frames) != \
            windows(caches, "BENCH_JIT_CACHE", frames):
        raise RuntimeError("BENCH_ENGINE and BENCH_JIT_CACHE windows differ")
    if any("ready=true" not in line for line in engines):
        raise RuntimeError(f"JIT stopped: {engines[-1].strip()}")
    fields = [dict((key, int(value)) for key, value in
                   re.findall(r"\b(\w+)=(\d+)", line)) for line in caches]
    if any(cache["flushes"] or cache["ram_flushes"] for cache in fields):
        raise RuntimeError("a code cache flushed: "
                           + "; ".join(line.strip() for line in caches))
    counted = sum(cache["chunk_retries"] for cache in fields)
    # The counter also includes ROM cache retries.
    if counted < retries:
        raise RuntimeError(f"chunk_retries={counted}, {retries} RAM retries logged")
    return f"{len(caches)} windows without a flush; chunk_retries={counted}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--emu", type=Path, required=True)
    parser.add_argument("--port", type=int, default=24680)
    parser.add_argument("--log", type=Path, help="save emulator output")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    config = (build / "config/sdkconfig.h").read_text()
    chunk = re.search(r"#define CONFIG_GPSP_RV32_CODE_CHUNK_KIB (\d+)", config)
    if not chunk or int(chunk[1]) != 64:
        parser.error("build must use 64 KiB code chunks")
    chunk = int(chunk[1]) * 1024
    count = re.search(r"#define CONFIG_GPSP_BENCHMARK_FRAMES (\d+)", config)
    if "#define CONFIG_GPSP_BENCHMARK 1" not in config or not count or \
            int(count[1]) < 160 or \
            "#define CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT 1" not in config:
        parser.error("build must enable a benchmark of at least 160 frames "
                     "that stops after its report")
    count = int(count[1])

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    listener.settimeout(15)
    process = subprocess.Popen([
        str(args.emu.resolve()), "--chip", "esp32p4", "--firmware",
        str(build / "merged-binary.bin"), "--elf", str(build / "gpsp_esp32.elf"),
        "--psram-size", "32M", "--net", "user", "--timeout", "90s",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    engine_line = []
    cache_lines = []
    done_line = []
    done = threading.Event()
    ram_events = []
    output_tail = deque(maxlen=20)
    output_lines = []

    def read_output():
        for line in process.stdout:
            output_tail.append(line)
            output_lines.append(line)
            event = re.search(r"test ram (translate|retry): (.*)", line)
            if event:
                addresses = {"pc", "emitted_end", "scan_end"}
                data = {key: int(value, 16 if key in addresses else 10)
                        for key, value in re.findall(r"(\w+)=([0-9a-f]+)", event[2])}
                ram_events.append((event[1], data))
            if "BENCH_ENGINE" in line:
                engine_line.append(line)
            elif "BENCH_JIT_CACHE" in line:
                cache_lines.append(line)
            elif "BENCH_DONE" in line:
                done_line.append(line)
                done.set()

    output_thread = threading.Thread(target=read_output, daemon=True)
    output_thread.start()
    try:
        client, _ = listener.accept()
        with client:
            client.settimeout(1)
            deadline = time.monotonic() + 120
            pending = bytearray()
            frames = 0
            last_pixels = (0, 0)
            # Read until BENCH_DONE and a quiet second, so extra frames fail.
            while True:
                try:
                    packet = client.recv(131072)
                except socket.timeout:
                    if done.is_set() and frames >= count:
                        break
                    if time.monotonic() > deadline:
                        raise RuntimeError(f"timed out after {frames} frames")
                    continue
                if not packet:
                    if done.is_set() and frames >= count:
                        break
                    raise RuntimeError(
                        f"emulator closed after {frames} frames:\n"
                        + "".join(output_tail))
                pending.extend(packet)
                while len(pending) >= 24:
                    magic, version, kind, size, _, _ = struct.unpack_from(
                        ">IHHIIQ", pending)
                    if magic != 0x47505350 or version != 1 or size > 100000:
                        raise RuntimeError("invalid video packet")
                    if len(pending) < 24 + size:
                        break
                    payload = pending[24:24 + size]
                    if kind == 1:
                        client.sendall(struct.pack(
                            ">IHHIIQHH", 0x47505350, 1, 3, 4, 0, 0, 0, 0))
                    elif kind == 2:
                        frames += 1
                        if frames > count:
                            raise RuntimeError(f"more than {count} frames")
                        pixels = struct.unpack_from("<2H", payload, 8)
                        # Black until a check finishes; green after it.
                        if any(pixel not in (0, 0x07c0) for pixel in pixels) or \
                                any(old and not new for old, new in
                                    zip(last_pixels, pixels)):
                            raise RuntimeError(
                                f"frame {frames}: pixels "
                                + ",".join(f"{pixel:04x}" for pixel in pixels))
                        last_pixels = pixels
                    del pending[:24 + size]
        if last_pixels != (0x07c0,) * 2:
            raise RuntimeError(f"the checks did not finish in {count} frames")
        if not re.search(rf"\bframes={count}(?=\s)", done_line[-1]):
            raise RuntimeError(f"BENCH_DONE does not match: {done_line[-1].strip()}")
        if any("block lookup failed" in line for line in output_lines):
            raise RuntimeError("a block lookup failed")
        result, retries = check(ram_events, chunk)
        print("RV32 chunk retry PASS: 2 green pixels; " + result + "; "
              + check_reports(engine_line, cache_lines, retries, count))
    finally:
        process.terminate()
        process.wait(timeout=5)
        output_thread.join(timeout=1)
        process.stdout.close()
        listener.close()
        if args.log:
            args.log.write_text("".join(output_lines))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check IWRAM entry aliases, mirrors, and self-modifying code."""

import argparse
import re
import socket
import struct
import subprocess
import threading
from collections import deque
from pathlib import Path


def find_markers(events, markers):
    positions = []
    for pc in markers:
        matches = [i for i, (kind, data) in enumerate(events)
                   if kind == "translate" and data["pc"] == pc]
        if not matches:
            raise RuntimeError(f"missing test phase marker {pc:08x}")
        if len(matches) > 1:
            flushes = [events[i][1]["flushes"] for i in matches]
            raise RuntimeError(
                f"test phase marker {pc:08x} translated {len(matches)} times; "
                f"RAM flush counts: {flushes}")
        positions.append(matches[0])
    if positions != sorted(positions):
        raise RuntimeError("test phase markers ran out of order")
    return positions


def code_grew(events, first, last, stable_used):
    """RAM code allocated between two markers. A new block starts at the
    next four-byte boundary, so the end marker may start up to 3 bytes past
    the last used byte."""
    if events[last][1]["native"] > (stable_used + 3) & ~3:
        return True
    return any(data.get("used", 0) > stable_used
               for _, data in events[first + 1:last])


def check_flushes(events, first, last, name):
    before, after = (events[i][1]["flushes"] for i in (first, last))
    if before != after:
        raise RuntimeError(
            f"RAM cache flushed during the {name} test: {before} -> {after}")


def check_switches(events):
    tail_pc, slot_pc, slots_pc = 0x03006000, 0x03006014, 0x03006200
    begin, warm, end, slots_begin, slots_end = find_markers(
        events, range(0x03006100, 0x03006114, 4))
    check_flushes(events, begin, end, "tail")
    check_flushes(events, slots_begin, slots_end, "exit")
    tail_events = events[begin + 1:end]
    translations = [data for kind, data in tail_events
                    if kind == "translate" and data["pc"] == tail_pc]
    if not 1 <= len(translations) <= 2:
        raise RuntimeError(
            f"expected one or two tail translations, got {len(translations)}")
    for data in translations:
        if data["emitted_end"] != slot_pc or data["scan_end"] <= slot_pc:
            raise RuntimeError("the learned slot was not in an untranslated tail")
    # Marker allocation is outside the measured interval at either end.
    if code_grew(events, warm, end, events[warm][1]["used"]):
        raise RuntimeError("RAM code grew after the first tail NOP/B cycle")
    revivals = [data for kind, data in tail_events
                if kind == "revive" and data["pc"] == tail_pc]
    native = {data["native"] for data in translations}
    if any(data["native"] not in native for data in revivals):
        raise RuntimeError("a tail restore did not reuse previously translated code")
    learned = [data for kind, data in events
               if kind == "switch" and data["pc"] == slot_pc]
    if len(learned) != 1:
        raise RuntimeError(f"expected one slot learning, got {len(learned)}")
    patches = [data for kind, data in tail_events
               if kind == "patch" and data["pc"] == slot_pc and data["switch"]]
    if len(patches) != 64 or len({data["native"] for data in patches}) != 1:
        raise RuntimeError("the 64 slot writes did not patch the same native code")

    exit_events = events[slots_begin + 1:slots_end]
    scans = [data for kind, data in exit_events
             if kind == "scan" and data["pc"] == slots_pc]
    limits = {data["limit"] for data in scans}
    if len(limits) != 1:
        raise RuntimeError("missing or inconsistent exit-limit scans")
    limit = limits.pop()
    learned_slots = {data["pc"] for kind, data in exit_events
                     if kind == "switch" and slots_pc <= data["pc"] < slots_pc + 128}
    if len(learned_slots) + 3 <= limit:
        raise RuntimeError(
            f"exit test learned only {len(learned_slots)} slots; "
            f"three branches do not exceed the {limit}-exit limit")
    if any(data["switches"] + data["exits"] > limit for data in scans):
        raise RuntimeError("the scanner exceeded the exit limit")
    # The fixture has 32 NOP candidates, CMP, three B, MOV, and BX.
    fixture_end = slots_pc + (32 + 6) * 4
    if not any(data["switches"] + 3 > limit and
               data["switches"] + data["exits"] <= limit and
               data["end"] < fixture_end for data in scans):
        raise RuntimeError("no block split with enough learned slots to exceed the exit limit")
    return (f"{len(translations)} tail translations, {len(revivals)} restores; "
            f"no RAM flush or code growth after warmup; 64 slot patches; "
            f"{len(learned_slots)} exit-test slots and a checked block split")


def check_cut(events, name, func_pc, body_pc, end_pc, markers):
    begin, end = find_markers(events, markers)
    check_flushes(events, begin, end, name)
    # Dormant-unit revival can hide invalidation from translation counts.
    rebuilt = [(kind, data["pc"]) for kind, data in events[begin + 1:end]
               if kind in ("translate", "revive") and
               func_pc <= data["pc"] < end_pc]
    writer = [pc for _, pc in rebuilt if pc < body_pc]
    if writer:
        raise RuntimeError(
            f"{name}: the self-writing code was rebuilt {len(writer)} more times")
    translated = [pc for kind, pc in rebuilt if kind == "translate"]
    if len(translated) > 2:
        raise RuntimeError(
            f"{name}: the rewritten ops were translated {len(translated)} times")
    # Marker allocation is outside the measured interval at either end.
    if code_grew(events, begin, end, events[begin][1]["used"]):
        raise RuntimeError(f"{name}: RAM code grew after warmup")
    return f"{name} {len(translated)}"


def check_dynamic(events):
    begin, end = find_markers(events, (0x03006124, 0x03006128))
    check_flushes(events, begin, end, "dynamic immediate")
    # Learned immediate writes must not trigger SMC handling.
    alerts = [data["pc"] for kind, data in events[begin + 1:end]
              if kind == "alert"]
    if alerts:
        raise RuntimeError(
            f"dynamic immediate writes handled {len(alerts)} SMC alerts")
    changed = [(kind, data["pc"]) for kind, data in events[begin + 1:end]
               if kind in ("translate", "revive", "patch", "write") and
               0x03006600 <= 0x03000000 | (data["pc"] & 0x7fff) < 0x03006610
               and data["pc"] >> 24 == 3]
    if changed:
        raise RuntimeError(
            f"dynamic immediate writes changed code: {changed[:4]}")
    # Marker allocation is outside the measured interval at either end.
    if code_grew(events, begin, end, events[begin][1]["used"]):
        raise RuntimeError("dynamic immediate: RAM code grew after learning")
    return "no SMC handling or new code for 16 learned immediate writes"


def check_switch_b(events):
    begin, end = find_markers(events, (0x0300612c, 0x03006130))
    check_flushes(events, begin, end, "B-state slot")
    # The pixel 11 slot was learned just before the first marker.
    slot = [data["pc"] for kind, data in events[:begin]
            if kind == "switch"][-1]
    head = slot - 128
    window = events[begin + 1:end]
    # The head is translated while the slot is B; NOP writes must patch it.
    heads = [kind for kind, data in window
             if kind in ("translate", "revive") and data["pc"] == head]
    if heads != ["translate"]:
        raise RuntimeError(f"B-state slot: expected one head translation, got {heads}")
    patches = [data for kind, data in window
               if kind == "patch" and data["pc"] == slot and data["switch"]]
    if len(patches) != 3:
        raise RuntimeError(f"B-state slot: expected 3 slot patches, got {len(patches)}")
    return "3 patches of a slot translated as B"


def check_switch_back(events):
    head, slot = 0x03006240, 0x03006250
    begin, end = find_markers(events, (0x03006134, 0x03006138))
    check_flushes(events, begin, end, "backward slot")
    window = events[begin + 1:end]
    # The B targets its own unit; its link must still move on NOP writes.
    heads = [kind for kind, data in window
             if kind in ("translate", "revive") and data["pc"] == head]
    if heads != ["translate"]:
        raise RuntimeError(f"backward slot: expected one head translation, got {heads}")
    patches = [data for kind, data in window
               if kind == "patch" and data["pc"] == slot and data["switch"]]
    if len(patches) != 4:
        raise RuntimeError(f"backward slot: expected 4 slot patches, got {len(patches)}")
    return "4 patches of a backward B slot"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--emu", type=Path, required=True)
    parser.add_argument("--chip", choices=("esp32p4", "esp32s31"), default="esp32p4")
    parser.add_argument("--psram-size", choices=("16M", "32M"), default="32M")
    parser.add_argument("--port", type=int, default=24680)
    parser.add_argument("--log", type=Path, help="save emulator output")
    parser.add_argument("--expect-fault", choices=("none", "once", "always"),
                        default="none",
                        help="build made with GPSP_TEST_SYNC_FAULT=1 or 2")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    config = (build / "config/sdkconfig.h").read_text()
    count = re.search(r"#define CONFIG_GPSP_BENCHMARK_FRAMES (\d+)", config)
    if "#define CONFIG_GPSP_BENCHMARK 1" not in config or not count or int(count[1]) < 160:
        parser.error("build must enable a benchmark of at least 160 frames")

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    listener.settimeout(15)
    process = subprocess.Popen([
        str(args.emu.resolve()), "--chip", args.chip, "--firmware",
        str(build / "merged-binary.bin"), "--elf", str(build / "gpsp_esp32.elf"),
        "--psram-size", args.psram_size, "--net", "user", "--timeout", "90s",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    engine_line = []
    engine_seen = threading.Event()
    regions_line = []
    regions_seen = threading.Event()
    unit_limit = threading.Event()
    ram_events = []
    fault_armed = threading.Event()
    output_tail = deque(maxlen=20)
    output_lines = []

    def read_output():
        for line in process.stdout:
            output_tail.append(line)
            output_lines.append(line)
            covered = re.search(r"test unit limit: live=(\d+)", line)
            if covered and int(covered[1]) >= 65:
                unit_limit.set()
            if "test sync fault armed" in line:
                fault_armed.set()
            event = re.search(r"test ram (\w+): (.*)", line)
            if event:
                addresses = {"pc", "slot", "end", "emitted_end", "scan_end"}
                data = {key: int(value, 16 if key in addresses else 10)
                        for key, value in re.findall(r"(\w+)=([0-9a-f]+)", event[2])}
                ram_events.append((event[1], data))
            if "BENCH_ENGINE" in line:
                engine_line.append(line)
                engine_seen.set()
            elif "BENCH_JIT_REGIONS" in line:
                regions_line.append(line)
                regions_seen.set()

    output_thread = threading.Thread(target=read_output, daemon=True)
    output_thread.start()
    try:
        client, _ = listener.accept()
        with client:
            client.settimeout(10)
            pending = bytearray()
            frames = 0
            while frames < 160:
                packet = client.recv(131072)
                if not packet:
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
                        if frames >= 2:
                            pixels = struct.unpack_from("<18H", payload, 8)
                            if pixels != (0x07c0,) * 18:
                                raise RuntimeError(
                                    f"frame {frames}: expected 18 green pixels, got "
                                    + ",".join(f"{pixel:04x}" for pixel in pixels))
                    del pending[:24 + size]
        if not engine_seen.wait(10):
            raise RuntimeError("missing BENCH_ENGINE after frame 160")
        if args.expect_fault != "none" and not fault_armed.is_set():
            raise RuntimeError("the test sync fault was not armed")
        if args.expect_fault == "always":
            # Every later publish fails, so the JIT gives way to the
            # interpreter and the pixels come from interpreted code.
            if "ready=false" not in engine_line[-1]:
                raise RuntimeError(
                    f"JIT stayed active: {engine_line[-1].strip()}")
            print("RV32 RAM cache PASS: 18 green pixels; "
                  "interpreter after persistent sync faults")
            return
        engine = re.search(r"\bblocks=(\d+)\b", engine_line[-1])
        if not engine or int(engine.group(1)) == 0 or "ready=true" not in engine_line[-1]:
            raise RuntimeError(f"JIT did not run: {engine_line[-1].strip()}")
        if not regions_seen.wait(10):
            raise RuntimeError("missing BENCH_JIT_REGIONS after frame 160")
        regions = re.search(r"dispatch_iwram=(\d+)", regions_line[-1])
        if not regions or int(regions.group(1)) == 0:
            raise RuntimeError(f"IWRAM blocks did not run: {regions_line[-1].strip()}")
        # Require the overlapping live units, not just a saturated lookup.
        if not unit_limit.is_set():
            raise RuntimeError("no code write covered at least 65 live RAM units")
        switch_result = check_switches(ram_events)
        cut_result = ", ".join((
            check_cut(ram_events, "ARM self-write", 0x03006400, 0x0300641c,
                      0x03006430, (0x03006114, 0x03006118)),
            check_cut(ram_events, "Thumb self-write", 0x03006500, 0x03006514,
                      0x03006520, (0x0300611c, 0x03006120))))
        cut_result += " translations after warmup"
        print("RV32 RAM cache PASS: 18 green pixels; "
              + switch_result + "; " + cut_result + "; "
              + check_dynamic(ram_events) + "; "
              + check_switch_b(ram_events) + "; "
              + check_switch_back(ram_events) + "; "
              "IWRAM JIT blocks executed"
              + (" after a sync fault" if args.expect_fault == "once" else ""))
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

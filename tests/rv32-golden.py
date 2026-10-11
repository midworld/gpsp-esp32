#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compare generated RV32 code across builds.

Reads 32-bit GOLDEN words from an ESP-EMU log (see rv32-golden.h) and the ELF
of the same build. Recognized LUI address uses in ADDI, loads, stores and
JALR become code-buffer or ELF symbol names, so builds with other link
layouts compare equal.

  rv32-golden.py normalize LOG ELF          print normalized lines
  rv32-golden.py compare BASE_LOG BASE_ELF LOG ELF
  rv32-golden.py selftest
"""

import bisect
import os
import re
import subprocess
import sys

LINE = re.compile(r"^GOLDEN (\S+) (\d+) base=([0-9a-f]+) size=([0-9a-f]+) "
                  r"at=([0-9a-f]+)((?: [0-9a-f]{8})*)\r?$")


def nm_tool():
    tool = os.environ.get("RV32_NM")
    if tool:
        return tool
    for root, _, files in os.walk(os.path.expanduser(
            "~/.espressif/tools/riscv32-esp-elf")):
        if "riscv32-esp-elf-nm" in files:
            return os.path.join(root, "riscv32-esp-elf-nm")
    return "riscv32-esp-elf-nm"


def symbols(elf):
    out = subprocess.run([nm_tool(), "-S", "--defined-only", elf],
                         check=True, capture_output=True, text=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, _, name = parts
            table.append((int(addr, 16), max(int(size, 16), 1), name))
        elif len(parts) == 3:
            addr, _, name = parts
            table.append((int(addr, 16), 1, name))
    table.sort()
    return table


class Resolver:
    def __init__(self, table):
        self.table = table
        self.addrs = [t[0] for t in table]

    def name(self, value, base, size):
        if base <= value < base + size:
            return "code+%x" % (value - base)
        i = bisect.bisect_right(self.addrs, value) - 1
        while i >= 0 and self.addrs[i] + 0x10000 > value:
            addr, length, sym = self.table[i]
            if addr <= value < addr + length:
                return "%s+%x" % (sym, value - addr)
            i -= 1
        return None


def sext(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def normalize_words(words, base, size, resolver):
    out = ["%08x" % w for w in words]
    upper = {}                       # rd -> (index, value)
    for i, w in enumerate(words):
        op, rd = w & 0x7f, (w >> 7) & 31
        rs1 = (w >> 15) & 31
        if op == 0x37:               # LUI
            upper[rd] = (i, w & 0xfffff000)
            continue
        # Only an address-forming use: addi, a load, jalr or a store. Shifts
        # and other OP-IMM forms keep all their bits.
        f3 = (w >> 12) & 7
        imm = None
        if op == 0x03 or (op in (0x13, 0x67) and f3 == 0):
            imm = sext(w >> 20, 12)
        elif op == 0x23:
            imm = sext(((w >> 25) << 5) | ((w >> 7) & 31), 12)
        if imm is not None and rs1 in upper:
            j, high = upper[rs1]
            name = resolver.name((high + imm) & 0xffffffff, base, size)
            if name:
                out[j] = "lui(x%d)" % ((words[j] >> 7) & 31)
                mask = 0x01fff07f if op == 0x23 else 0x000fffff
                out[i] = "%08x@%s" % (w & mask, name)
        # Any write of a register ends its pending upper value; control
        # flow ends all of them (a callee or another path may change them).
        if op in (0x63, 0x67, 0x6f):
            upper.clear()
        elif op != 0x23 and rd in upper:
            del upper[rd]
    return out


def parse(log):
    records = []
    with open(log, errors="replace") as f:
        for raw in f:
            if not raw.startswith("GOLDEN "):
                continue
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                raise SystemExit("golden: malformed line in %s: %s"
                                 % (log, raw.strip()[:120]))
            records.append(m.groups())
    return records


def normalize(log, elf):
    records = parse(log)
    if not records:
        return []
    resolver = Resolver(symbols(elf))
    lines = []
    for tag, ident, base, size, at, data in records:
        words = [int(x, 16) for x in data.split()]
        norm = normalize_words(words, int(base, 16), int(size, 16), resolver)
        lines.append("%s %s at=%s %s" % (tag, ident, at, " ".join(norm)))
    return lines


class _Fixed:
    """Resolves every value to one name, for selftest."""

    def name(self, value, base, size):
        return "sym+0"


def selftest():
    import tempfile
    lui = 0x40000f37                   # lui t5, 0x40000
    srli, srai = 0x001f5f93, 0x401f5f93  # srli/srai t6, t5, 1
    addi = 0x000f0f93                  # addi t6, t5, 0
    jal = 0x0000006f
    checks = [
        normalize_words([lui, srli], 0, 0, _Fixed()) !=
        normalize_words([lui, srai], 0, 0, _Fixed()),
        normalize_words([lui, addi], 0, 0, _Fixed())[1] ==
        "000f0f93@sym+0",
        # Control flow ends the pending upper value.
        normalize_words([lui, jal, addi], 0, 0, _Fixed())[2] == "000f0f93",
    ]
    good = "GOLDEN t 0 base=10 size=10 at=0 00000013\n"
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, "log")
        for text, ok in ((good, True), (good[:-1] + " 1234\n", False)):
            with open(log, "w") as f:
                f.write(text)
            try:
                parse(log)
                checks.append(ok)
            except SystemExit:
                checks.append(not ok)
        with open(log, "w") as f:
            f.write("no dump\n")
        checks.append(parse(log) == [])
    failed = [i for i, ok in enumerate(checks) if not ok]
    print("golden selftest: %s" % ("FAIL %s" % failed if failed else "PASS"))
    return 1 if failed else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) == 4 and argv[1] == "normalize":
        print("\n".join(normalize(argv[2], argv[3])))
        return 0
    if len(argv) == 6 and argv[1] == "compare":
        base = normalize(argv[2], argv[3])
        new = normalize(argv[4], argv[5])
        for lines, log in ((base, argv[2]), (new, argv[4])):
            if not lines:
                print("golden: no GOLDEN lines in %s" % log)
                return 1
        diffs = [i for i in range(max(len(base), len(new)))
                 if i >= len(base) or i >= len(new) or base[i] != new[i]]
        for i in diffs[:5]:
            print("- %s" % (base[i] if i < len(base) else "<missing>"))
            print("+ %s" % (new[i] if i < len(new) else "<missing>"))
        print("golden: %d lines, %d differ" % (len(base), len(diffs)))
        return 1 if diffs else 0
    print(__doc__.strip())
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

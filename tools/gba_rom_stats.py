#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

"""Estimate ARM/Thumb instruction and branch mix in a GBA ROM.

Dependency-free static control-flow traversal. This is a partial, heuristic
code sample, not a runtime profile or a complete disassembly.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import struct

BASE = 0x08000000
MAX_SIZE = 32 * 1024 * 1024
MAX_BUNDLED_BIOS_SWI = 0x2A


def signed(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def ror(value, count):
    count %= 32
    return ((value >> count) | (value << (32 - count))) & 0xFFFFFFFF if count else value


class Analyzer:
    def __init__(self, data):
        self.data = data
        self.nodes = {}  # (address, mode): (size, category, is_compare)
        self.difficult = {}  # (address, mode): instruction details
        self.byte_modes = bytearray(len(data))  # 1=ARM, 2=Thumb
        self.pending = []
        self.unresolved = set()
        self.conflicts = set()
        self.pointer_seeds = set()
        self.address_taken = set()
        self.entry_count = 0

    def inside(self, address, size, mode):
        offset = address - BASE
        return (offset >= 0 and offset + size <= len(self.data)
                and address % (4 if mode == 'arm' else 2) == 0)

    def word(self, address):
        if self.inside(address, 4, 'arm'):
            return struct.unpack_from('<I', self.data, address - BASE)[0]
        return None

    def half(self, address):
        if self.inside(address, 2, 'thumb'):
            return struct.unpack_from('<H', self.data, address - BASE)[0]
        return None

    def seed(self, address, mode):
        if self.inside(address, 4 if mode == 'arm' else 2, mode):
            self.pending.append((address, mode))

    def walk_pending(self):
        while self.pending:
            address, mode = self.pending.pop()
            self.walk(address, mode)

    def mark(self, address, mode, size, category, compare, detail):
        start = address - BASE
        label = 1 if mode == 'arm' else 2
        if any(self.byte_modes[start:start + size]):
            self.conflicts.add(address)
            return False
        self.byte_modes[start:start + size] = bytes([label]) * size
        self.nodes[(address, mode)] = (size, category, compare)
        if detail is not None:
            self.difficult[(address, mode)] = detail
        return True

    def walk(self, address, mode):
        # Constants are local to one straight-line path. Merging paths would
        # require dataflow analysis; clearing them at jumps avoids false targets.
        regs = [None] * 16
        while self.inside(address, 4 if mode == 'arm' else 2, mode):
            if (address, mode) in self.nodes:
                return
            if mode == 'thumb':
                result = self.thumb(address, regs)
            else:
                result = self.arm(address, regs)
            if result is None:
                return
            size, category, compare, target, next_mode, fallthrough, detail = result
            if not self.mark(address, mode, size, category, compare, detail):
                return
            if target is not None:
                if self.inside(target, 4 if next_mode == 'arm' else 2, next_mode):
                    self.seed(target, next_mode)
                elif category in ('indirect', 'call_indirect'):
                    self.unresolved.add((address, mode))
            elif category in ('indirect', 'call_indirect'):
                self.unresolved.add((address, mode))
            if not fallthrough:
                return
            if category in ('call', 'call_indirect', 'software_interrupt'):
                regs = [None] * 16
            address += size

    def thumb(self, address, regs):
        hw = self.half(address)
        if hw is None:
            return None
        size, category, compare = 2, 'other', False
        target, next_mode, fallthrough = None, 'thumb', True
        detail = None

        if hw & 0xF800 == 0xF000:
            lo = self.half(address + 2)
            if lo is None or lo & 0xF800 != 0xF800:
                return None  # unpaired BL prefix; likely data or unsupported flow
            size, category = 4, 'call'
            target = address + 4 + signed(((hw & 0x7FF) << 12) | ((lo & 0x7FF) << 1), 23)
            regs[14] = (address + 4) | 1
        elif hw & 0xF800 == 0xF800:
            return None
        elif hw & 0xF000 == 0xD000:
            cond = (hw >> 8) & 15
            if cond == 15:
                category = 'software_interrupt'
                detail = {'kind': 'swi_thumb', 'service': hw & 0xFF,
                          'outside_bios_table': (hw & 0xFF) > MAX_BUNDLED_BIOS_SWI,
                          'description': f'SWI 0x{hw & 0xFF:02x}'}
            elif cond == 14:
                return None
            else:
                category = 'conditional_branch'
                target = address + 4 + signed((hw & 0xFF) << 1, 9)
        elif hw & 0xF800 == 0xE000:
            category, fallthrough = 'branch', False
            target = address + 4 + signed((hw & 0x7FF) << 1, 12)
        elif hw & 0xFF87 == 0x4700:
            source = (hw >> 3) & 15
            category = 'return' if source == 14 else 'indirect'
            fallthrough = False
            value = regs[source]
            if value is not None:
                next_mode = 'thumb' if value & 1 else 'arm'
                target = value & ~1 if value & 1 else value & ~3
        elif hw & 0xFE00 == 0xBC00:  # POP; bit 8 selects PC
            if hw & 0x100:
                category, fallthrough = 'return', False
            for reg in range(8):
                if hw & (1 << reg):
                    regs[reg] = None
            regs[13] = None
        elif hw & 0xFC00 == 0x4400:  # high register operations
            op = (hw >> 8) & 3
            rd = (hw & 7) | ((hw >> 4) & 8)
            rs = (hw >> 3) & 15
            if op == 1:
                compare = True
            elif op in (0, 2):
                value = regs[rs]
                if op == 0 and value is not None and regs[rd] is not None:
                    value = (regs[rd] + value) & 0xFFFFFFFF
                elif op == 0:
                    value = None
                regs[rd] = value
                if rd == 15:
                    category, fallthrough = 'indirect', False
                    if value is not None:
                        target = value & ~1
        elif hw & 0xF800 == 0x4800:  # LDR from PC-relative literal
            rd = (hw >> 8) & 7
            literal = ((address + 4) & ~3) + 4 * (hw & 0xFF)
            regs[rd] = self.word(literal)
        elif hw & 0xF800 == 0x2000:  # MOV immediate
            regs[(hw >> 8) & 7] = hw & 0xFF
        elif hw & 0xF800 == 0x2800:
            compare = True
        elif hw & 0xF800 == 0x3000:  # ADD immediate
            rd = (hw >> 8) & 7
            regs[rd] = None if regs[rd] is None else (regs[rd] + (hw & 0xFF)) & 0xFFFFFFFF
        elif hw & 0xF800 == 0x3800:  # SUB immediate
            rd = (hw >> 8) & 7
            regs[rd] = None if regs[rd] is None else (regs[rd] - (hw & 0xFF)) & 0xFFFFFFFF
        elif hw & 0xF800 == 0xA000:  # ADR
            regs[(hw >> 8) & 7] = ((address + 4) & ~3) + 4 * (hw & 0xFF)
        elif hw & 0xFC00 == 0x4000:  # ALU operations
            op = (hw >> 6) & 15
            compare = op in (8, 10, 11)
            if not compare:
                regs[hw & 7] = None
        elif hw & 0xE000 == 0x0000 or hw & 0xF000 in (0x5000, 0x6000, 0x7000, 0x8000):
            regs[hw & 7] = None
        elif hw & 0xF000 in (0x9000, 0xA000):
            regs[(hw >> 8) & 7] = None
        elif hw & 0xF800 == 0xC800:  # LDM
            for reg in range(8):
                if hw & (1 << reg):
                    regs[reg] = None
            regs[(hw >> 8) & 7] = None
        elif hw & 0xF800 == 0xC000:  # STM updates the base register
            regs[(hw >> 8) & 7] = None
        elif hw & 0xFF00 == 0xB000:  # ADD/SUB SP, #immediate
            regs[13] = None
        elif hw & 0xFE00 == 0xB400:  # PUSH updates SP
            regs[13] = None
        return size, category, compare, target, next_mode, fallthrough, detail

    def arm(self, address, regs):
        word = self.word(address)
        if word is None or word >> 28 == 15:
            return None
        cond = word >> 28
        category, compare, target, next_mode, fallthrough = 'other', False, None, 'arm', True
        detail = None
        if word & 0x0E000000 == 0x0A000000:
            link = bool(word & 0x01000000)
            category = 'call' if link else 'conditional_branch' if cond != 14 else 'branch'
            target = address + 8 + signed((word & 0xFFFFFF) << 2, 26)
            fallthrough = link or cond != 14
        elif word & 0x0FFFFFF0 == 0x012FFF10:  # BX register
            rs = word & 15
            category = 'return' if rs == 14 else 'indirect'
            fallthrough = cond != 14
            value = regs[rs]
            if value is not None:
                next_mode = 'thumb' if value & 1 else 'arm'
                target = value & ~1 if value & 1 else value & ~3
        elif word & 0x0F000000 == 0x0F000000:
            category = 'software_interrupt'
            service = (word >> 16) & 0xFF
            detail = {'kind': 'swi_arm', 'service': service,
                      'outside_bios_table': service > MAX_BUNDLED_BIOS_SWI,
                      'description': f'SWI 0x{word & 0xFFFFFF:06x}'}
        elif word & 0x0FBF0FFF == 0x010F0000:  # MRS Rd, CPSR/SPSR
            psr = 'spsr' if word & 0x00400000 else 'cpsr'
            rd = (word >> 12) & 15
            regs[rd] = None
            detail = {'kind': f'mrs_{psr}',
                      'description': f'MRS r{rd}, {psr.upper()}'}
        elif (word & 0x0FB0FFF0 == 0x0120F000 or
              word & 0x0FB0F000 == 0x0320F000):  # MSR register/immediate
            psr = 'spsr' if word & 0x00400000 else 'cpsr'
            immediate = bool(word & 0x02000000)
            fields = (word >> 16) & 15
            field_names = ''.join(name for bit, name in enumerate('cxsf')
                                  if fields & (1 << bit))
            source = (f'#0x{ror(word & 0xFF, ((word >> 8) & 15) * 2):x}'
                      if immediate else f'r{word & 15}')
            detail = {'kind': f'msr_{psr}_{"immediate" if immediate else "register"}',
                      'fields': fields,
                      'description': f'MSR {psr.upper()}_{field_names}, {source}'}
            if psr == 'cpsr' and fields & 1:
                # A mode switch can change the banked registers, not the PC.
                regs[8:15] = [None] * 7
        elif word & 0x0F7F0000 == 0x051F0000:  # LDR Rd, [PC, #+/-imm]
            rd = (word >> 12) & 15
            delta = word & 0xFFF
            literal = address + 8 + (delta if word & 0x00800000 else -delta)
            value = self.word(literal)
            regs[rd] = value if cond == 14 else None
            if rd == 15:
                category, fallthrough = 'indirect', cond != 14
                target = regs[rd]
        elif word & 0x0FE00000 == 0x03A00000:  # MOV immediate
            rd = (word >> 12) & 15
            regs[rd] = ror(word & 0xFF, ((word >> 8) & 15) * 2) if cond == 14 else None
            if rd == 15:
                category, fallthrough, target = 'indirect', cond != 14, regs[rd]
        elif word & 0x0FE00000 in (0x02800000, 0x02400000) and (word >> 16) & 15 == 15:
            # ADD/SUB Rd, PC, #immediate. This often stores a handler address.
            rd = (word >> 12) & 15
            delta = ror(word & 0xFF, ((word >> 8) & 15) * 2)
            value = (address + 8 + (delta if word & 0x00400000 == 0 else -delta)) & 0xFFFFFFFF
            regs[rd] = value if cond == 14 else None
            if cond == 14 and self.inside(value, 4, 'arm'):
                self.address_taken.add(value)
            if rd == 15:
                category, fallthrough, target = 'indirect', cond != 14, regs[rd]
        elif word & 0x0FC000F0 == 0x00000090:  # MUL/MLA
            regs[(word >> 16) & 15] = None
        elif word & 0x0F8000F0 == 0x00800090:  # long multiply
            regs[(word >> 16) & 15] = None
            regs[(word >> 12) & 15] = None
        elif word & 0x0FB00FF0 == 0x01000090:  # SWP/SWPB
            byte = bool(word & 0x00400000)
            rd, rn, rm = (word >> 12) & 15, (word >> 16) & 15, word & 15
            regs[rd] = None
            detail = {'kind': 'swap_byte' if byte else 'swap_word',
                      'description': f'SWP{"B" if byte else ""} r{rd}, r{rm}, [r{rn}]'}
        elif word & 0x0C000000 == 0:  # data processing and shared encodings
            op = (word >> 21) & 15
            rd = (word >> 12) & 15
            # Register forms with bits 7 and 4 set include SWP and halfword access.
            data_proc = (bool(word & 0x02000000)
                         or (word & 0x90) != 0x90)
            compare = (op in (8, 9, 10, 11)
                       and bool(word & 0x00100000) and data_proc)
            if not compare:
                regs[rd] = None
                if rd == 15:
                    category, fallthrough = 'indirect', cond != 14
                    if data_proc and word & 0x00100000 and op not in (8, 9, 10, 11):
                        category = 'return'
                        detail = {'kind': 'exception_return_data_processing',
                                  'description': 'data-processing S, Rd=PC (SPSR restore)'}
            if not data_proc and word & 0x60 and (word & 0x01000000 == 0 or word & 0x00200000):
                regs[(word >> 16) & 15] = None  # halfword transfer writeback
        elif word & 0x0C000000 == 0x04000000:  # load/store
            rd = (word >> 12) & 15
            if word & 0x00100000:
                regs[rd] = None
                if rd == 15:
                    category, fallthrough = 'indirect', cond != 14
            if word & 0x01000000 == 0 or word & 0x00200000:
                regs[(word >> 16) & 15] = None
        elif word & 0x0E000000 == 0x08000000:  # block load/store
            load = bool(word & 0x00100000)
            if word & 0x00400000:
                if load and word & (1 << 15):
                    detail = {'kind': 'exception_return_ldm',
                              'description': 'LDM^ with PC (SPSR restore)'}
                else:
                    detail = {'kind': 'user_bank_ldm' if load else 'user_bank_stm',
                              'description': f'{"LDM" if load else "STM"}^ (user bank)'}
            if word & 0x00100000:
                for reg in range(16):
                    if word & (1 << reg):
                        regs[reg] = None
                if word & (1 << 15):
                    category, fallthrough = 'return', cond != 14
            if word & 0x00200000:
                regs[(word >> 16) & 15] = None
        return 4, category, compare, target, next_mode, fallthrough, detail

    def scan_pointer_seeds(self):
        # A ROM word containing an odd ROM address whose target begins with a
        # common Thumb function prologue is a useful, but uncertain, seed.
        for offset in range(0, len(self.data) - 3, 4):
            value = struct.unpack_from('<I', self.data, offset)[0]
            if value & 1 == 0:
                continue
            target = value & ~1
            hw = self.half(target)
            if hw is None or hw & 0xFF00 != 0xB500:
                continue
            if (target, 'thumb') not in self.nodes:
                self.pointer_seeds.add(target)
        for address in self.pointer_seeds:
            self.seed(address, 'thumb')
        self.walk_pending()

    def scan_address_taken(self):
        # ARM PC-relative address construction is common for interrupt handlers.
        while self.address_taken:
            targets = self.address_taken
            self.address_taken = set()
            for address in targets:
                self.seed(address, 'arm')
            self.walk_pending()

    def result(self, source, pointer_scan, difficult_sites=0):
        modes = Counter(mode for _, mode in self.nodes)
        kinds = Counter(info[1] for info in self.nodes.values())
        compare = sum(info[2] for info in self.nodes.values())
        compare_branch = sum(
            info[1] == 'conditional_branch' and
            self.nodes.get((address - (4 if mode == 'arm' else 2), mode), (0, '', False))[2]
            for (address, mode), info in self.nodes.items()
        )
        back_edges = sum(
            info[1] in ('conditional_branch', 'branch') and
            (self.arm_target(address) if mode == 'arm' else self.thumb_target(address)) < address
            for (address, mode), info in self.nodes.items()
            if info[1] in ('conditional_branch', 'branch')
        )
        total = len(self.nodes)
        difficult = Counter(detail['kind'] for detail in self.difficult.values())
        difficult_kinds = (
            'swi_arm', 'swi_thumb', 'mrs_cpsr', 'mrs_spsr',
            'msr_cpsr_register', 'msr_cpsr_immediate',
            'msr_spsr_register', 'msr_spsr_immediate',
            'exception_return_data_processing', 'exception_return_ldm',
            'user_bank_ldm', 'user_bank_stm', 'swap_word', 'swap_byte',
        )
        difficult_counts = {kind: difficult[kind] for kind in difficult_kinds}
        difficult_counts['total_sites'] = len(self.difficult)
        difficult_counts['swi_outside_bios_table'] = sum(
            detail.get('outside_bios_table', False)
            for detail in self.difficult.values())
        difficult_counts['msr_cpsr_control_field'] = sum(
            detail['kind'].startswith('msr_cpsr_') and detail['fields'] & 1 != 0
            for detail in self.difficult.values())
        difficult_counts['msr_spsr_control_field'] = sum(
            detail['kind'].startswith('msr_spsr_') and detail['fields'] & 1 != 0
            for detail in self.difficult.values())
        swi_services = {}
        for mode in ('arm', 'thumb'):
            services = Counter(detail['service'] for detail in self.difficult.values()
                               if detail['kind'] == f'swi_{mode}')
            swi_services[mode] = [
                {'service': f'0x{service:02x}', 'sites': count}
                for service, count in sorted(services.items(),
                                             key=lambda item: (-item[1], item[0]))[:8]
            ]
        return {
            'file': str(source),
            'scope': 'identified_static_code_only',
            'rom_bytes': len(self.data),
            'instruction_counts': {'arm': modes['arm'], 'thumb': modes['thumb'], 'total': total},
            'instruction_percent': {
                'arm': round(100 * modes['arm'] / total, 3) if total else 0,
                'thumb': round(100 * modes['thumb'] / total, 3) if total else 0,
            },
            'branch_counts': {key: kinds[key] for key in (
                'conditional_branch', 'branch', 'call', 'indirect', 'return', 'software_interrupt')},
            'compare_test_count': compare,
            'compare_then_conditional_branch_count': compare_branch,
            'backward_direct_branch_count': back_edges,
            'branch_like_percent': round(100 * sum(kinds[key] for key in (
                'conditional_branch', 'branch', 'call', 'indirect', 'return', 'software_interrupt')) / total, 3) if total else 0,
            'difficult_instruction_counts': difficult_counts,
            'top_swi_services': swi_services,
            'difficult_sites': {
                kind: [
                    {'address': f'0x{address:08x}',
                     'opcode': f'0x{self.word(address) if mode == "arm" else self.half(address):0{8 if mode == "arm" else 4}x}',
                     'instruction': detail['description'],
                     **({'outside_bios_table': True}
                        if detail.get('outside_bios_table') else {})}
                    for (address, mode), detail in sorted(self.difficult.items())
                    if detail['kind'] == kind
                ][:difficult_sites]
                for kind in difficult_kinds if difficult[kind]
            } if difficult_sites else {},
            'identified_code_bytes': self.byte_modes.count(1) + self.byte_modes.count(2),
            'identified_rom_percent': round(100 * (self.byte_modes.count(1) + self.byte_modes.count(2)) / len(self.data), 3),
            'entry_reachable_instructions': self.entry_count,
            'pointer_seed_candidates': len(self.pointer_seeds) if pointer_scan else 0,
            'unresolved_indirect_sites': len(self.unresolved),
            'overlapping_code_sites': len(self.conflicts),
        }

    def arm_target(self, address):
        word = self.word(address)
        return address + 8 + signed((word & 0xFFFFFF) << 2, 26)

    def thumb_target(self, address):
        hw = self.half(address)
        bits = 8 if hw & 0xF000 == 0xD000 else 11
        return address + 4 + signed((hw & ((1 << bits) - 1)) << 1, bits + 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path, help='GBA ROM file')
    parser.add_argument('--no-pointer-scan', action='store_true',
                        help='skip heuristic Thumb function-pointer search')
    parser.add_argument('--seed', action='append', default=[], metavar='ADDRESS[:arm|thumb]',
                        help='add a known ROM code entry address (repeatable)')
    parser.add_argument('--json', action='store_true', help='machine-readable output')
    parser.add_argument('--difficult-sites', type=int, default=0, metavar='N',
                        help='show up to N static sites per difficult instruction kind')
    args = parser.parse_args()
    if args.difficult_sites < 0:
        parser.error('--difficult-sites must be zero or greater')
    try:
        data = args.rom.read_bytes()
    except OSError as exc:
        parser.error(str(exc))
    if not 0xC0 <= len(data) <= MAX_SIZE:
        parser.error('GBA ROM size must be between 192 bytes and 32 MiB')
    analyzer = Analyzer(data)
    analyzer.seed(BASE, 'arm')
    analyzer.walk_pending()
    analyzer.entry_count = len(analyzer.nodes)
    analyzer.scan_address_taken()
    for raw in args.seed:
        parts = raw.split(':')
        if len(parts) > 2 or (len(parts) == 2 and parts[1] not in ('arm', 'thumb')):
            parser.error(f'invalid seed: {raw}')
        try:
            value = int(parts[0], 0)
        except ValueError:
            parser.error(f'invalid seed address: {raw}')
        mode = parts[1] if len(parts) == 2 else ('thumb' if value & 1 else 'arm')
        analyzer.seed(value & ~1 if mode == 'thumb' else value, mode)
    analyzer.walk_pending()
    analyzer.scan_address_taken()
    if not args.no_pointer_scan:
        analyzer.scan_pointer_seeds()
        analyzer.scan_address_taken()
    result = analyzer.result(args.rom, not args.no_pointer_scan, args.difficult_sites)
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return
    print(f"ROM: {args.rom} ({result['rom_bytes']:,} bytes)")
    print(f"Identified code sample: {result['instruction_counts']['total']:,} instructions, "
          f"{result['identified_code_bytes']:,} bytes "
          f"({result['identified_rom_percent']}% of ROM)")
    print(f"  ARM:   {result['instruction_counts']['arm']:,} "
          f"({result['instruction_percent']['arm']}%)")
    print(f"  Thumb: {result['instruction_counts']['thumb']:,} "
          f"({result['instruction_percent']['thumb']}%)")
    print('Control flow (static instruction sites):')
    for key, value in result['branch_counts'].items():
        print(f'  {key:22s} {value:,}')
    print(f"  compare/test           {result['compare_test_count']:,}")
    print(f"  compare then cond.     {result['compare_then_conditional_branch_count']:,}")
    print(f"  backward direct branch {result['backward_direct_branch_count']:,}")
    print(f"  branch-like density    {result['branch_like_percent']}%")
    print('Difficult instructions (static sites; groups do not overlap):')
    for key, value in result['difficult_instruction_counts'].items():
        if key not in ('total_sites', 'swi_outside_bios_table',
                       'msr_cpsr_control_field', 'msr_spsr_control_field'):
            print(f'  {key:32s} {value:,}')
    print(f"  {'total_sites':32s} {result['difficult_instruction_counts']['total_sites']:,}")
    print(f"  {'swi_outside_bios_table (subset)':32s} "
          f"{result['difficult_instruction_counts']['swi_outside_bios_table']:,}")
    print(f"  {'msr_cpsr_control_field (subset)':32s} "
          f"{result['difficult_instruction_counts']['msr_cpsr_control_field']:,}")
    print(f"  {'msr_spsr_control_field (subset)':32s} "
          f"{result['difficult_instruction_counts']['msr_spsr_control_field']:,}")
    for mode, services in result['top_swi_services'].items():
        if services:
            print(f"  top {mode.upper()} SWI services: " + ', '.join(
                f"{item['service']} ({item['sites']})" for item in services))
    if args.difficult_sites:
        print(f'Difficult sites (up to {args.difficult_sites} per kind):')
        for kind, sites in result['difficult_sites'].items():
            print(f'  {kind}:')
            for site in sites:
                note = ' [outside bundled BIOS table]' if site.get('outside_bios_table') else ''
                print(f"    {site['address']} {site['opcode']}  {site['instruction']}{note}")
    print(f"Entry-reachable instructions: {result['entry_reachable_instructions']:,}")
    if not args.no_pointer_scan:
        print(f"Pointer-derived Thumb seeds: {result['pointer_seed_candidates']:,}")
    print(f"Unresolved indirect sites: {result['unresolved_indirect_sites']:,}")
    print(f"Overlapping code sites: {result['overlapping_code_sites']:,}")
    print('Ratios apply only to identified instructions. ROM bytes also include data/assets.')
    print('Static estimate only: indirect targets, RAM code, and execution frequencies may be missed.')


if __name__ == '__main__':
    main()

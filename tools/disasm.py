#!/usr/bin/env python3
"""Tracing 6502 disassembler for Boulder Dash (NES) -> ca65 source.

The PRG is a fixed 32 KB image at $8000-$FFFF (MMC1, no PRG banking), so a
CPU address maps to PRG offset addr - $8000.

Code is found by recursive descent from the vectors plus the entry points in
tools/hints.txt (and, optionally, a Mesen2 .cdl file). Everything not reached
is emitted as .byte data, so the output always reassembles byte-for-byte.

Usage: python tools/disasm.py BASEROM [--cdl FILE]
Writes src/prg.s, src/ram.inc and assets/chr.bin.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x8000

# mode -> operand size
MODES = {
    'imp': 0, 'acc': 0, 'imm': 1, 'zp': 1, 'zpx': 1, 'zpy': 1, 'indx': 1,
    'indy': 1, 'rel': 1, 'abs': 2, 'absx': 2, 'absy': 2, 'ind': 2,
}

OPS = {}
def _def(mn, *pairs):
    for mode, op in pairs:
        OPS[op] = (mn, mode)

_def('adc', ('imm', 0x69), ('zp', 0x65), ('zpx', 0x75), ('abs', 0x6D), ('absx', 0x7D), ('absy', 0x79), ('indx', 0x61), ('indy', 0x71))
_def('and', ('imm', 0x29), ('zp', 0x25), ('zpx', 0x35), ('abs', 0x2D), ('absx', 0x3D), ('absy', 0x39), ('indx', 0x21), ('indy', 0x31))
_def('asl', ('acc', 0x0A), ('zp', 0x06), ('zpx', 0x16), ('abs', 0x0E), ('absx', 0x1E))
_def('bcc', ('rel', 0x90)); _def('bcs', ('rel', 0xB0)); _def('beq', ('rel', 0xF0)); _def('bmi', ('rel', 0x30))
_def('bne', ('rel', 0xD0)); _def('bpl', ('rel', 0x10)); _def('bvc', ('rel', 0x50)); _def('bvs', ('rel', 0x70))
_def('bit', ('zp', 0x24), ('abs', 0x2C))
_def('brk', ('imp', 0x00))
_def('clc', ('imp', 0x18)); _def('cld', ('imp', 0xD8)); _def('cli', ('imp', 0x58)); _def('clv', ('imp', 0xB8))
_def('cmp', ('imm', 0xC9), ('zp', 0xC5), ('zpx', 0xD5), ('abs', 0xCD), ('absx', 0xDD), ('absy', 0xD9), ('indx', 0xC1), ('indy', 0xD1))
_def('cpx', ('imm', 0xE0), ('zp', 0xE4), ('abs', 0xEC))
_def('cpy', ('imm', 0xC0), ('zp', 0xC4), ('abs', 0xCC))
_def('dec', ('zp', 0xC6), ('zpx', 0xD6), ('abs', 0xCE), ('absx', 0xDE))
_def('dex', ('imp', 0xCA)); _def('dey', ('imp', 0x88))
_def('eor', ('imm', 0x49), ('zp', 0x45), ('zpx', 0x55), ('abs', 0x4D), ('absx', 0x5D), ('absy', 0x59), ('indx', 0x41), ('indy', 0x51))
_def('inc', ('zp', 0xE6), ('zpx', 0xF6), ('abs', 0xEE), ('absx', 0xFE))
_def('inx', ('imp', 0xE8)); _def('iny', ('imp', 0xC8))
_def('jmp', ('abs', 0x4C), ('ind', 0x6C))
_def('jsr', ('abs', 0x20))
_def('lda', ('imm', 0xA9), ('zp', 0xA5), ('zpx', 0xB5), ('abs', 0xAD), ('absx', 0xBD), ('absy', 0xB9), ('indx', 0xA1), ('indy', 0xB1))
_def('ldx', ('imm', 0xA2), ('zp', 0xA6), ('zpy', 0xB6), ('abs', 0xAE), ('absy', 0xBE))
_def('ldy', ('imm', 0xA0), ('zp', 0xA4), ('zpx', 0xB4), ('abs', 0xAC), ('absx', 0xBC))
_def('lsr', ('acc', 0x4A), ('zp', 0x46), ('zpx', 0x56), ('abs', 0x4E), ('absx', 0x5E))
_def('nop', ('imp', 0xEA))
_def('ora', ('imm', 0x09), ('zp', 0x05), ('zpx', 0x15), ('abs', 0x0D), ('absx', 0x1D), ('absy', 0x19), ('indx', 0x01), ('indy', 0x11))
_def('pha', ('imp', 0x48)); _def('php', ('imp', 0x08)); _def('pla', ('imp', 0x68)); _def('plp', ('imp', 0x28))
_def('rol', ('acc', 0x2A), ('zp', 0x26), ('zpx', 0x36), ('abs', 0x2E), ('absx', 0x3E))
_def('ror', ('acc', 0x6A), ('zp', 0x66), ('zpx', 0x76), ('abs', 0x6E), ('absx', 0x7E))
_def('rti', ('imp', 0x40)); _def('rts', ('imp', 0x60))
_def('sbc', ('imm', 0xE9), ('zp', 0xE5), ('zpx', 0xF5), ('abs', 0xED), ('absx', 0xFD), ('absy', 0xF9), ('indx', 0xE1), ('indy', 0xF1))
_def('sec', ('imp', 0x38)); _def('sed', ('imp', 0xF8)); _def('sei', ('imp', 0x78))
_def('sta', ('zp', 0x85), ('zpx', 0x95), ('abs', 0x8D), ('absx', 0x9D), ('absy', 0x99), ('indx', 0x81), ('indy', 0x91))
_def('stx', ('zp', 0x86), ('zpy', 0x96), ('abs', 0x8E))
_def('sty', ('zp', 0x84), ('zpx', 0x94), ('abs', 0x8C))
_def('tax', ('imp', 0xAA)); _def('tay', ('imp', 0xA8)); _def('tsx', ('imp', 0xBA)); _def('txa', ('imp', 0x8A))
_def('txs', ('imp', 0x9A)); _def('tya', ('imp', 0x98))

ENDS_FLOW = {'jmp', 'rts', 'rti', 'brk'}

HW_REGS = {
    0x2000: 'PPUCTRL', 0x2001: 'PPUMASK', 0x2002: 'PPUSTATUS', 0x2003: 'OAMADDR',
    0x2004: 'OAMDATA', 0x2005: 'PPUSCROLL', 0x2006: 'PPUADDR', 0x2007: 'PPUDATA',
    0x4000: 'SQ1_VOL', 0x4001: 'SQ1_SWEEP', 0x4002: 'SQ1_LO', 0x4003: 'SQ1_HI',
    0x4004: 'SQ2_VOL', 0x4005: 'SQ2_SWEEP', 0x4006: 'SQ2_LO', 0x4007: 'SQ2_HI',
    0x4008: 'TRI_LINEAR', 0x4009: 'APU_UNUSED1', 0x400A: 'TRI_LO', 0x400B: 'TRI_HI',
    0x400C: 'NOISE_VOL', 0x400D: 'APU_UNUSED2', 0x400E: 'NOISE_LO', 0x400F: 'NOISE_HI',
    0x4010: 'DMC_FREQ', 0x4011: 'DMC_RAW', 0x4012: 'DMC_START', 0x4013: 'DMC_LEN',
    0x4014: 'OAMDMA', 0x4015: 'SND_CHN', 0x4016: 'JOY1', 0x4017: 'JOY2',
}


class Disasm:
    def __init__(self, prg):
        self.prg = prg
        self.code = {}          # addr -> (mn, mode, operand)
        self.labels = {}        # addr -> kind ('sub', 'loc', 'dat') or explicit name
        self.names = {}         # addr -> user name (ROM or RAM)
        self.comments = {}      # addr -> comment
        self.ram_refs = {}      # addr -> set of 'zp'/'abs'
        self.data_words = {}    # addr -> True: emit .word with label (pointer tables)
        self.data_rts = {}      # addr -> True: emit .word label-1 (RTS dispatch tables)
        self.data_lo = {}       # addr -> target (lo-byte table entry)
        self.data_hi = {}       # addr -> target (hi-byte table entry)
        self.forced_data = set()
        self.consts = {}          # ROM addresses used as mapper registers: addr -> name
        self.dispatchers = set()  # subs followed by an inline .word table
        self.inline_counts = {}   # table addr -> explicit entry count

    def byte(self, a):
        return self.prg[a - BASE]

    def word(self, a):
        return self.byte(a) | self.byte(a + 1) << 8

    def in_rom(self, a):
        return BASE <= a <= 0xFFFF

    def add_label(self, a, kind):
        prio = {'sub': 3, 'loc': 2, 'dat': 1}
        old = self.labels.get(a)
        if old is None or prio[kind] > prio[old]:
            self.labels[a] = kind

    def trace(self, entries):
        todo = list(entries)
        while todo:
            pc = todo.pop()
            while self.in_rom(pc) and pc not in self.code and pc not in self.forced_data:
                op = self.byte(pc)
                if op not in OPS or op == 0x00:
                    # BRK is treated as data: in practice it means we fell
                    # through an always-taken branch into a table
                    if op != 0x00:
                        self.comments.setdefault(pc, 'trace hit invalid opcode here')
                    break
                mn, mode = OPS[op]
                size = 1 + MODES[mode]
                if pc + size - 1 > 0xFFFF:
                    break
                if mode == 'rel':
                    off = self.byte(pc + 1)
                    operand = (pc + 2 + (off - 256 if off >= 128 else off)) & 0xFFFF
                elif size == 3:
                    operand = self.word(pc + 1)
                elif size == 2:
                    operand = self.byte(pc + 1)
                else:
                    operand = None
                self.code[pc] = (mn, mode, operand)
                nxt = pc + size
                if mode == 'rel':
                    self.add_label(operand, 'loc')
                    todo.append(operand)
                elif mn == 'jsr':
                    self.add_label(operand, 'sub')
                    todo.append(operand)
                    if operand in self.dispatchers:
                        todo.extend(self.inline_table(nxt))
                        break
                elif mn == 'jmp' and mode == 'abs':
                    self.add_label(operand, 'loc')
                    todo.append(operand)
                elif mode in ('abs', 'absx', 'absy', 'ind') and self.in_rom(operand) \
                        and operand not in self.consts:
                    self.add_label(operand, 'dat')
                if mode in ('zp', 'zpx', 'zpy', 'indx', 'indy'):
                    self.ram_refs.setdefault(operand, set()).add('zp')
                elif mode in ('abs', 'absx', 'absy', 'ind') and operand < 0x2000:
                    self.ram_refs.setdefault(operand, set()).add('abs')
                if mn in ENDS_FLOW:
                    break
                pc = nxt

    def inline_table(self, a):
        """Words following a JSR to a dispatcher. Without an explicit count,
        read until a word leaves ROM or the table would run into a target."""
        self.add_label(a, 'dat')
        n = self.inline_counts.get(a)
        targets = []
        p = a
        while n is None or len(targets) < n:
            t = self.word(p)
            if n is None and (not self.in_rom(t) or p in self.code or
                              (targets and p >= min(x for x in targets + [0x10000] if x > a))):
                break
            targets.append(t)
            self.data_words[p] = True
            self.forced_data.update((p, p + 1))
            self.add_label(t, 'loc')
            p += 2
        return targets

    def instr_size(self, a):
        return 1 + MODES[self.code[a][1]]

    def finalize(self):
        """Decide which instruction starts are emitted as instructions.

        An instruction is dropped to raw bytes if a label or another
        instruction start falls inside it (e.g. the BIT-skip trick).
        """
        starts = set(self.code)
        self.emit_instr = set()
        for a in sorted(self.code):
            n = self.instr_size(a)
            inner = range(a + 1, a + n)
            if any(i in starts or i in self.labels for i in inner):
                self.comments.setdefault(a, 'overlapping instruction emitted as bytes')
                continue
            self.emit_instr.add(a)
        # owner map so labels inside instructions can be expressed as base+N
        self.owner = {}
        for a in self.emit_instr:
            for i in range(a, a + self.instr_size(a)):
                self.owner[i] = a
        for a in list(self.labels):
            o = self.owner.get(a)
            if o is not None and o != a:
                del self.labels[a]
                self.add_label(o, 'loc')
                self.inner_refs = getattr(self, 'inner_refs', {})
                self.inner_refs[a] = o

    def label_name(self, a):
        if a in self.names:
            return self.names[a]
        kind = self.labels.get(a, 'loc')
        prefix = {'sub': 'sub_', 'loc': 'loc_', 'dat': 'dat_'}[kind]
        return '%s%04X' % (prefix, a)

    def ref(self, a):
        """Symbolic reference to an address used as an operand."""
        if a in self.consts:
            return self.consts[a]
        if self.in_rom(a):
            if a in self.labels:
                return self.label_name(a)
            o = getattr(self, 'inner_refs', {}).get(a)
            if o is not None:
                return '%s+%d' % (self.label_name(o), a - o)
            return '$%04X' % a
        if a in HW_REGS:
            return HW_REGS[a]
        if a in self.names:
            return self.names[a]
        if a < 0x800:
            return ram_name(a)
        return '$%04X' % a

    def fmt_operand(self, mn, mode, v):
        if mode in ('imp',):
            return ''
        if mode == 'acc':
            return 'a'
        if mode == 'imm':
            return '#$%02X' % v
        if mode == 'rel':
            return self.ref(v)
        r = self.ref(v)
        # force absolute addressing for operands that would fit in zero page
        force = 'a:' if mode in ('abs', 'absx', 'absy') and v < 0x100 and mn not in ('jmp', 'jsr') else ''
        return {
            'zp': '%s', 'zpx': '%s,x', 'zpy': '%s,y', 'abs': force + '%s',
            'absx': force + '%s,x', 'absy': force + '%s,y', 'ind': '(%s)',
            'indx': '(%s,x)', 'indy': '(%s),y',
        }[mode] % r


def ram_name(a):
    return ('zp_%02X' % a) if a < 0x100 else ('ram_%04X' % a)


def load_hints(path, d):
    entries = []
    if not os.path.exists(path):
        return entries
    for ln, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        cmd, args = f[0], f[1:]
        h = lambda s: int(s, 16)
        try:
            if cmd == 'code':          # code ADDR [NAME]
                a = h(args[0]); entries.append(a); d.add_label(a, 'loc')
                if len(args) > 1: d.names[a] = args[1]
            elif cmd == 'sub':         # sub ADDR [NAME]
                a = h(args[0]); entries.append(a); d.add_label(a, 'sub')
                if len(args) > 1: d.names[a] = args[1]
            elif cmd == 'name':        # name ADDR NAME[:SIZE]  (ROM or RAM; SIZE for port/ RAM struct)
                d.names[h(args[0])] = args[1].split(':')[0]
            elif cmd == 'comment':     # comment ADDR text...  (repeat for more lines)
                a, text = h(args[0]), ' '.join(args[1:])
                d.comments[a] = d.comments[a] + '\n; ' + text if a in d.comments else text
            elif cmd == 'mapreg':      # mapreg ADDR NAME  -- mapper register (constant, not a label)
                d.consts[h(args[0])] = args[1]
            elif cmd == 'dispatch':    # dispatch ADDR [NAME]  -- JSR ADDR is followed by a .word table
                a = h(args[0]); d.dispatchers.add(a); entries.append(a); d.add_label(a, 'sub')
                if len(args) > 1: d.names[a] = args[1]
            elif cmd == 'inline':      # inline ADDR COUNT  -- explicit size of one inline table
                d.inline_counts[h(args[0])] = int(args[1])
            elif cmd == 'data':        # data ADDR  -- never trace into
                d.forced_data.add(h(args[0]))
            elif cmd == 'words':       # words ADDR COUNT  -- pointer table to code
                a, n = h(args[0]), int(args[1])
                d.add_label(a, 'dat')
                for i in range(n):
                    d.data_words[a + 2 * i] = True
                    t = d.word(a + 2 * i); entries.append(t); d.add_label(t, 'loc')
            elif cmd == 'dwords':      # dwords ADDR COUNT  -- pointer table to data
                a, n = h(args[0]), int(args[1])
                d.add_label(a, 'dat')
                for i in range(n):
                    d.data_words[a + 2 * i] = True
                    d.add_label(d.word(a + 2 * i), 'dat')
            elif cmd == 'rtstable':    # rtstable ADDR COUNT  -- words of target-1
                a, n = h(args[0]), int(args[1])
                d.add_label(a, 'dat')
                for i in range(n):
                    d.data_rts[a + 2 * i] = True
                    t = d.word(a + 2 * i) + 1; entries.append(t); d.add_label(t, 'loc')
            elif cmd == 'splittable':  # splittable LO HI COUNT [rts]
                lo, hi, n = h(args[0]), h(args[1]), int(args[2])
                adj = 1 if len(args) > 3 and args[3] == 'rts' else 0
                d.add_label(lo, 'dat'); d.add_label(hi, 'dat')
                for i in range(n):
                    t = (d.byte(lo + i) | d.byte(hi + i) << 8) + adj
                    entries.append(t); d.add_label(t, 'loc')
                    d.data_lo[lo + i] = (t, adj); d.data_hi[hi + i] = (t, adj)
            else:
                raise ValueError('unknown command ' + cmd)
        except (IndexError, ValueError) as e:
            sys.exit('%s:%d: %s' % (path, ln, e))
    return entries


def load_cdl(path, size):
    """Code/Data Log from an emulator: one flag byte per PRG byte, then CHR.

    Mesen2 files start with 'CDLv2' + crc32 and mark jump targets (0x04) and
    sub entries (0x08); Mesen 1 / FCEUX files are headerless. Entry points are
    marked entries plus the start of every run of bytes logged as code
    (bit 0). Not yet checked against a real log from this game.
    """
    raw = open(path, 'rb').read()
    entry_mask = 0x00
    if raw[:5] == b'CDLv2':
        raw = raw[9:]
        entry_mask = 0x0C
    flags = raw[:size]
    out = []
    for i, f in enumerate(flags):
        if not f & 0x01:
            continue
        if f & entry_mask or i == 0 or not flags[i - 1] & 0x01:
            out.append(BASE + i)
    return out


def emit(d, out):
    w = out.append
    w('; Boulder Dash (USA) - PRG ROM ($8000-$FFFF)')
    w('; Generated by tools/disasm.py, then meant to be hand-annotated.')
    w('')
    w('.include "hw.inc"')
    w('.include "ram.inc"')
    w('')
    w('.segment "PRG"')
    w('')
    a = BASE
    end = 0x10000
    pending = []  # raw bytes buffer

    def flush():
        while pending:
            chunk = pending[:16]
            del pending[:16]
            w('    .byte ' + ','.join('$%02X' % b for b in chunk))

    while a < end:
        if a in d.labels or a in d.comments:
            flush()
        if a in d.comments:
            w('; %s' % d.comments[a])
        if a in d.labels:
            if d.labels[a] == 'sub' and a in d.emit_instr:
                w('')
            w('%s:' % d.label_name(a))
        if a == 0xFFFA:
            flush()
            w('    .word %s, %s, %s' % tuple(d.ref(d.word(0xFFFA + 2 * i)) for i in range(3)))
            break
        if a in d.emit_instr:
            flush()
            mn, mode, v = d.code[a]
            ops = d.fmt_operand(mn, mode, v)
            w('    %-4s%s' % (mn, ' ' + ops if ops else ''))
            a += d.instr_size(a)
            continue
        if a in d.data_words or a in d.data_rts:
            flush()
            t = d.word(a)
            expr = ('%s-1' % d.ref(t + 1)) if a in d.data_rts else d.ref(t)
            if a + 1 in d.labels:
                # something indexes the high bytes (lda tbl+1,x): split the word
                w('    .byte <(%s)' % expr)
                w('%s:' % d.label_name(a + 1))
                w('    .byte >(%s)' % expr)
            else:
                w('    .word %s' % expr)
            a += 2
            continue
        if a in d.data_lo or a in d.data_hi:
            flush()
            if a in d.data_lo:
                t, adj = d.data_lo[a]
                w('    .byte <(%s%s)' % (d.ref(t), '-1' if adj else ''))
            else:
                t, adj = d.data_hi[a]
                w('    .byte >(%s%s)' % (d.ref(t), '-1' if adj else ''))
            a += 1
            continue
        if a in d.code:
            # instruction dropped to bytes: keep a hint of what it was
            mn, mode, v = d.code[a]
            flush()
            w('    ; %s %s' % (mn, d.fmt_operand(mn, mode, v)))
        pending.append(d.byte(a))
        a += 1
        if a - BASE and (a & 0xF) == 0 and a not in d.labels:
            flush()
    flush()
    w('')


def peek(d, pc, count):
    for _ in range(count):
        op = d.byte(pc)
        if op not in OPS:
            print('%04X  %02X        .byte $%02X' % (pc, op, op))
            pc += 1
            continue
        mn, mode = OPS[op]
        size = 1 + MODES[mode]
        raw = d.prg[pc - BASE:pc - BASE + size]
        if mode == 'rel':
            off = raw[1]
            v = (pc + 2 + (off - 256 if off >= 128 else off)) & 0xFFFF
        else:
            v = raw[1] | raw[2] << 8 if size == 3 else (raw[1] if size == 2 else None)
        ops = d.fmt_operand(mn, mode, v).replace('a:', '') if v is not None or mode == 'acc' else ''
        print('%04X  %-8s  %s %s' % (pc, raw.hex(' '), mn, ops))
        pc += size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('baserom')
    ap.add_argument('--cdl')
    ap.add_argument('--peek', nargs=2, metavar=('ADDR', 'COUNT'),
                    help='print COUNT instructions linearly from ADDR and exit')
    args = ap.parse_args()

    rom = open(args.baserom, 'rb').read()
    assert rom[:4] == b'NES\x1a', 'not an iNES file'
    prg_size = rom[4] * 0x4000
    chr_size = rom[5] * 0x2000
    assert prg_size == 0x8000, 'expected a 32 KB PRG'
    prg = rom[16:16 + prg_size]
    chr_ = rom[16 + prg_size:16 + prg_size + chr_size]

    d = Disasm(prg)
    if args.peek:
        peek(d, int(args.peek[0], 16), int(args.peek[1]))
        return
    entries =[d.word(0xFFFA), d.word(0xFFFC), d.word(0xFFFE)]
    for a, n in zip(entries, ('NMI', 'Reset', 'IRQ')):
        d.add_label(a, 'sub')
        d.names.setdefault(a, n)
    entries += load_hints(os.path.join(ROOT, 'tools', 'hints.txt'), d)
    if args.cdl:
        entries += load_cdl(args.cdl, prg_size)
    d.trace(entries)
    d.finalize()

    out = []
    emit(d, out)
    os.makedirs(os.path.join(ROOT, 'src'), exist_ok=True)
    with open(os.path.join(ROOT, 'src', 'prg.s'), 'w', newline='\n') as f:
        f.write('\n'.join(out))

    # RAM symbols: only those referenced and without a user name
    ram = []
    for a in sorted(d.ram_refs):
        if a in HW_REGS or a >= 0x800:
            continue
        ram.append('%s = $%s' % (d.names.get(a, ram_name(a)), ('%02X' % a) if a < 0x100 else ('%04X' % a)))
    with open(os.path.join(ROOT, 'src', 'ram.inc'), 'w', newline='\n') as f:
        f.write('; RAM symbols (generated). Rename via "name ADDR NAME" in tools/hints.txt.\n\n')
        f.write('\n'.join(ram) + '\n')
        if d.consts:
            f.write('\n; Mapper registers (writes to ROM addresses)\n')
            for a in sorted(d.consts):
                f.write('%s = $%04X\n' % (d.consts[a], a))

    os.makedirs(os.path.join(ROOT, 'assets'), exist_ok=True)
    with open(os.path.join(ROOT, 'assets', 'header.bin'), 'wb') as f:
        f.write(rom[:16])
    with open(os.path.join(ROOT, 'assets', 'chr.bin'), 'wb') as f:
        f.write(chr_)

    code_bytes = sum(d.instr_size(a) for a in d.code)
    print('traced %d instructions, %d bytes of code (%.1f%% of PRG)' % (
        len(d.code), code_bytes, 100.0 * code_bytes / prg_size))


if __name__ == '__main__':
    main()

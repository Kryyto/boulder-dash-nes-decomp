#!/usr/bin/env python3
"""Replay the original routine of one fixture record on tools/nesemu.py and
trace it, to find where the C port diverges.

Prints every call to the addresses given with --at (with $4D/$4E/$55-56) and,
with --writes, every RAM write (address, value, PC).

Usage: python tools/fxtrace.py ROM FIXTURES INDEX [--at C468,...] [--writes]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nesemu  # noqa: E402

REC = 2 + 2 * (5 + 0x800)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('fixtures')
    ap.add_argument('index', type=int)
    ap.add_argument('--at', default='C468')
    ap.add_argument('--writes', action='store_true')
    args = ap.parse_args()
    at = {int(x, 16) for x in args.at.split(',') if x}

    data = open(args.fixtures, 'rb').read()
    version, count = struct.unpack_from('<II', data, 4)
    if not 0 <= args.index < count:
        sys.exit('record %d out of range (0-%d)' % (args.index, count - 1))
    off = 12
    for _ in range(args.index):  # version 2 records carry an APU log of variable length
        off += REC
        if version == 2:
            off += 2 + 3 * struct.unpack_from('<H', data, off)[0]
    addr = struct.unpack_from('<H', data, off)[0]
    a, x, y, p, s = data[off + 2:off + 7]
    ram0 = data[off + 7:off + 7 + 0x800]
    ram1 = data[off + 7 + 0x800 + 5:off + 7 + 0x800 + 5 + 0x800]

    nes = nesemu.NES(open(args.rom, 'rb').read())
    nes.ram[:] = ram0
    nes.a, nes.x, nes.y, nes.s = a, x, y, s
    nes.set_p(p)
    nes.pc = addr
    stop_s = (s + 2) & 0xFF
    pc = None
    if args.writes:
        orig_wr = nes.wr

        def wr(ad, v):
            if ad < 0x2000 and not (0x100 <= (ad & 0x7FF) < 0x200):
                print('  write $%04X = %02X  (pc $%04X)' % (ad & 0x7FF, v, pc))
            orig_wr(ad, v)
        nes.wr = wr
    print('record %d: routine $%04X' % (args.index, addr))
    for _ in range(2_000_000):
        pc = nes.pc
        if pc in at:
            r = nes.ram
            print('$%04X  row $4D=%02X col $4E=%02X ptr $55=%04X' % (pc, r[0x4D], r[0x4E], r[0x55] | r[0x56] << 8))
        op = nes.prg[pc - 0x8000]
        nes.step()
        if op == 0x60 and nes.s == stop_s:
            break
    same = bytes(nes.ram) == bytes(ram1) or all(
        nes.ram[i] == ram1[i] for i in range(0x800) if not 0x100 <= i < 0x200)
    print('replay matches the recorded result: %s' % same)


if __name__ == '__main__':
    main()

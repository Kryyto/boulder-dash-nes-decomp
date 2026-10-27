#!/usr/bin/env python3
"""Assemble src/ into build/bd.nes and compare it with the original ROM.

Usage: python tools/build.py [BASEROM]
BASEROM defaults to "Boulder Dash (USA).nes" in the project root.
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
BUILD = os.path.join(ROOT, 'build')
SOURCES = ['header.s', 'prg.s']


def run(cmd):
    r = subprocess.run(cmd, cwd=SRC)
    if r.returncode:
        sys.exit(r.returncode)


def main():
    baserom = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'Boulder Dash (USA).nes')
    os.makedirs(BUILD, exist_ok=True)
    objs = []
    for s in SOURCES:
        o = os.path.join(BUILD, os.path.splitext(s)[0] + '.o')
        run(['ca65', '-o', o, s])
        objs.append(o)
    out = os.path.join(BUILD, 'bd.nes')
    run(['ld65', '-C', 'nes.cfg', '-o', out, '-m', os.path.join(BUILD, 'bd.map'),
         '--dbgfile', os.path.join(BUILD, 'bd.dbg')] + objs)

    built = open(out, 'rb').read()
    orig = open(baserom, 'rb').read()
    if built == orig:
        print('OK: build/bd.nes matches (sha1 %s)' % hashlib.sha1(built).hexdigest())
        return
    diffs = [i for i in range(min(len(built), len(orig))) if built[i] != orig[i]]
    print('MISMATCH: sizes %d vs %d, %d differing bytes' % (len(built), len(orig), len(diffs)))
    for i in diffs[:20]:
        where = 'PRG $%04X' % (i - 16 + 0x8000) if 16 <= i < 16 + 0x8000 else 'file +%X' % i
        print('  %s: built %02X, original %02X' % (where, built[i], orig[i]))
    sys.exit(1)


if __name__ == '__main__':
    main()

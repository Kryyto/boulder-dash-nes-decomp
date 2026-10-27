#!/usr/bin/env python3
"""Produce build/coverage.cdl: run the coverage scripts and the bot on
tools/nesemu.py and merge their Code/Data Logs.

Usage:
  python tools/coverage.py [BASEROM]           run everything (about an hour), then merge
  python tools/coverage.py [BASEROM] --merge   only merge the build/*.cdl already there
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
SCRIPTS = ['menus.txt']  # in tools/scripts; tomap.txt is a prefix used by the bot
OUT = os.path.join(BUILD, 'coverage.cdl')


def merge(paths, out):
    acc = None
    for p in paths:
        raw = bytearray(open(p, 'rb').read())
        if acc is None:
            acc = raw
        else:
            for i in range(9, len(acc)):
                acc[i] |= raw[i]
    open(out, 'wb').write(acc)
    code = sum(1 for b in acc[9:9 + 0x8000] if b & 1)
    print('merged %d logs -> %s: %d code bytes (%.1f%% of PRG)' % (
        len(paths), os.path.relpath(out, ROOT), code, 100.0 * code / 0x8000))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    rom = args[0] if args else os.path.join(ROOT, 'Boulder Dash (USA).nes')
    os.makedirs(BUILD, exist_ok=True)
    if '--merge' not in sys.argv:
        py = sys.executable
        for s in SCRIPTS:
            cdl = os.path.join(BUILD, 'cov_' + os.path.splitext(s)[0] + '.cdl')
            subprocess.run([py, os.path.join(ROOT, 'tools', 'nesemu.py'), rom,
                            os.path.join(ROOT, 'tools', 'scripts', s), '--cdl', cdl], check=True)
        bot = [py, os.path.join(ROOT, 'tools', 'bot.py'), rom]
        passes = {
            'bot': [],                                           # really play all 24 caves
            'wclear': ['--preclear', '--cheat-exit', '--after', '40'],  # every world clear
            'clear': ['--cheat-exit', '--after', '20'],          # every cave clear
            'end1': ['--worlds', '5', '--stages', '0', '--preclear', '--cheat-exit',
                     '--after', '120'],                          # first ending
            'end2': ['--worlds', '5', '--preclear', '--cheat-exit', '--loop', '3',
                     '--after', '160'],                          # later-loop ending
        }
        for name, extra in passes.items():
            subprocess.run(bot + extra + ['--cdl', os.path.join(BUILD, 'cov_%s.cdl' % name)], check=True)
    logs = [p for p in glob.glob(os.path.join(BUILD, '*.cdl')) if os.path.abspath(p) != OUT]
    merge(logs, OUT)


if __name__ == '__main__':
    main()

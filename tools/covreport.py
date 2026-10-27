#!/usr/bin/env python3
"""Compare a Code/Data Log with the static trace from tools/disasm.py.

Usage: python tools/covreport.py BASEROM CDL [--all]

Prints code the log found that static tracing missed, and the statically
traced routines that never ran (what the coverage scripts still need to
exercise).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disasm as D  # noqa: E402


def ranges(addrs):
    out = []
    for a in sorted(addrs):
        if out and a == out[-1][1] + 1:
            out[-1][1] = a
        else:
            out.append([a, a])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('baserom')
    ap.add_argument('cdl')
    ap.add_argument('--all', action='store_true', help='list every unexecuted routine')
    args = ap.parse_args()

    rom = open(args.baserom, 'rb').read()
    prg = rom[16:16 + 0x8000]
    d = D.Disasm(prg)
    entries = [d.word(0xFFFA), d.word(0xFFFC), d.word(0xFFFE)]
    entries += D.load_hints(os.path.join(D.ROOT, 'tools', 'hints.txt'), d)
    d.trace(entries)
    static = set()
    for a in d.code:
        static.update(range(a, a + d.instr_size(a)))

    raw = open(args.cdl, 'rb').read()
    flags = raw[9:9 + 0x8000] if raw[:5] == b'CDLv2' else raw[:0x8000]
    ran = {D.BASE + i for i, f in enumerate(flags) if f & 1}

    new = ran - static
    print('executed code: %d bytes; static trace: %d bytes; overlap %d' % (
        len(ran), len(static), len(ran & static)))
    print('\nexecuted but not statically traced (%d bytes):' % len(new))
    for s, e in ranges(new):
        print('  $%04X-$%04X' % (s, e))

    # routines (label starts) in the static trace that never ran
    starts = sorted(a for a, k in d.labels.items() if k == 'sub' and a in d.code)
    cold = []
    for i, a in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else 0x10000
        body = [x for x in static if a <= x < end]
        if body and not any(x in ran for x in body):
            cold.append((a, len(body)))
    print('\nroutines never executed: %d (%d bytes)' % (len(cold), sum(n for _, n in cold)))
    for a, n in (cold if args.all else sorted(cold, key=lambda c: -c[1])[:30]):
        print('  %s  %d bytes' % (d.label_name(a), n))
    unrun = static - ran
    print('\nstatic code not executed: %d bytes (%.1f%% of traced code)' % (
        len(unrun), 100.0 * len(unrun) / max(1, len(static))))


if __name__ == '__main__':
    main()

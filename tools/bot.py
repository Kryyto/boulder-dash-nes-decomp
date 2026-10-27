#!/usr/bin/env python3
"""Coverage bot for Boulder Dash running on tools/nesemu.py.

Reads the cave grid from RAM and walks with BFS: first to diamonds, then to
the exit. Save states let it retry after dying. Used to drive the game through
cave completion so that code shows up in the Code/Data Log.

RAM facts this relies on (found with nesemu):
  $1F game state ($0A cave intro, $0B playing, $0D lost life)
  $75 world (high nibble) / stage (low nibble) for player 1
  $3E0 cave grid, 40 x 22 bytes; high nibble = tile type
       0 space, 2 dirt, 3 steel, 4 exit, 5 brick, 7 boulder, 8 diamond, E player

Usage: python tools/bot.py ROM [--cdl OUT] [--worlds 0-5] [--stages 0-3]
"""
import argparse
import collections
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nesemu  # noqa: E402

GRID, W, H = 0x3E0, 40, 22
DIRS = {'up': (0, -1), 'down': (0, 1), 'left': (-1, 0), 'right': (1, 0)}
PASSABLE = {0x0, 0x2, 0x8}


def grid(nes):
    return nes.ram[GRID:GRID + W * H]


def find_player(g):
    for i, b in enumerate(g):
        if b >> 4 == 0xE:
            return i % W, i // W
    return None


def bfs(g, start, targets, allow, rng, banned=()):
    prev = {start: None}
    q = collections.deque([start])
    order = list(DIRS.items())
    while q:
        p = q.popleft()
        if p in targets and p != start:
            # walk back to the first step
            while prev[p] != start:
                p = prev[p]
            for name, (dx, dy) in DIRS.items():
                if (start[0] + dx, start[1] + dy) == p:
                    return name
        rng.shuffle(order)
        for name, (dx, dy) in order:
            n = (p[0] + dx, p[1] + dy)
            if not (0 <= n[0] < W and 0 <= n[1] < H) or n in prev:
                continue
            if p == start and name in banned:
                continue
            t = g[n[1] * W + n[0]] >> 4
            if (t in allow or n in targets) and safe(g, n, t):
                prev[n] = p
                q.append(n)
    return None


KNOWN = {0x0, 0x2, 0x3, 0x4, 0x5, 0x7, 0x8, 0xE}


def safe(g, p, t):
    x, y = p
    above = g[(y - 1) * W + x] if y > 0 else 0x30
    # never stand under a boulder/diamond: it can start falling at any time
    if above >> 4 in (0x7, 0x8):
        return False
    # nor under something already falling (low nibble set) further up the column
    yy = y - 1
    while yy > 0 and g[yy * W + x] >> 4 == 0x0:
        yy -= 1
    if g[yy * W + x] >> 4 in (0x7, 0x8) and g[yy * W + x] & 0x0F:
        return False
    # keep away from anything that isn't a known static tile (enemies, amoeba...)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H and g[ny * W + nx] >> 4 not in KNOWN:
            return False
    return True


def step(nes, move, me):
    """Hold a direction until the player leaves tile `me` (24 frames max).
    Returns True if the player moved."""
    nes.joy = nesemu.BUTTONS[move]
    moved = False
    for _ in range(24):
        nes.run_frame()
        if nes.ram[0x1F] != 0x0B or find_player(grid(nes)) != me:
            moved = True
            break
    nes.joy = 0
    return moved


def dies_after(nes, move, me):
    """Try a move on a copy of the machine and watch it for about a second."""
    c = _strip(nes)
    step(c, move, me)
    for _ in range(40):
        if c.ram[0x1F] != 0x0B or dying(c):
            break
        c.run_frame()
    return c.ram[0x1F] == 0x0D or dying(c)


def dying(nes):
    # $91 low nibble 3 = death sequence (counts down $92, then state $0D)
    return nes.ram[0x1F] == 0x0B and nes.ram[0x91] & 0x0F == 3


def choose(nes, g, me, rng, lookahead):
    diamonds = {(i % W, i // W) for i, b in enumerate(g) if b >> 4 == 0x8}
    exits = {(i % W, i // W) for i, b in enumerate(g) if b >> 4 == 0x4}
    banned = set()
    for _ in range(4):
        move = bfs(g, me, diamonds, PASSABLE, rng, banned) if diamonds else None
        if move is None:
            move = bfs(g, me, exits, PASSABLE, rng, banned)
        if move is None:
            left = [d for d in DIRS if d not in banned]
            if not left:
                break
            move = rng.choice(left)
        if not lookahead or not dies_after(nes, move, me):
            return move
        banned.add(move)
    return rng.choice(list(DIRS))


def cheat_exit(nes):
    """Open the exit ($90) and put an exit tile next to the player, then walk
    into it: the game's own cave-clear path runs from there."""
    while nes.ram[0x1F] == 0x0B and find_player(grid(nes)) is None:
        nes.run_frame()
    me = find_player(grid(nes))
    if me is None:
        return nes, 'no player'
    for name, (dx, dy) in DIRS.items():
        x, y = me[0] + dx, me[1] + dy
        if 0 < x < W - 1 and 0 < y < H - 1:
            nes.ram[0x90] = 1
            nes.ram[GRID + y * W + x] = 0x40
            step(nes, name, me)
            for _ in range(30):
                nes.run_frame()
            break
    if nes.ram[0x1F] == 0x0B:
        return nes, 'cheat failed'
    return nes, 'left state $%02X' % nes.ram[0x1F]


def play_cave(nes, rng, max_steps=1500, lookahead=False):
    """Play state $0B until it is left. Returns (nes, outcome).

    On death, rewinds to an older checkpoint each time so that a checkpoint
    taken just before an unavoidable death doesn't trap the bot. With
    lookahead, each move is first tried on a copy of the machine."""
    checkpoints = [_strip(nes)]
    tries = 0
    steps = 0
    stuck = 0
    while steps < max_steps:
        if nes.ram[0x1F] == 0x0D or dying(nes):  # lost a life: rewind
            tries += 1
            if tries > 40:
                return nes, 'died'
            if len(checkpoints) > 1:
                checkpoints.pop()
            cdl = nes.cdl
            nes = nesemu.copy.deepcopy(checkpoints[-1])
            nes.cdl = cdl
            continue
        if nes.ram[0x1F] != 0x0B:
            break
        g = grid(nes)
        me = find_player(g)
        if me is None:
            nes.joy = 0
            nes.run_frame()
            continue
        if stuck > 6:
            # no progress: dig/push in a random direction
            move = rng.choice(list(DIRS))
            stuck = 0
        else:
            move = choose(nes, g, me, rng, lookahead)
        if not step(nes, move, me):
            stuck += 1
        steps += 1
        if steps % 15 == 0 and nes.ram[0x1F] == 0x0B:
            checkpoints.append(_strip(nes))
            del checkpoints[:-8]
    if nes.ram[0x1F] == 0x0B:
        return nes, 'timeout'
    return nes, 'left state $%02X' % nes.ram[0x1F]


def _strip(nes):
    """Copy-friendly view: share the CDL instead of copying it."""
    cdl = nes.cdl
    nes.cdl = None
    c = nesemu.copy.deepcopy(nes)
    nes.cdl = cdl
    c.cdl = cdl
    return c


def frames(nes, n, joy=0):
    nes.joy = joy
    for _ in range(n):
        nes.run_frame()
    nes.joy = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--cdl')
    ap.add_argument('--worlds', default='0-5')
    ap.add_argument('--stages', default='0-3')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--shots')
    ap.add_argument('--preclear', action='store_true',
                    help='mark the other stages of the world as cleared, so a win completes the world')
    ap.add_argument('--lookahead', action='store_true',
                    help='try each move on a copy of the machine first (slower, dies less)')
    ap.add_argument('--cheat-exit', action='store_true',
                    help="don't play: open the exit and step into it right away")
    ap.add_argument('--loop', type=lambda s: int(s, 0),
                    help='set the loop counter $74 (difficulty / which ending)')
    ap.add_argument('--after', type=int, default=12,
                    help='rounds of 50 frames + A press to run after leaving a cave')
    args = ap.parse_args()
    rng = random.Random(args.seed)
    span = lambda s: range(int(s.split('-')[0]), int(s.split('-')[-1]) + 1)

    nes = nesemu.NES(open(args.rom, 'rb').read())
    here = os.path.dirname(os.path.abspath(__file__))
    script = open(os.path.join(here, 'scripts', 'tomap.txt')).read().split('\n')
    nes = nesemu.run_script(nes, script)
    box = _strip(nes)
    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
    try:
        for w in span(args.worlds):
            for s in span(args.stages):
                nes = nesemu.copy.deepcopy(box)
                nes.cdl = box.cdl
                nes.ram[0x75] = w << 4 | s
                if args.preclear:
                    nes.ram[0x76] = 0x0F ^ (1 << s)
                nesemu.run_script(nes, ['tap a'])
                while nes.ram[0x1F] != 0x0B and nes.frame < box.frame + 2000:
                    frames(nes, 1)
                if args.loop is not None:
                    nes.ram[0x74] = args.loop
                if args.cheat_exit:
                    nes, outcome = cheat_exit(nes)
                else:
                    nes, outcome = play_cave(nes, rng, lookahead=args.lookahead)
                print('world %d stage %d: %s at frame %d' % (w + 1, s + 1, outcome, nes.frame), flush=True)
                if outcome.startswith('left'):
                    # let whatever follows (bonus count, map, next cave) run, pressing A now and then
                    for k in range(args.after):
                        frames(nes, 50)
                        frames(nes, 4, nesemu.BUTTONS['a'])
                    if args.shots:
                        nes.screenshot(os.path.join(args.shots, 'after_%d%d.png' % (w, s)))
                    print('   then state $%02X' % nes.ram[0x1F], flush=True)
    finally:
        if args.cdl:
            nes.write_cdl(args.cdl)


if __name__ == '__main__':
    main()

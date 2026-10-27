# Boulder Dash (NES): decompilation and native C port

A byte-matching disassembly of **Boulder Dash** for the NES (USA release), and a
native C11 + SDL2 port of the whole game built on top of it. The port runs on
your PC without an emulator. It covers every screen: boot screen, title and
attract demo, menus, password, world map, all 24 caves and 4 difficulty
loops, game over, the scenes after each world, the ending and the credits,
with the original music and sound effects.

> **Unofficial fan project.** It is not affiliated with, endorsed by or
> connected to BBG Entertainment, First Star Software, Victor Musical
> Industries / JVC, Data East or Nintendo. "Boulder Dash" and "NES" are
> trademarks of their respective owners. **This repository contains no game
> code, graphics, levels, music or text from the game**, and no ROM.

## You need your own ROM

**Nothing here works without a ROM of the game.** The port reads everything it
needs from the ROM when it starts: the code tables, the caves, the graphics,
the music. The disassembly is also generated from it. You must use a copy
you own legally, for example one dumped from your own cartridge. Don't ask
for ROMs in the issues, and never commit one: `.gitignore` excludes `*.nes`.

The expected file is the USA release:

| | |
|---|---|
| File name (default) | `Boulder Dash (USA).nes`, in the repository's root |
| SHA-1 | `3aa63c41990e8d544ee79dd32701f51f09bb82ea` |
| Header | iNES / NES 2.0, mapper 1 (MMC1), 32 KB PRG, 32 KB CHR |

Other releases or modified ROMs are not supported (the port only checks the sizes in the header).

## Play

You need CMake (3.20+) and a C11 compiler. SDL2 is used if it is installed,
otherwise CMake downloads and builds it (internet needed the first time).

**Windows** (tested with MinGW-w64 GCC):

```bash
cmake -S port -B build/cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j 8
build/cmake/bdview.exe "Boulder Dash (USA).nes"
```

**Linux / macOS** (should work the same way, not tested yet; when SDL2 is
built from source on Linux it needs the X11 or Wayland development
packages, or install `libsdl2-dev` first):

```bash
cmake -S port -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j 8
build/cmake/bdview "Boulder Dash (USA).nes"
```

| Key | NES | |
|---|---|---|
| Arrows | D-pad | |
| Z / X | A / B | hold A or B + an arrow to snap (dig or grab without moving); hold A+B to give up |
| Enter | Start | pause in a cave |
| Right Shift or Tab | Select | 1 / 2 players on the title |
| F5 | | power on again |
| Space, `.` | | freeze, step one frame |
| + / - | | speed up, back to normal |
| PgUp / PgDn | | jump to the previous / next cave (a test aid, not in the game) |
| Esc | | quit |

`bdview "Boulder Dash (USA).nes" 5` starts a 1-player game directly in cave
5 (0-23). `--scale N` sets the window size (default 3x), `--mute` turns the
sound off. In a 2-player game the players take turns on the same keyboard.

## Testing: what helps

The goal is a port that behaves **exactly** like the original. Useful reports:

- anything that looks, sounds or plays differently from the original game on
  a NES or an accurate emulator: a cave, a screen, a sound, the timing;
- crashes or freezes, with what you did just before.

For reports, the window title shows the current screen and cave. A headless
run can reproduce a problem from scripted input and save the final frame:

```bash
build/cmake/bdview "Boulder Dash (USA).nes" 0 --shot out.bmp 600 --keys RRRRDDDD
```

Each character of `--keys` holds a button for 8 frames: `U D L R` d-pad,
`u d l r` A + direction, `A`, `B`, `S` Start, `T` Select, `G` A+B, `.` nothing.

## How the port is checked

Each routine of the game was translated from the disassembly. Each one makes
the same memory writes as the original, scratch bytes included, and the same
hardware accesses in the same order. `tools/record.py` runs the original game
on a small Python NES emulator (`tools/nesemu.py`). It saves the RAM before
and after each call, and every PPU, APU, joypad and mapper access. The
`ramdiff` test replays those calls through the C code and compares
everything. The fixtures come from your ROM, so you record them yourself
(Python 3 needed; the cave sets take a long time):

```bash
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_screens.bdfx --screens
python tools/record.py "Boulder Dash (USA).nes" build/fixtures.bdfx
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_fuzz.bdfx --fuzz --per-cave 80
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_sound.bdfx --sound
cmake -S port -B build/cmake    # picks up the fixture files that exist
ctest --test-dir build/cmake
```

On the author's machine, every record of the four sets matches, about 144,000
calls in all. [port/README.md](port/README.md) explains the design: the game's
RAM as a C struct, the hardware layer and the PPU model behind it, and how
the viewer renders and paces frames.

## The disassembly

`tools/disasm.py` generates the annotated source `src/prg.s` (ca65 syntax) from
your ROM and `tools/hints.txt`. The hints are the knowledge part: entry points,
jump tables, names and comments. `tools/build.py` reassembles it and checks that
the result is identical to your ROM. It needs Python 3 and
[cc65](https://cc65.github.io/) (`ca65`, `ld65`) on `PATH`.

```bash
python tools/disasm.py "Boulder Dash (USA).nes"
python tools/build.py
```

`docs/game-notes.md` describes how the game works: frame structure, states,
caves, the object scan, the player, enemies, amoeba, the sound driver.

## Layout

```
port/           the C port: game code (src/), RAM-diff test (tests/), SDL2 player (src/viewer.c)
tools/          disassembler + hints, build/verify, NES emulator, fixture recorder, coverage tools
docs/           notes on how the game works
src/            assembler glue; prg.s and ram.inc are generated from your ROM
```

Generated from the ROM, and never committed: `src/prg.s`, `src/ram.inc`,
`assets/` (header and CHR), `build/` (assembled ROM, fixtures, binaries).

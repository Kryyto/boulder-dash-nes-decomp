# C port

A C11 port of Boulder Dash (NES) on top of the disassembly, checked routine
by routine against the original with RAM diffs.

## Design

- **The game state is the NES RAM.** `include/bd_ram.h` is a 2 KB union
  generated from `tools/hints.txt` by `tools/genram.py`: every named variable
  sits at its NES address (checked with `_Static_assert`), and `R(addr)`
  reaches anything not named yet. Regenerate after renaming RAM in the hints.
- **Data comes from the user's ROM at run time** (`src/rom.c`); nothing from the
  game is embedded. Named ROM tables are in the generated `include/bd_syms.h`.
- **First pass is a faithful translation**: each C routine makes the same RAM
  writes as the 6502 code, scratch bytes included, so whole-RAM comparison
  works. Cleaning up into idiomatic C can come later, under the same tests.
- **The hardware is behind `io_write` / `io_read`** (`src/io.c`): every PPU,
  APU, joypad and MMC1 access of the ported code goes through them, in the
  original's order. Behind them is a model of the hardware: MMC1 (serial
  writes, CHR banks, mirroring), the PPU (nametable RAM, palette RAM, OAM, the
  address and scroll latches, the PPUDATA read buffer, CHR ROM through the
  banks) and the joypads. `ppu_render` draws a frame from it the way the PPU
  does, line by line from the scroll registers, with the split the title
  makes at sprite 0. Nothing is drawn from game RAM directly.

## Ported so far

| routine | address | file | fixtures |
|---|---|---|---|
| `LoadCave` | $B789 | `src/cave.c` | match |
| `ApplyCavePatches` | $B8AE | `src/cave.c` | match |
| `ScanCave` and its callees (gravity, magic wall, fireflies, butterflies, explosions, scoring, extra lives, sound requests) | $C413 | `src/scan.c`, `src/core.c` | match |
| `PlayerInput` (idle, walk, snap, push, dying, out of time) and `CollectDiamond` | $C06B, $CC8F | `src/player.c` | match |
| `PlayerHazards` (give up, enemy contact) | $C373 | `src/player.c` | match |
| `TickTime` | $CFC6 | `src/player.c` | match |
| `AmoebaUpdate` (edge walker, growth) | $CE51 | `src/amoeba.c` | match |
| `MagicWallTimer`, `AmoebaSound` | $CDAE, $C044 | `src/amoeba.c` | match |
| `CopyPlayerInput` | $D060 | `src/core.c` | match |
| `PlayerSprite`, `DrawMetaspriteWorld` | $D398, $A505 | `src/gfx.c` | match |
| `UpdateCamera` (step logic, row/column strips, metatile + attribute queues) | $B197 | `src/camera.c` | match |
| `UpdateStatusSprites` (score / diamonds / time digits) | $D2AD | `src/camera.c` | match |
| `HideUnusedSprites` | $A5E0 | `src/gfx.c` | not covered |
| `State0A_CaveIntro` (the camera pans across the cave, then play starts) | $AB5E | `src/state.c` | match (whole frames) |
| `State0B_CavePlay` (play, pause box, time-up box, score popups, attract demo input, end of frame) | $AB76 | `src/state.c` | match (whole frames) |
| `State0C_CaveCleared` (bonus tier, rising bonus sprite, time-to-score count, fade out, stage/world/loop progress) | $ACA1 | `src/state.c` | match (whole frames) |
| `State0D_LostLife` (lives, 2P turns, GAME OVER box, fade out) | $ADF4 | `src/state.c` | match (whole frames) |
| `BuildMetatileQueue`, `DrawMetaspriteScreen` | $CA4D, $A5AA | `src/state.c` | via the states |
| NMI handler: OAM DMA, scroll, MMC1 state, palette upload, rendering on/off, VRAM buffer, metatile queues, then the sound driver | $A2C7, $B001 | `src/nmi.c` | match, RAM and hardware |
| `WaitForNMI`, `WaitFrames`, `FadeOut`, `BlackoutNow` | $A0C9, $A4C0, $A48A, $A4F5 | `src/nmi.c` | via the states |
| PPU, mapper and joypad helpers (`ClearScreen`, `DrawPPUString`, `LoadNametable`, `ReadVramTo600`, palettes, `FadeIn`, `MMC1_Set*`, `WaitSprite0Hit`, `ReadJoypads`...) | $A0A2-$A4F4 | `src/screen.c` | via the states |
| `Reset`, `InitGame`, the main loop's dispatch | $FF8C, $A022, $A00B | `src/title.c` | |
| `State00_BootScreen`, `State01_Title` (logo, the two Rockfords, scores, 1P/2P, attract demo) | $A618, $A67E | `src/title.c` | match (whole frames) |
| `State02_TitleMenu` (lives, COLOR SELECT), `State03_Password`, `State05/06_WorldSelect`, `State07/08_StageIntro`, `State09_CaveSetup`, `State0E_GameOver` | $A686-$AB2E, $AEA3 | `src/menu.c` | match (whole frames) |
| `State04_NextStage` (the six world scenes), `State0F_Ending`, `State10_EndingScene`, `State11_Credits` (credits), `State12_NextLevel` | $A9E1, $EABB-$EFF5 | `src/ending.c` | match (whole frames) |
| `Sound_Init`, `Sound_Update` (the whole driver: tempo, music and effect streams, all 51 commands, register writes) | $8011, $81B4 | `src/sound.c` | match, RAM and APU writes |
| `SetCell`, `SetGameState` | $D077, $A258 | `src/core.c` | via callers |

That is the whole program: every routine the game runs is ported (the
disassembly's dead code aside).

The sound driver writes the APU through `io_write`. `ramdiff` captures
those writes and compares them in order with the original's. `bdview` sends
them to `src/apu.c`, an APU emulation (pulse x2 with envelope, sweep and
length counter, triangle with linear counter, noise; the game doesn't use the
DMC) mixed with the NES's non-linear mixer and output through SDL audio.

The driver lets the 6502 registers leak between routines: some paths return
with X = channel x 4 or channel + 4, and one case (ending a music channel)
advances the stream pointer by the Y that the pitch writer left. The port
reproduces these, which the fixtures caught.

## RAM-diff tests

`tools/record.py` runs the game on `tools/nesemu.py` and saves the registers
and full RAM at entry to and return from every call of the watched routines
(any NMI is held back until the call returns). State handlers, which the
main loop enters by an indirect JMP and which end with `JMP MainLoop`, are
recorded from their entry until `MainLoop`: one whole frame of the state.
A frame that fades out waits for several frames inside (`WaitForNMI`); there
the recorder lets exactly one NMI through per wait, in the wait's spin loop.
The port's `WaitForNMI` calls `bd_frame_hook`, which by default applies the
NMI handler's RAM effects (`NmiHandler`), so the two line up. `ramdiff` loads each entry RAM,
runs the C routine, and compares all 2 KB with the original's result, skipping
only the stack bytes the original's JSRs write.

```bash
python tools/record.py "Boulder Dash (USA).nes" build/fixtures.bdfx
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_fuzz.bdfx --fuzz --per-cave 80
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_sound.bdfx --sound
python tools/record.py "Boulder Dash (USA).nes" build/fixtures_screens.bdfx --screens
```

Each record also carries the hardware accesses made during the call. Version
3 files (what `record.py` writes now) log all of them in order: PPU, APU,
joypads, mapper writes, and reads with the value read. `ramdiff` checks
that the port writes the same sequence. When the port reads a register, it
gets the value the original read from it, since the hardware's state at
entry isn't in the fixture. Version 2 files, recorded before the hardware
layer, log the APU writes only, and only those are compared.

`--sound` starts each of the 35 sound ids from
the world map, alone and in 20 random music + effect pairs, and records every
`Sound_Update` call for 240 frames, so whole streams are exercised.

`--screens` plays through everything outside the caves and records every
frame of it, plus a sample of NMIs (entry to RTI):
- the cold boot, the boot screen and the title, three attract demos;
- the title menu with COLOR SELECT;
- for each world, its password, then its scene;
- the world map and a stage intro and cave setup for each of the 24 caves;
- game over with CONTINUE and with END, and a 2-player END where the other
  player goes on;
- the ending from loops 0-3: the next-level screen three times, then the credits.

The plain run records real play in all 24 caves (loop 0 and 2). `--fuzz`
seeds random situations into RAM right before each recorded call and lets
the original react. For `ScanCave`: falling boulders over enemies and the player,
magic walls, explosion tiles, amoeba states, scores near an extra life, a
full redraw list. For the amoeba and magic wall: random blobs (some fully
enclosed), walker and origin cells, grow countdowns about to hit zero, magic
wall timers about to expire. For the player routines: a random cell and surroundings,
phase and timer, buttons (including snap pushes and A+B), diamonds and time
nearly done. After recording, CPU and RAM are rewound so the game itself
runs on unaffected. That reaches paths normal play rarely hits.

## Build

Needs CMake and a C11 compiler (tested with MinGW-w64 GCC 16 on Windows).
SDL2 is taken from the system, or fetched from libsdl-org and built
statically when not found (`-DBD_VIEWER=OFF` skips the viewer).

```bash
cmake -S port -B build/cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j 8
build/cmake/ramdiff "Boulder Dash (USA).nes" build/fixtures.bdfx -v
build/cmake/ramdiff "Boulder Dash (USA).nes" build/fixtures_fuzz.bdfx -v
build/cmake/ramdiff "Boulder Dash (USA).nes" build/fixtures_sound.bdfx -v
build/cmake/ramdiff "Boulder Dash (USA).nes" build/fixtures_screens.bdfx -v
ctest --test-dir build/cmake
```

`ramdiff -v` lists the differing bytes of the first failures, `-r ScanCave`
limits the run to one routine. To see what the original did on a failing
record, `python tools/fxtrace.py ROM FIXTURES INDEX --at C468 --writes`
replays it in nesemu and prints calls and RAM writes.

Status: every record matches: normal, fuzzed, sound and screens, RAM and
hardware accesses. See the commit log for the latest counts. gcov shows the
fixtures exercise about 96-98% of `src/scan.c` and `src/player.c`, and
94-99.6% of the screen code (`title.c`, `screen.c`, `menu.c`, `ending.c`,
`nmi.c`; what's left is mostly `Reset`, which no fixture starts from).

`nesemu.py` emulates both joypads, so the recorder plays player 2's turns
on pad 2, as the game reads them.

One lesson from fuzzing: the game can compute pointers outside RAM (a dead
player's row below the cave sends `CaveRowPointers` past its end). On the
NES those accesses hit unmapped space, so `(zp),y` in the port goes through
`bus_ref()`, which follows the bus instead of wrapping into RAM.

## Playing: bdview

`bdview ROM` plays the whole game in an SDL2 window. It powers on and runs
the ported program as the NES does: `Reset`, then the main loop (wait for
the frame, read the pads, run the state's handler). Everything you see and
hear comes from the ported code:
- the boot screen, the title with its attract demos;
- 1 or 2 players, the title menu with COLOR SELECT, the password screen;
- the world map with its stage marks, the stage intros;
- the caves, the game over screen (CONTINUE / END);
- the scene after each world, the ending, the credits, and the next level
  with its password.

At each frame (`WaitForNMI`) the viewer draws what the PPU model shows,
then runs the NMI if the game has enabled it (PPU updates, sound driver).
It feeds the APU writes to the APU emulation and reads the keyboard into
both joypads (in a 2-player game the players take turns, each on their own
pad), then keeps to the NES's 60.1 frames a second (by the audio clock, or a timer when muted), whatever the display's refresh rate. The picture is the NES's 256x240,
scaled 3x by default (`--scale N`). It is rendered from VRAM: the nametables
the game wrote, scrolled by the scroll registers, with the sprites from OAM
(8 per line, priorities, behind-background). In caves, a column the camera
brings in can briefly show old tiles, as on the NES.

Keys: arrows are the d-pad, **Z / X are A / B**, Enter is Start, Right Shift
or Tab is Select. In a cave, hold A or B plus an arrow to snap (dig or
collect without moving; A + Left/Right pushes a boulder from where you
stand), and hold A+B to give up. Viewer keys:
- F5 powers on again; Esc quits.
- Space freezes, `.` steps one frame, +/- change the speed.
- PgUp/PgDn jump to the previous/next cave, a test aid that isn't part of the game.

`bdview ROM 5` starts a 1-player game directly in cave 5 (0-23).

Sound: music and effects come from the ported driver through the APU
emulation (`--mute` turns it off). `--wav FILE` with `--shot` saves the audio
of a headless run.

`bdview ROM --shot out.bmp 900 --keys ...` runs without a window, with
scripted input on pad 1, and saves the last frame at 1x. The keys are one
character per 8 frames:
- U D L R: d-pad; u d l r: A + direction;
- A, B; S: Start; T: Select; G: A+B held (give up);
- E: opens the exit right of the player (a test aid); `.`: nothing.

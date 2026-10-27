# How Boulder Dash (NES) works

Notes from the disassembly (`tools/hints.txt` has the names). "?" marks
things not yet confirmed.

## Frame structure

`MainLoop` ($A00B) runs once per frame:

1. `WaitForNMI`: spin until the NMI sets `nmi_done`.
2. `ReadJoypads`: read both pads twice and only accept matching reads, then
   compute held / newly pressed buttons.
3. Jump to `GameStateTable[game_state]`. Each handler ends with `jmp MainLoop`.

The NMI ($A2C7) does, when `nmi_ppu_enable` is set: OAM DMA from `oam_buf`,
then `NmiPpuUpdate`. It always runs `Sound_Update`.

`NmiPpuUpdate` restores scroll, PPUCTRL, the MMC1 control and both CHR banks,
then runs one handler per bit of `nmi_requests`:

| bit | handler | does |
|---|---|---|
| 0 | `Nmi_UploadPalette` | `palette_buf` (32 bytes) -> $3F00 |
| 1 | `Nmi_RenderingOn` | PPUMASK |= $18 |
| 2 | `Nmi_RenderingOff` | PPUMASK &= ~$18 |
| 3 | `Nmi_FlushVramBuffer` | `vram_buffer`: [lo, hi, count, bytes…]… $FF |
| 4 | `Nmi_FlushMetatiles4` | 4 metatile + attribute entries |
| 5 | `Nmi_FlushMetatiles6` | 6 metatile + 6 attribute entries |

Metatile entries (`metatile_queue`, 6 bytes): PPU address lo/hi ($FF = empty)
and the 4 tiles of a 16×16 cell. Attribute entries (`attr_queue`, 3 bytes):
address lo/hi and the byte.

## Game states

| id | handler | what |
|---|---|---|
| $00 | BootScreen | cold boot only; any button or ~4 s |
| $01 | Title | logo, high scores, 1P/2P; idles into the attract demo |
| $02 | TitleMenu | world map with "N PLAYER / COLOR SELECT"; Left/Right colour, A password |
| $03 | Password | password entry |
| $04 | NextStage | after a password or a cleared world: the world's scene ("TRY THE NEXT STAGE?"), typed text, then the screen scrolls left onto the map |
| $05/$06 | WorldSelect | walk the map (`WorldSelect_Input`), A enters a stage |
| $07/$08 | StageIntro | "WORLD n-m" box ($07 never set by the game) |
| $09 | CaveSetup | `LoadCave`, `ApplyCavePatches`, `CaveFirstScreen` (draws the first screenful by scrolling the camera over it from 256 pixels to the left), fade in |
| $0A | CaveIntro | reveal; Start skips the demo |
| $0B | CavePlay | the game |
| $0C | CaveCleared | entered the open exit: time bonus, stage bit, next world |
| $0D | LostLife | lives - 1, retry or game over |
| $0E | GameOver | the world's password, CONTINUE (the world from its first stage, 3 lives, score reset) or END (the other player goes on in 2-player games) |
| $0F | Ending | after world 6: Rockford jumps about while the text types; A/B goes on |
| $10 | EndingScene | the first world's scene again, then $12 |
| $11 | Credits | when the new loop is 4 or more (`ram_0408`): each world's scene with names, then the last screen, which stays (in a 2-player game, A/B hands over to the other player) |
| $12 | NextLevel | "TRY THE NEXT LEVEL!" + the password of the next loop; Start goes to the map |

The scenes ($04, $10, $11) share one engine: `cave_grid[0]` is the scene
number (the world), the objects' table is copied after it, and `TypeText`
($E853) writes one character every 4 frames from a text stream (FC = jump,
FD = next line, FE = position, FF = end) into the VRAM buffer. The scene is
drawn on the second nametable (`ppuctrl` nametable bits 3, vertical
mirroring), its picture and text read out of CHR ROM with `ReadVramTo600`.

The title splits the screen: `WaitSprite0Hit` waits for sprite 0 (at y $D8),
then the code points the PPU at $2B40 with a fixed scroll, so the score panel
below stays put while the logo above scrolls.

Progress lives in a 10-byte per-player block (`PlayerOffset` gives 0 or $0A):
`loop_count`, `world_stage` (world in the high nibble, stage in the low),
`stages_cleared` (bit per stage), `lives`, `score`. Clearing all 4 stage bits
advances the world; after world 6 `loop_count` increments (harder caves, ending).

## Caves

24 caves (cave index = world × 4 + stage, `WorldStageToCave`).

- **Header** (`CaveHeaderTable`, 12 bytes): start row, start column, diamond
  value, bonus diamond value, then (diamonds needed, time) for loop 0-3.
- **Layout is in CHR ROM.** `CaveLayoutLocations` gives a CHR bank and address.
  `LoadCave` switches CHR1 and reads 22 rows × 20 bytes through PPUDATA; each
  byte holds two tiles (low nibble first), expanded to a 40×22 byte grid at
  `cave_grid` ($03E0). `CaveRowPointers` ($D08B) points at each row.
- **Patches** (`ApplyCavePatches`): from loop 2 onwards a per-cave list of
  (row, column, tile) changes is applied.

Tile byte: high nibble = type, low nibble = state (bit 0 falling, bit 3
processed in this scan).

| type | tile | type | tile |
|---|---|---|---|
| 0 | space | 8 | diamond |
| 1 | vacated (becomes space next pass) | 9 | explosion → diamond |
| 2 | dirt | A | explosion → space |
| 3 | steel wall | B | firefly (explodes to space) |
| 4 | exit / inbox | C | butterfly (explodes to diamonds) |
| 5 | brick wall | D | amoeba |
| 6 | magic wall | E | player |
| 7 | boulder | | |

## Cave play ($0B)

Each frame: `UpdateStatusSprites`, other per-frame updates, then (unless
paused) `PlayerHazards`, `ScanCave`, `PlayerInput`, `MagicWallTimer`,
`AmoebaUpdate`, `TickTime`, `AmoebaSound`, `UpdateCamera`,
`BuildMetatileQueue`.

**`ScanCave`** is spread over 8 frames using `frame_phase & 7`:

- phase 0: nothing
- phases 1-4: `ProcessCell` over 5 rows each, bottom-up (rows 20-1),
  columns 38→1
- phases 5-7: `ProcessCell2` over 7/7/6 rows top-down

So every object moves at most once per 8 frames. `ProcessCell` handles
gravity for boulders and diamonds: falling, landing (with sound), rolling off
round objects and brick, and magic wall conversion (a boulder falling through
becomes a diamond two rows down). It also crushes the player (`KillPlayer`)
and enemies (`Explode3x3`). `ProcessCell2` clears the processed bits, turns
vacated cells into space and animates explosions.

Changed cells are added to `redraw_list` (`QueueCellRedraw`, max 55);
`BuildMetatileQueue` turns them into metatile/attribute entries for the NMI.

## Player

The position is in pixels (`player_y`, `player_x`, 16-bit); the cell is the
position / 16 (`GetPlayerCell`). In the grid the player is tile `E0`.
`player_phase` holds the direction in bits 4-5 (0 up, 1 down, 2 left, 3 right)
and a phase in the low nibble, dispatched by `PlayerInput`:

| phase | handler | |
|---|---|---|
| 0 | `Player_Idle` | reads the d-pad on frames where `frame_phase & 7 == 0` |
| 1 | `Player_Walking` | 8 frames × 2 px toward the new cell |
| 2 | `Player_Pushing` | 24 frames pushing on a boulder, then it moves if space is behind it |
| 3 | `Player_Dying` | sprite thrown upward with gravity, then state $0D |
| 4 | – | standing in the exit |
| 6, 7, 8 | `Player_OutOfTime` | 32 frames each, then state $0D |

From idle, the target cell decides the outcome: dirt is dug (sound 0),
space/vacated is walked into, diamonds are collected (`CollectDiamond`), the
exit ends the cave only when `exit_open`, and a boulder starts a push (left
and right only). With **A or B held** the player *snaps*: it digs or collects
the cell next to it without moving, and A/B + Left/Right pushes a boulder
from where it stands. When moving, the old cell becomes `10` (vacated) and the
new one `E0`.

`CollectDiamond` adds `diamond_value` (`bonus_diamond_value` once the exit is
open), counts `diamonds_needed` down in decimal, and at zero opens the exit:
it redraws the exit cell from `ExitPositions` and plays sound 5.

`PlayerHazards` runs every 8 frames: holding **A+B** fills `giveup_counter`
and at $40 switches to phase 6 (same as time running out in `TickTime`); a
firefly or butterfly next to the player calls `KillPlayer`, which explodes the
3×3 area and starts phase 3.

## Enemies

Fireflies (`B`) and butterflies (`C`) keep a heading in the low 2 bits of
their tile. Each scan pass (`Cell_Firefly`, `Cell_Butterfly`):

1. If a boulder or diamond is resting on top: explode
   (`ExplodeToSpace` / `ExplodeToDiamonds`, plus `AwardEnemyScore`).
2. Otherwise, try to turn (firefly: left, butterfly: right), then go
   straight, else turn the other way in place.

The probes (`ProbeUp/Down/Left/Right`) report empty cells and amoeba;
butterflies explode on touching amoeba. Moving left or up sets the processed
bit, since the scan still has to reach those cells. Explosions are
`Explode3x3`; `explosion_timers` time the explosion→diamond cells.

## Amoeba

Unlike the computer original, the NES amoeba grows deterministically.
`AmoebaUpdate` runs every 8 frames: a walker (`amoeba_walker`) follows the
amoeba's edge for 25 steps using `AmoebaStep*`. Each empty or dirt cell it
passes sets "room to grow" (bit 7 of `amoeba_state`) and counts down
`amoeba_grow_countdown`; at zero that cell becomes amoeba and
`amoeba_grow_interval` shrinks by 4, so growth speeds up. The low 2 bits of
`amoeba_state` count walker passes. After 4 passes without room it becomes
0, and `ProcessCell2` turns every amoeba cell into a diamond. There is no
"too big" rule. Caves 6, 12 and 22 play the amoeba hum (`AmoebaSound`).

## Sound driver ($8000-$8949)

Entry points: `Sound_Init`, `Sound_Play` (A = id, 0 stops all) and
`Sound_Update` (from the NMI). There are 35 sound ids (`SoundTable`).
A header holds a channel mask (bit 7 = sound effect), then one stream pointer
per channel. Music and effects have separate request slots (`music_request`,
`sfx_request`); an effect takes over its channels from the music, and
`SoundAllowed` stops an effect from interrupting a higher-priority one.
Effect ids $19 / $1A pause and resume the music.

Timing: `tempo` is added to a 16-bit accumulator every frame, and each carry
is one music tick (`SoundTempoTick`). Effects run every frame.

Music stream (`MusicParse`): a byte below $80 sets the note length, $80-$8F
plays a note (index + `transpose` into `NotePeriodTable`, 96 semitones), and
$90 and up is a command. Effect stream (`SfxParse`): a byte below $90 is a
duration, $90 and up is a command. The 51 commands (`SoundCommandTable`, named
`SndCmd_XX_*`) cover octave/transpose, volume, duty, envelopes and release
modes, jumps, loops, calls, tempo, and for effects direct period/volume/duty
slides and raw APU register writes.

## For the C port

- The game logic is strictly grid-based and tick-based (one object pass per
  8 frames), on top of `cave_grid`.
- Outside the caves, the screens are built straight into VRAM (nametables,
  text streams, CHR ROM reads), so the port keeps the hardware accesses and
  puts a PPU model behind them, rather than drawing from RAM.
- `tools/record.py` records RAM and hardware accesses of any routine on
  `tools/nesemu.py`, which is the reference to diff the port against.

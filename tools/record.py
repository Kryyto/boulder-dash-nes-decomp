#!/usr/bin/env python3
"""Record RAM fixtures for the C port's differential tests.

Runs the game on tools/nesemu.py and, for each call to a watched routine,
saves the registers and the full 2 KB of RAM at entry and at the matching
RTS. State handlers (entered by the main loop's indirect JMP, ending with
JMP MainLoop) are recorded from their entry until MainLoop ($A00B): one
frame of that state. An NMI arriving during a watched call is held back until it returns
(the NMI changes RAM too), which shifts timing slightly in these runs only.

Every hardware access during a call is logged too, in order: PPU and APU
registers, joypads, mapper writes; reads with the value read. The NMI
handler is recorded from its entry to its RTI.

Fixture file (little endian):
  "BDFX" u32 version=3 u32 count
  count x { u16 addr; u8 a,x,y,p,s (entry); u8 ram[2048] (entry);
            u8 a,x,y,p,s (return); u8 ram[2048] (return);
            u32 n; n x { u16 io_addr; u8 value; u8 read } }

Usage: python tools/record.py ROM OUT.bdfx [--per-cave N] [--fuzz | --sound | --screens]
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nesemu  # noqa: E402
import bot  # noqa: E402

WATCH = {0xB789: 'LoadCave', 0xB8AE: 'ApplyCavePatches', 0xC413: 'ScanCave',
         0xC06B: 'PlayerInput', 0xC373: 'PlayerHazards', 0xCFC6: 'TickTime',
         0xD060: 'CopyPlayerInput', 0xCE51: 'AmoebaUpdate', 0xCDAE: 'MagicWallTimer',
         0xC044: 'AmoebaSound', 0xD398: 'PlayerSprite', 0xB197: 'UpdateCamera',
         0xD2AD: 'UpdateStatusSprites', 0x8006: 'Sound_Update', 0x8000: 'Sound_Init'}
STATES = {0xAB5E: 'State0A_CaveIntro', 0xAB76: 'State0B_CavePlay',
          0xACA1: 'State0C_CaveCleared', 0xADF4: 'State0D_LostLife'}
SCREENS = {0xA618: 'State00_BootScreen', 0xA67E: 'State01_Title', 0xA686: 'State02_TitleMenu',
           0xA82D: 'State03_Password', 0xA9E1: 'State04_NextStage', 0xA9E7: 'State05_WorldSelectInit',
           0xA9F2: 'State06_WorldSelect', 0xAA2B: 'State07_StageIntroInit', 0xAA36: 'State08_StageIntro',
           0xAB2E: 'State09_CaveSetup', 0xAEA3: 'State0E_GameOver', 0xEABB: 'State0F_Ending',
           0xEC82: 'State10_EndingScene', 0xED09: 'State11_Credits', 0xEFF5: 'State12_NextLevel'}
STATES.update(SCREENS)
NMI = 0xA2C7  # recorded from its entry to its RTI
INIT_GAME, READ_JOYPADS = 0xA022, 0xA12B
# During a watched frame, an NMI is let through only in WaitForNMI's spin loop
# while nmi_done is still 0, so each wait takes exactly one NMI.
WAIT_FOR_NMI = (0xA0CD, 0xA0CF)
MAIN_LOOP = 0xA00B
WATCH.update(STATES)
WATCH[NMI] = 'NmiHandler'
WATCH[INIT_GAME] = 'InitGame'
WATCH[READ_JOYPADS] = 'ReadJoypads'


def is_io(a):
    return 0x2000 <= a < 0x4018
SAMPLED = (0xAB5E, 0xAB76, 0xC413, 0xC06B, 0xC373, 0xCFC6, 0xD060, 0xCE51, 0xCDAE, 0xC044, 0xD398, 0xB197, 0xD2AD,
           0x8006)  # called every frame: record a sample of calls


FUZZ_TILES = ([0x00] * 6 + [0x10, 0x20, 0x20, 0x30, 0x50, 0x60, 0x70, 0x70, 0x71, 0x71, 0x79,
              0x80, 0x81, 0x89, 0xA0, 0xD0, 0xE0] + list(range(0x90, 0x98)) +
              [0xB0, 0xB1, 0xB2, 0xB3, 0xB8, 0xC0, 0xC1, 0xC2, 0xC3, 0xC8])


def fuzz(ram, rng):
    """Seed a cave state with situations normal play rarely reaches."""
    for _ in range(rng.randint(5, 60)):
        r, c = rng.randint(1, 20), rng.randint(1, 38)
        ram[0x3E0 + r * 40 + c] = rng.choice(FUZZ_TILES)
    if rng.random() < 0.5:  # a falling object right above something interesting
        r, c = rng.randint(2, 19), rng.randint(1, 38)
        ram[0x3E0 + (r - 1) * 40 + c] = rng.choice([0x71, 0x81, 0x70, 0x80])
        ram[0x3E0 + r * 40 + c] = rng.choice([0xB0, 0xB2, 0xC1, 0xC3, 0xE0, 0x60, 0x00])
        ram[0x3E0 + (r + 1) * 40 + c] = rng.choice([0x00, 0x20, 0x50])
    if rng.random() < 0.3:  # a falling boulder onto a magic wall with space below
        r, c = rng.randint(2, 18), rng.randint(1, 38)
        ram[0x3E0 + (r - 1) * 40 + c] = 0x71
        ram[0x3E0 + r * 40 + c] = 0x60
        ram[0x3E0 + (r + 1) * 40 + c] = 0x00
    ram[0xFE] = rng.randrange(256)                                  # frame_phase: any scan phase
    ram[0xB7] = rng.choice([0x00, 0xFF, 0x50, 0x81, 0x02, 0x83])    # amoeba_state
    ram[0xB5] = rng.randrange(4)                                    # magic_wall_state
    ram[0xB6] = rng.choice([0x00, 0x10])
    ram[0xFD] = rng.randrange(256)
    for i in range(8):
        ram[0x7F8 + i] = rng.choice([0, 0, 3, 0x10])                # explosion_timers
    ram[0x8D] = rng.randrange(24)                                   # cave_index (tables)
    ram[0x72] = rng.choice([0x00, 0x00, 0x01, 0x80])                # play_mode: 1P, 2P, demo
    ram[0x750] = rng.choice([0, 0, 0x10, 0x30, 0x36, 0x37])         # redraw_count, incl. full
    ram[0x751] = rng.randrange(0x37)
    for base in (0x78, 0x82):                                       # scores near the next life
        s = rng.randrange(0x10000)
        n = s + rng.randint(-40, 400)
        ram[base:base + 3] = bytes([s & 0xFF, s >> 8 & 0xFF, 0])
        ram[base + 3:base + 6] = bytes([n & 0xFF, n >> 8 & 0xFF, 0]) if n >= 0 else bytes(3)
        ram[base - 1] = rng.choice([3, 8, 9])                       # lives
    ram[0x8A:0x8D] = bytes([rng.randrange(256), rng.randrange(256), rng.choice([0, 0, 1])])
    ram[0x302] = rng.choice([0, 0, 0x81, 0x85, 0x8C])               # sfx_request
    ram[0x301] = rng.choice([0, 0x93, 0x13, 0x8F])                  # music_request


PLAYER_TILES = [0x00, 0x00, 0x10, 0x20, 0x20, 0x30, 0x40, 0x50, 0x70, 0x70, 0x71, 0x80, 0x80,
                0x81, 0x90, 0xA0, 0xB0, 0xC1, 0xD0]


def fuzz_player(ram, rng):
    """Seed player situations: position, phase, buttons, surroundings, counters."""
    r, c = rng.randint(2, 19), rng.randint(2, 37)
    for dr in range(-2, 3):
        for dc in range(-2, 3):
            ram[0x3E0 + (r + dr) * 40 + c + dc] = rng.choice(PLAYER_TILES)
    ram[0x3E0 + r * 40 + c] = 0xE0
    phase = rng.choice([0, 0, 0, 0, 1, 1, 2, 2, 3, 4, 6, 7, 8])
    ram[0x91] = rng.randrange(4) << 4 | phase                       # player_phase
    ram[0x92] = rng.choice([1, 1, 2, 5, 0x18, 0x40])                # player_phase_timer
    y, x = r * 16, c * 16
    if phase == 1:  # mid-step: anywhere along the way to the next cell
        d, k = ram[0x91] >> 4 & 3, rng.randrange(0, 16, 2)
        y += (-k, k, 0, 0)[d]
        x += (0, 0, -k, k)[d]
    if phase == 3:  # dying: the sprite flies anywhere
        y = rng.randrange(0x40, 0x180)
    ram[0x93], ram[0x94], ram[0x95], ram[0x96] = y & 0xFF, y >> 8, x & 0xFF, x >> 8
    ram[0x97], ram[0x98], ram[0x99] = rng.randrange(256), rng.choice([0xFC, 0xFE, 0x00, 0x02]),         rng.choice([0xFF, 0x00])
    ram[0x9A] = rng.choice([0, 0x10, 0x20, 0x40, 0x80, 0x41, 0x81, 0x42, 0x11, 0x21, 0x03, 0x83, 0xC0])
    ram[0x9B] = ram[0x9A] & rng.choice([0, 0xFF])
    ram[0x9C] = rng.choice([0, 1, 0x80])
    ram[0x9D] = rng.choice([0, 0x10, 0x3F, 0x40, 0x41])            # giveup_counter
    ram[0xFE] = rng.choice([0, 8, 16, rng.randrange(256)])          # frame_phase (often & 7 == 0)
    ram[0x1E] = rng.choice([0, 0x40, rng.randrange(256)])           # frame_count (TickTime: & 3F)
    ram[0x1F] = rng.choice([0x0B] * 6 + [0x0C])                     # game_state
    ram[0x72] = rng.choice([0x00, 0x00, 0x01, 0x80])                # play_mode
    ram[0x90] = rng.choice([0, 0, 1])                               # exit_open
    ram[0xAE:0xB1] = bytes(rng.choice([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1], [5, 2, 0]]))
    ram[0xB1:0xB4] = bytes(rng.choice([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1], [4, 3, 0],
                                       [0, 3, 0], [1, 3, 0], [9, 9, 0]]))
    if rng.random() < 0.3:  # set up a snap push: A + Left/Right, boulder next, space beyond
        d = rng.choice([-1, 1])
        ram[0x9A] = 0x01 | (0x80 if d > 0 else 0x40)
        ram[0x91] &= 0xF0
        ram[0xFE] &= 0xF8
        ram[0x3E0 + r * 40 + c + d] = 0x70
        ram[0x3E0 + r * 40 + c + 2 * d] = 0x00
    ram[0xC0], ram[0xC1] = rng.randrange(256), rng.choice([0, 1])  # camera_y
    ram[0xBE], ram[0xBF] = rng.randrange(256), rng.choice([0, 1])  # camera_x
    ram[0x8D] = rng.randrange(24)                                   # cave_index
    ram[0x750] = rng.choice([0, 0, 0x20, 0x36, 0x37])               # redraw_count
    ram[0x751] = rng.randrange(0x37)
    for base in (0x78, 0x82):
        s = rng.randrange(0x10000)
        n = s + rng.randint(-40, 400)
        ram[base:base + 3] = bytes([s & 0xFF, s >> 8 & 0xFF, 0])
        ram[base + 3:base + 6] = bytes([n & 0xFF, n >> 8 & 0xFF, 0]) if n >= 0 else bytes(3)
    ram[0x302] = rng.choice([0, 0, 0x81, 0x85, 0x8C])               # sfx_request
    ram[0x301] = rng.choice([0, 0x93, 0x13, 0x8F])                  # music_request


def fuzz_amoeba(ram, rng):
    """Seed an amoeba blob and walker, and magic wall timing."""
    r, c = rng.randint(4, 17), rng.randint(4, 35)
    for dr in range(-3, 4):
        for dc in range(-3, 4):
            ram[0x3E0 + (r + dr) * 40 + c + dc] = rng.choice([0x00, 0x20, 0x20, 0x30, 0x50, 0x70, 0x80])
    blob = [(r, c)]
    for _ in range(rng.randint(0, 12)):  # random-walk blob
        br, bc = rng.choice(blob)
        dr, dc = rng.choice([(0, 1), (0, -1), (1, 0), (-1, 0)])
        nr, nc = br + dr, bc + dc
        if 2 <= nr <= 19 and 2 <= nc <= 37:
            blob.append((nr, nc))
    for br, bc in blob:
        ram[0x3E0 + br * 40 + bc] = 0xD0
    if rng.random() < 0.3:  # enclosed: nothing to grow into
        for br, bc in blob:
            for dr, dc in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                i = 0x3E0 + (br + dr) * 40 + bc + dc
                if ram[i] != 0xD0:
                    ram[i] = rng.choice([0x30, 0x50, 0x70])
    o = rng.choice(blob)
    w = rng.choice(blob)
    ram[0xB8], ram[0xB9] = o[0] | rng.randrange(4) << 6, o[1]       # amoeba_origin
    ram[0xBB], ram[0xBC] = w[0] | rng.randrange(4) << 6, w[1]       # amoeba_walker
    ram[0xB7] = rng.choice([0x50, 0x00, 0xFF, 0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0x83])
    ram[0xBA] = rng.choice([0xFF, 0xF7, 0x10, 0x04, 0x02])          # grow interval
    ram[0xBD] = rng.choice([1, 1, 2, 5, 0x40, 0xFF])                # grow countdown
    ram[0xB5] = rng.randrange(4)                                    # magic_wall_state
    ram[0xB4] = rng.choice([1, 1, 2, 0, 0x80])                      # magic_wall_timer
    ram[0xFE] = rng.choice([0, 8, 16, 32, rng.randrange(256)])      # frame_phase
    ram[0x8D] = rng.choice([6, 12, 22, 7, 14, 15, rng.randrange(24)])
    ram[0x302] = rng.choice([0, 0, 0x81, 0x06, 0x86])               # sfx_request
    ram[0x301] = rng.choice([0, 0x93, 0x13])
    ram[0x750] = rng.choice([0, 0, 0x30, 0x37])
    ram[0x751] = rng.randrange(0x37)


def fuzz_camera(ram, rng):
    """Camera near every edge, player positions that trigger each step,
    steps in progress, and status digits."""
    cx = rng.choice([0, 0, 0x10, rng.randrange(0, 0x180, 2), 0x180, 0x1F0, 0x200, 0x210])
    cy = rng.choice([0, 0, 0x10, rng.randrange(0, 0x90, 2), 0x8E, 0x90, 0xA0])
    ram[0xBE], ram[0xBF], ram[0xC0], ram[0xC1] = cx & 0xFF, cx >> 8, cy & 0xFF, cy >> 8
    py = max(0, min(0x150, cy + rng.choice([0x00, 0x20, 0x3E, 0x40, 0x60, 0x6E, 0x70, 0x90, -0x10])))
    px = max(0, min(0x260, cx + rng.choice([0x00, 0x08, 0x0E, 0x10, 0x40, 0x5E, 0x60, 0x80, -0x10])))
    ram[0x93], ram[0x94], ram[0x95], ram[0x96] = py & 0xFF, py >> 8, px & 0xFF, px >> 8
    ram[0xC4] = rng.choice([0, 0, 0, 0, 1])                         # frozen after death
    if rng.random() < 0.5:                                          # a step in progress
        ram[0xC5] = rng.choice([0x81, 0x82, 0x84, 0x88, 0x01, 0x02, 0x04, 0x08])
        ram[0xC6] = rng.randrange(8)
        ram[0xC2], ram[0xC3] = rng.randrange(0, 0xA0, 2), rng.randrange(0, 256, 2)
        for i in range(16):
            ram[0x106 + i] = rng.choice([0x00, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90,
                                         0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0x10])
    else:
        ram[0xC5] = ram[0xC6] = 0
    for i in range(64):
        ram[0x136 + i] = rng.randrange(256)                          # attribute shadow
    for i in range(60):
        r, c = rng.randint(0, 21), rng.randint(0, 39)
        ram[0x3E0 + r * 40 + c] = rng.choice([0x00, 0x20, 0x40, 0x50, 0x60, 0x70, 0x80, 0xD0])
    ram[0x90] = rng.choice([0, 1])
    ram[0xB5] = rng.randrange(4)
    ram[0x8D] = rng.randrange(24)
    ram[0xFE] = rng.randrange(256)
    ram[0x72] = rng.choice([0x00, 0x01])
    for base in (0x78, 0x82):
        v = rng.choice([0, 7, 1234, 99999, 123456, 999999, 1000000, rng.randrange(1 << 24)])
        ram[base:base + 3] = bytes([v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF])
    ram[0xAE:0xB1] = bytes(rng.randrange(10) for _ in range(3))
    ram[0xB1:0xB4] = bytes(rng.randrange(10) for _ in range(3))


def fuzz_ending(ram, rng):
    """A frame of the cave-cleared or lost-life state."""
    if ram[0x1F] == 0x0C:
        ram[0x20] = rng.choice([0, 0, 1, 1, 2, 2, 3])                # substate
        ram[0x21] = rng.choice([0, 0x10, 0x16, 0x17, 0x7E, 0x7F, rng.randrange(256)])
        ram[0x91] = (ram[0x91] & 0xF0) | rng.choice([0, 1, 4, 4, 5])  # in the exit?
        ram[0x54] = rng.randrange(3)
        ram[0xB1:0xB4] = bytes(rng.choice([[0, 0, 0], [1, 0, 0], [1, 6, 0], [2, 6, 0], [1, 2, 0],
                                           [0, 2, 0], [9, 9, 1], [rng.randrange(10), rng.randrange(10), 0]]))
        ram[0x76] = rng.choice([0, 1, 7, 0x0E, 0x0B])                 # stages cleared
        ram[0x75] = rng.randrange(6) << 4 | rng.randrange(4)
        ram[0x74] = rng.choice([0, 1, 3, 7])
        ram[0x72] = rng.choice([0x00, 0x00, 0x80])
    else:
        ram[0x20] = rng.choice([0, 1, 1, 2])
        ram[0x21] = rng.choice([0, 0x7E, 0x7F, 0x7F, rng.randrange(256)])  # 0x7F: the fade starts
        ram[0x22] = rng.choice([0, 1, 9, 0x0A])
        ram[0x77] = rng.choice([0, 0, 1, 3, 9])                       # lives
        ram[0x81] = rng.choice([0, 1, 3])
        ram[0x72] = rng.choice([0x00, 0x01, 0x30, 0x31])              # 1P / 2P alternating
        ram[0x74] = rng.choice([0x00, 0x80, 0xC0, 0x40])
        ram[0x7E] = rng.choice([0x00, 0x80, 0xC0])
        ram[0x4D] = rng.choice([2, 5])
    for counter in (0xA0,):
        ram[counter] = rng.choice([0, 0, 0x10])
    ram[0x4C] = rng.choice([0, 0x10, 0x20, 0x01])
    ram[0x750] = rng.choice([0, 3, 0x37])


def fuzz_state(ram, rng):
    """One frame of the cave intro or cave play: substates, the pause and
    time-up boxes, popups, the demo, plus the player situations."""
    if ram[0x1F] == 0x0B:
        fuzz_player(ram, rng)
        ram[0x1F] = 0x0B
        ram[0x70] = rng.choice([0, 0, 0, 1, 1, 2])                   # pause_state
        ram[0x71] = rng.choice([0, 1, 2])
        ram[0x20] = rng.choice([1, 1, 2, 9, 10])                     # substate: box countdown
        for base in (0x5F, 0x61):
            v = rng.randrange(0, 0x280)
            ram[base], ram[base + 1] = v & 0xFF, v >> 8
        if rng.random() < 0.4:  # second half of a camera step: cells are queued
            ram[0xC5] = rng.choice([0x01, 0x02, 0x04, 0x08])
            ram[0xC6] = rng.randrange(4, 8)
    else:  # intro
        ram[0x20] = rng.choice([0, 1, 1, 2, 2, 3])
        if rng.random() < 0.5:  # camera at or next to a leg target
            t = 0xBC92 + ram[0x8D] * 9
            ram[0x751], ram[0x754] = rng.randrange(4), rng.randrange(4)
            for i in range(0x752, 0x757):
                if i != 0x754:
                    ram[i] = rng.randrange(256) if rng.random() < 0.3 else ram[i]
            ram[0xC5] = rng.choice([0, 0, 0x81, 0x02, 0x84, 0x08])
            ram[0xC6] = rng.randrange(8)
    ram[0x29] = rng.choice([0, 0, 0, 0x08])                          # joy1_pressed: Start
    ram[0x72] = rng.choice([0x00, 0x00, 0x01, 0x80, 0x81])           # play_mode, incl. demo
    ram[0x73] = rng.randrange(4)                                     # demo index
    ram[0x9E], ram[0x9F] = rng.randrange(8), rng.choice([0, 0, 1, 3])
    for counter in (0xA0, 0xA3, 0xA6):                               # popups
        ram[counter] = rng.choice([0, 0, 1, 0x10, 0x20])
    for zp in (0xA1, 0xA2, 0xA4, 0xA5, 0xA7, 0xA8, 0xA9, 0xAA):
        ram[zp] = rng.randrange(256) if zp not in (0xA8, 0xAA) else rng.choice([0, 1])
    ram[0x4C] = rng.choice([0, 0, 0x10, 0x20])                       # nmi_requests
    ram[0x750] = rng.choice([0, 0, 3, 6, 0x20, 0x37])                # redraw_count
    ram[0x751], ram[0x752] = rng.randrange(0x37), rng.randrange(0x37)
    for i in range(0x753, 0x753 + 165, 3):
        ram[i] = rng.choice([0x00, 0x20, 0x40, 0x60, 0x70, 0x80, 0xD0])
        ram[i + 1], ram[i + 2] = rng.randrange(22), rng.randrange(40)


FUZZERS = {0xAB5E: fuzz_state, 0xAB76: fuzz_state, 0xACA1: fuzz_ending, 0xADF4: fuzz_ending, 0xC413: fuzz, 0xC06B: fuzz_player, 0xC373: fuzz_player, 0xCFC6: fuzz_player,
           0xD060: fuzz_player, 0xCE51: fuzz_amoeba, 0xCDAE: fuzz_amoeba, 0xC044: fuzz_amoeba,
           0xD398: fuzz_player, 0xB197: fuzz_camera, 0xD2AD: fuzz_camera}


class Recorder(nesemu.NES):
    def __init__(self, rom):
        self.active = None  # before the CPU's reset reads the vectors
        super().__init__(rom)
        self.records = []
        self.limit = {}
        self.enabled = True
        self.fuzz_rng = None
        self.rewind = None
        self.io_log = []

    def regs(self):
        return bytes([self.a, self.x, self.y, self.get_p(0) & 0xEF, self.s])

    def rd(self, a):
        v = super().rd(a)
        if self.active is not None and is_io(a):
            self.io_log.append((a, v, 1))
        return v

    def wr(self, a, v):
        if self.active is not None and (is_io(a) or a >= 0x8000):
            self.io_log.append((a, v, 0))
        super().wr(a, v)

    def nmi(self):
        if self.active and not (self.pc in WAIT_FOR_NMI and self.ram[0x17] == 0):
            # defer the NMI until the watched call returns, so the RAM diff
            # only contains the routine's own writes
            self.nmi_pending = True
            return self.step()
        return super().nmi()

    def step(self):
        pc = self.pc
        if self.enabled and self.active is None and pc in WATCH and self.limit.get(pc, 1) > 0:
            if self.fuzz_rng and pc in FUZZERS:
                # these routines only touch RAM, so after recording the fuzzed
                # call we can rewind CPU + RAM and let the game run it for real
                self.rewind = (pc, self.a, self.x, self.y, self.get_p(0), self.s, bytes(self.ram))
                FUZZERS[pc](self.ram, self.fuzz_rng)
            self.active = (pc, self.s, self.regs(), bytes(self.ram))
            self.io_log = []
        act = self.active
        if act is not None and act[0] in STATES and pc == MAIN_LOOP:
            self.end_record(act)  # one frame of a state handler
            return self.step()
        c = super().step()
        act = self.active
        if act is not None and act[0] not in STATES:
            op = self.prg[pc - 0x8000]
            if (op == 0x60 and self.s == (act[1] + 2) & 0xFF) or \
                    (op == 0x40 and act[0] == NMI and self.s == (act[1] + 3) & 0xFF):
                self.end_record(act)
        return c

    def end_record(self, act):
        self.records.append((act[0], act[2], act[3], self.regs(), bytes(self.ram), self.io_log))
        if act[0] in self.limit:
            self.limit[act[0]] -= 1
        self.active = None
        if self.rewind:
            self.pc, self.a, self.x, self.y, p, self.s, ram = self.rewind
            self.set_p(p)
            self.ram[:] = ram
            self.rewind = None


def write(path, records):
    with open(path, 'wb') as f:
        f.write(b'BDFX' + struct.pack('<II', 3, len(records)))
        for addr, r0, m0, r1, m1, log in records:
            f.write(struct.pack('<H', addr) + r0 + m0 + r1 + m1 + struct.pack('<I', len(log)))
            f.write(b''.join(struct.pack('<HBB', a, v, r) for a, v, r in log))


def record_sounds(nes, out, here, frames=240):
    """Start each sound id from the world map and record every Sound_Update
    call for `frames` frames; then music + effect pairs."""
    nes = nesemu.run_script(nes, open(os.path.join(here, 'scripts', 'tomap.txt')).read().split('\n'))
    records = list(nes.records)
    nes.records = []
    box = bot._strip(nes)
    prg = nes.prg
    word = lambda a: prg[a - 0x8000] | prg[a - 0x7FFF] << 8
    is_sfx = {i: bool(prg[word(0x894E + 2 * i) - 0x8000] & 0x80) for i in range(1, 36)}
    runs = [(i,) for i in range(1, 36)]
    music = [i for i in range(1, 36) if not is_sfx[i]]
    sfx = [i for i in range(1, 36) if is_sfx[i]]
    rng = random.Random(11)
    runs += [(rng.choice(music), rng.choice(sfx)) for _ in range(20)]
    for ids in runs:
        n = nesemu.copy.deepcopy(box)
        n.records = []
        n.limit = {a: 0 for a in WATCH}
        n.limit[0x8006] = frames
        for i in ids:
            n.ram[0x302 if is_sfx[i] else 0x301] = i
        for _ in range(frames + 2):
            n.run_frame()
        records += n.records
        print('sound %s: %d records' % (ids, len(n.records)), flush=True)
    write(out, records)
    print('wrote %s: %d records' % (out, len(records)))


A, B, SELECT, START, UP, DOWN, LEFT, RIGHT = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80


class Screens:
    """Drive the game through every screen, recording each frame of the
    screen states and a sample of NMIs."""

    def __init__(self, nes, rng):
        self.n = nes
        self.rng = rng

    def frame(self, joy=0):
        n = self.n
        if n.limit.get(NMI, 0) == 0 and self.rng.random() < 0.12:
            n.limit[NMI] = 1
        if n.limit.get(READ_JOYPADS, 0) == 0 and self.rng.random() < 0.03:
            n.limit[READ_JOYPADS] = 1
        # the current player's pad: player 2 plays on pad 2
        if n.ram[0x72] & 0x01 and n.ram[0x1F] != 0x01:
            n.joy, n.joy2 = 0, joy
        else:
            n.joy, n.joy2 = joy, 0
        n.run_frame()

    def frames(self, k, joy=0):
        for _ in range(k):
            self.frame(joy)

    def tap(self, joy, gap=10):
        self.frames(4, joy)
        self.frames(gap)

    def until(self, cond, limit=3000, joy=0):
        for _ in range(limit):
            if cond():
                return True
            self.frame(joy)
        print('  timed out waiting (state $%02X/%d)' % (self.state(), self.n.ram[0x20]), flush=True)
        return False

    def state(self):
        return self.n.ram[0x1F]

    def at(self, state, sub=None):
        # (a record may be in progress at a frame boundary: poke() waits for it to end)
        return lambda: self.state() == state and (sub is None or self.n.ram[0x20] == sub)

    def poke(self, addr, *values):
        while self.n.active is not None:  # never in the middle of a record
            self.frame()
        for i, v in enumerate(values):
            self.n.ram[(addr + i) & 0x7FF] = v

    def goto(self, state):
        self.poke(0x1F, state, 0, 0, 0)

    def password(self, loop, world):
        """In the password screen: type a few digits, then the password of
        (loop, world) from the game's table, and confirm."""
        self.until(self.at(0x03, 1))
        for b in (RIGHT, UP, UP, LEFT, DOWN, LEFT, LEFT, DOWN, RIGHT):
            self.tap(b, 6)
        prg = self.n.prg
        t = 0xA921 + (loop * 6 + world) * 8 - 0x8000
        self.poke(0x4D, *prg[t:t + 6])
        self.tap(A)

    def lose_life(self, lives):
        """In a cave: set the current player's lives, then give up (A+B held)
        until the next screen."""
        self.until(self.at(0x0B), 600)
        self.poke(0x77 + (0x0A if self.n.ram[0x72] & 1 else 0), lives)
        self.until(lambda: self.state() not in (0x0A, 0x0B, 0x0D) and self.n.active is None, 2000, A | B)

    def into_cave(self):
        """From the world map or a stage intro: into a cave."""
        if self.state() == 0x06:
            self.pick_stage()
        self.until(self.at(0x08, 2), 600)
        self.tap(A)
        self.until(self.at(0x0B), 600)

    def pick_stage(self):
        """On the world map: move the cursor about and press A until a stage starts."""
        if self.rng.random() < 0.5:  # some stages cleared: picking those does nothing
            self.until(self.at(0x06, 1), 300)
            x = 0x0A if self.n.ram[0x72] & 1 else 0
            self.poke(0x76 + x, self.rng.choice([0x05, 0x0A, 0x0E, 0x07]))
        for _ in range(60):
            if self.state() != 0x06:
                return
            self.tap(self.rng.choice([UP, DOWN, LEFT, RIGHT]), 6)
            self.tap(A, 6)

    def play_some(self, k):
        n = self.n
        for _ in range(k):
            g = bot.grid(n)
            me = bot.find_player(g)
            if me and self.state() == 0x0B:
                bot.step(n, self.rng.choice(list(bot.DIRS)), me)
            else:
                self.frame()


def record_screens(nes, out):
    rng = random.Random(5)
    nes.limit = {a: (100000 if a in SCREENS else 0) for a in WATCH}
    s = Screens(nes, rng)
    n = nes
    say = lambda m: print('frame %6d: %s (%d records)' % (n.frame, m, len(n.records)), flush=True)

    say('cold boot: boot screen, title, the attract demo')
    s.until(s.at(0x01, 3), 2000)
    s.tap(SELECT)
    s.tap(DOWN)
    s.until(s.at(0x09), 3000)          # idle: the demo starts
    s.until(s.at(0x0A), 300)
    s.frames(30)
    s.tap(START)                        # back to the title
    s.until(s.at(0x01, 3), 2000)
    s.until(s.at(0x09), 3000)          # the next two demos
    s.until(s.at(0x0B), 600)
    s.play_some(60)
    s.goto(0x01)
    s.until(s.at(0x01, 3), 2000)
    s.until(s.at(0x09), 3000)
    s.until(s.at(0x0A), 300)
    s.goto(0x01)

    say('1 player: title menu, COLOR SELECT')
    s.until(s.at(0x01, 3), 2000)
    s.tap(START)
    s.until(s.at(0x02, 3))
    s.frames(40, RIGHT)
    s.frames(3)
    s.frames(25, LEFT)
    s.tap(RIGHT)
    s.tap(A)

    for world in range(6):
        say('password of world %d, its scene' % (world + 1))
        s.password(0, world)
        s.until(s.at(0x04, 1))
        s.until(s.at(0x06), 2500)      # the scene ends by itself, then scrolls to the map
        if world < 5:
            s.goto(0x03)

    say('world map, stage intros, cave setups')
    s.until(s.at(0x06, 1))
    s.pick_stage()
    for cave in range(24):
        s.until(s.at(0x08, 2), 600)
        s.poke(0x75, (cave // 4) << 4 | cave % 4)
        s.poke(0x74, rng.choice([0x80, 0x82]))
        s.tap(A)
        s.until(s.at(0x0A), 300)
        if cave in (0, 5, 13, 22):     # some play, with NMIs during the camera's scrolling
            s.until(s.at(0x0B), 600)
            s.play_some(150)
        s.goto(0x07 if cave % 2 else 0x05)
        if cave % 2 == 0:
            s.until(s.at(0x06, 1), 300)
            s.pick_stage()

    say('game over: CONTINUE')
    s.into_cave()
    s.lose_life(0)
    s.until(s.at(0x0E, 3))
    s.tap(DOWN)
    s.tap(UP)
    s.tap(DOWN)
    s.tap(DOWN)
    s.tap(A)
    s.until(s.at(0x06), 600)

    say('game over: END')
    s.into_cave()
    s.lose_life(0)
    s.until(s.at(0x0E, 3))
    s.tap(DOWN)
    s.tap(A)
    s.until(s.at(0x01), 600)

    say('2 players: turns, CONTINUE, then one ends and the other goes on')
    s.until(s.at(0x01, 3), 2000)
    s.tap(SELECT)
    s.tap(START)
    s.until(s.at(0x02, 3))
    s.tap(A)
    s.until(s.at(0x03, 1))
    s.tap(A)
    s.until(s.at(0x06, 1), 3000, A)
    s.into_cave()
    s.lose_life(2)                      # player 1 loses a life: player 2's turn
    s.until(s.at(0x02, 3), 600)        # PLAYER 2 on the title menu
    s.tap(A)
    s.until(s.at(0x03, 1))
    s.tap(A)
    s.until(s.at(0x06, 1), 3000, A)
    s.into_cave()
    s.lose_life(0)                      # player 2: game over, CONTINUE
    s.until(s.at(0x0E, 3))
    s.tap(A)
    s.until(s.at(0x06, 1), 600)        # player 1's turn again
    s.into_cave()
    s.lose_life(0)                      # player 1: game over, END
    s.until(s.at(0x0E, 3))
    s.tap(DOWN)
    s.tap(A)
    s.until(lambda: s.state() not in (0x0E,), 800)
    s.until(lambda: s.state() in (0x06, 0x08) and n.active is None, 3000, A)

    for loop in range(4):
        two = loop == 3
        say('the ending, from loop %d%s' % (loop, ', 2 players' if two else ''))
        s.until(lambda: s.state() in (0x06, 0x08) and n.active is None, 3000)
        s.poke(0x72, 0x30 if two else 0x00)
        s.poke(0x74, 0x80 | loop, 0x53, 0x07)
        s.poke(0x7E, 0x80, 0x00, 0x00, 0x03)
        s.goto(0x09)
        s.until(s.at(0x0B), 600)
        n2, _ = bot.cheat_exit(n)
        s.until(lambda: s.state() == 0x0F, 3000)
        s.until(s.at(0x0F, 2), 3000)
        s.frames(140)
        s.tap(A)
        if loop < 3:
            s.until(s.at(0x12, 2), 4000)
            s.frames(60)
            s.tap(START)
            s.until(s.at(0x06, 1), 600)
        else:
            s.until(s.at(0x11, 6), 20000)
            s.frames(200)
            s.tap(A)                    # player 2's turn
            s.until(lambda: s.state() in (0x02, 0x05), 600)
            s.frames(120)

    say('reset with the RAM kept (warm boot), then power off and on')
    for _ in range(2):
        while n.active is not None:
            s.frame()
        n.limit[INIT_GAME] = 1
        n.pc = n.rd16(0xFFFC)
        n.s, n.i, n.nmi_pending = 0xFD, 1, False
        n.ppu.ctrl = n.ppu.mask = 0
        s.until(s.at(0x01, 3), 2000)
        n.ram[:] = bytes(0x800)
    write(out, n.records)
    counts = {}
    for r in n.records:
        counts[WATCH[r[0]]] = counts.get(WATCH[r[0]], 0) + 1
    print('wrote %s: %s' % (out, counts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('out')
    ap.add_argument('--per-cave', type=int, default=48, help='calls kept per every-frame routine, cave and loop')
    ap.add_argument('--frames', type=int, default=900, help='frames played per cave and loop')
    ap.add_argument('--caves', default='0-23')
    ap.add_argument('--fuzz', action='store_true',
                    help='seed random situations into RAM before each recorded ScanCave / player call')
    ap.add_argument('--sound', action='store_true',
                    help='record Sound_Update for every sound id (and music + effect pairs)')
    ap.add_argument('--screens', action='store_true',
                    help='record the screens outside the caves: boot, title, demo, menus, '
                         'map, passwords, scenes, game over, endings, and NMIs')
    args = ap.parse_args()
    lo, hi = (int(x) for x in (args.caves.split('-') + args.caves.split('-'))[:2])

    nes = Recorder(open(args.rom, 'rb').read())
    if args.screens:
        record_screens(nes, args.out)
        return
    nes.limit = {a: 0 for a in WATCH}
    nes.limit[0x8000] = 1  # Sound_Init, once at boot
    here = os.path.dirname(os.path.abspath(__file__))
    if args.sound:
        record_sounds(nes, args.out, here)
        return
    boot_records = []
    nes = nesemu.run_script(nes, open(os.path.join(here, 'scripts', 'tomap.txt')).read().split('\n'))
    boot_records = nes.records
    nes.records = []
    nes.enabled = False
    box = bot._strip(nes)
    rng = random.Random(7)
    records = list(boot_records)
    for cave in range(lo, hi + 1):
        for loop in (0, 2):
            n = nesemu.copy.deepcopy(box)
            n.records = []
            n.enabled = True
            n.fuzz_rng = random.Random(cave * 4 + loop) if args.fuzz else None
            n.ram[0x75] = (cave // 4) << 4 | cave % 4
            n.ram[0x74] = loop
            # spread samples of the every-frame routines over the cave
            n.limit = {a: (1 if a in (0xB789, 0xB8AE) else 0) for a in WATCH}
            nesemu.run_script(n, ['tap a'])
            start = n.frame
            while n.ram[0x1F] != 0x0B and n.frame < start + 2000:
                if n.ram[0x1F] == 0x0A and n.limit[0xAB5E] == 0 and rng.random() < 0.3:
                    n.limit[0xAB5E] = 1  # a frame of the intro pan
                n.run_frame()
            kept = {a: 0 for a in SAMPLED}
            while n.frame < start + args.frames and (n.ram[0x1F] in (0x0B, 0x0D) or n.active):
                for a in SAMPLED:
                    if kept[a] < args.per_cave and n.limit[a] == 0 and rng.random() < 0.6:
                        n.limit[a] = 1  # record the next call
                        kept[a] += 1
                if n.ram[0x1F] == 0x0D:
                    n.limit[0xADF4] = 1  # every frame of a lost life
                g = bot.grid(n)
                me = bot.find_player(g)
                if me and n.ram[0x1F] == 0x0B:
                    move = bot.choose(n, g, me, rng, False) if rng.random() < 0.8 else rng.choice(list(bot.DIRS))
                    bot.step(n, move, me)
                else:
                    n.joy = 0
                    n.run_frame()
            while n.active:  # let a record in progress finish before poking RAM
                n.run_frame()
            # then end the cave and record every frame of the ending, up to the
            # next state: loop 0 clears it (exit cheat, bonus count), loop 2
            # gives up (A+B held: the lost-life sequence)
            if loop == 2 and n.ram[0x1F] in (0x0B, 0x0D):
                n.limit[0xADF4] = 1000  # (a death already under way is recorded to its end)
                n.joy = 0x03
                for _ in range(1500):
                    if n.ram[0x1F] not in (0x0B, 0x0D) and n.active is None:
                        break
                    n.run_frame()
                n.joy = 0
                n.limit[0xADF4] = 0
            elif n.ram[0x1F] == 0x0B and bot.find_player(bot.grid(n)):
                n.limit[0xACA1] = 1000
                n, _ = bot.cheat_exit(n)
                for _ in range(1200):
                    if n.ram[0x1F] != 0x0C and n.active is None:
                        break
                    n.run_frame()
                n.limit[0xACA1] = 0
            records += n.records
            print('cave %2d loop %d: %d records' % (cave, loop, len(n.records)), flush=True)
    write(args.out, records)
    counts = {}
    for r in records:
        counts[WATCH[r[0]]] = counts.get(WATCH[r[0]], 0) + 1
    print('wrote %s: %s' % (args.out, counts))


if __name__ == '__main__':
    main()

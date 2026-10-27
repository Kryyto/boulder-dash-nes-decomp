#!/usr/bin/env python3
"""Minimal headless NES emulator for research on Boulder Dash.

Good enough to run the game, not cycle-exact: official 6502 opcodes with
page-cross timing, MMC1, a PPU that models registers, VRAM, vblank/NMI and an
approximate sprite-0 hit (no per-pixel rendering), joypad 1, APU writes
ignored.

It records a Code/Data Log of PRG ROM (Mesen2 layout: 'CDLv2' + crc32 + one
flag byte per PRG byte; 0x01 code, 0x02 data read, 0x04 executed opcode) that
tools/disasm.py reads with --cdl, and can save screenshots as PNG.

Usage: python tools/nesemu.py ROM SCRIPT [--cdl OUT] [--shots DIR]

Script commands (one per line, '#' comments):
  wait N                hold nothing for N frames
  hold BTN[+BTN] N      hold buttons for N frames (a b select start up down left right)
  tap BTN[+BTN]         press for 4 frames, release for 8
  random N [SEED]       random joypad input for N frames
  shot NAME             save screenshot NAME.png (needs --shots)
  poke ADDR VAL         write a byte of CPU RAM (hex)
  peek ADDR [COUNT]     print RAM bytes (hex)
  save NAME / load NAME snapshot / restore the machine state (coverage is kept)
  repeat N ... end      repeat a block
  print TEXT            echo
"""
import argparse
import copy
import os
import random as _random
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import OPS, MODES  # noqa: E402

BUTTONS = {'a': 0x01, 'b': 0x02, 'select': 0x04, 'start': 0x08,
           'up': 0x10, 'down': 0x20, 'left': 0x40, 'right': 0x80}

DOTS_PER_LINE = 341
LINES = 262
VBLANK_DOT = 241 * DOTS_PER_LINE + 1
FRAME_DOTS = LINES * DOTS_PER_LINE

RMW = {'asl', 'lsr', 'rol', 'ror', 'inc', 'dec'}
STORES = {'sta', 'stx', 'sty'}


def base_cycles(mn, mode):
    if mn in ('jsr', 'rts', 'rti'):
        return 6
    if mn == 'brk':
        return 7
    if mn == 'jmp':
        return 3 if mode == 'abs' else 5
    if mn in ('pha', 'php'):
        return 3
    if mn in ('pla', 'plp'):
        return 4
    if mn in RMW and mode not in ('acc',):
        return {'zp': 5, 'zpx': 6, 'abs': 6, 'absx': 7}[mode]
    c = {'imp': 2, 'acc': 2, 'imm': 2, 'rel': 2, 'zp': 3, 'zpx': 4, 'zpy': 4,
         'abs': 4, 'absx': 4, 'absy': 4, 'indx': 6, 'indy': 5}[mode]
    if mn in STORES and mode in ('absx', 'absy', 'indy'):
        c += 1
    return c


# (mnemonic, mode, size, cycles, page-cross penalty applies)
DECODE = [None] * 256
for _op, (_mn, _mode) in OPS.items():
    DECODE[_op] = (_mn, _mode, 1 + MODES[_mode], base_cycles(_mn, _mode),
                   _mode in ('absx', 'absy', 'indy') and _mn not in STORES and _mn not in RMW)


class Mapper:
    """MMC1 with fixed 32 KB PRG."""

    def __init__(self):
        self.shift = 0x10
        self.ctrl = 0x0C
        self.chr0 = 0
        self.chr1 = 0
        self.prg = 0

    def write(self, addr, v):
        if v & 0x80:
            self.shift = 0x10
            self.ctrl |= 0x0C
            return
        done = self.shift & 1
        self.shift = (self.shift >> 1) | ((v & 1) << 4)
        if done:
            r = self.shift
            self.shift = 0x10
            if addr < 0xA000:
                self.ctrl = r
            elif addr < 0xC000:
                self.chr0 = r
            elif addr < 0xE000:
                self.chr1 = r
            else:
                self.prg = r

    def chr_offset(self, a):
        if self.ctrl & 0x10:
            bank = self.chr0 if a < 0x1000 else self.chr1
            return (bank * 0x1000 + (a & 0xFFF))
        return ((self.chr0 & 0x1E) * 0x1000 + a)

    def mirror(self, a):
        """Nametable address $2000-$2FFF -> offset in 2 KB CIRAM."""
        a &= 0xFFF
        m = self.ctrl & 3
        if m == 0:
            return a & 0x3FF
        if m == 1:
            return 0x400 | (a & 0x3FF)
        if m == 2:  # vertical
            return a & 0x7FF
        return ((a >> 1) & 0x400) | (a & 0x3FF)  # horizontal


class PPU:
    def __init__(self, nes):
        self.nes = nes
        self.ctrl = 0
        self.mask = 0
        self.status = 0
        self.oam_addr = 0
        self.oam = bytearray(256)
        self.ciram = bytearray(0x800)
        self.pal = bytearray(32)
        self.v = 0
        self.t = 0
        self.x = 0
        self.w = 0
        self.buf = 0
        self.frame_ctrl = 0
        self.frame_t = 0
        self.frame_x = 0

    def vram_read(self, a):
        a &= 0x3FFF
        if a < 0x2000:
            return self.nes.chr[self.nes.mapper.chr_offset(a) % len(self.nes.chr)]
        if a < 0x3F00:
            return self.ciram[self.nes.mapper.mirror(a)]
        return self.pal[self.pal_index(a)]

    def vram_write(self, a, v):
        a &= 0x3FFF
        if a < 0x2000:
            return  # CHR ROM
        if a < 0x3F00:
            self.ciram[self.nes.mapper.mirror(a)] = v
        else:
            self.pal[self.pal_index(a)] = v & 0x3F

    @staticmethod
    def pal_index(a):
        i = a & 0x1F
        if i in (0x10, 0x14, 0x18, 0x1C):
            i -= 0x10
        return i

    def read(self, r):
        if r == 2:
            v = self.status
            self.status &= 0x7F
            self.w = 0
            return v
        if r == 4:
            return self.oam[self.oam_addr]
        if r == 7:
            a = self.v & 0x3FFF
            if a < 0x3F00:
                v = self.buf
                self.buf = self.vram_read(a)
            else:
                v = self.vram_read(a)
                self.buf = self.vram_read(a - 0x1000)
            self.v = (self.v + (32 if self.ctrl & 4 else 1)) & 0x7FFF
            return v
        return 0

    def write(self, r, v):
        if r == 0:
            was = self.ctrl & 0x80
            self.ctrl = v
            self.t = (self.t & 0xF3FF) | ((v & 3) << 10)
            if not was and v & 0x80 and self.status & 0x80:
                self.nes.nmi_pending = True
        elif r == 1:
            self.mask = v
        elif r == 3:
            self.oam_addr = v
        elif r == 4:
            self.oam[self.oam_addr] = v
            self.oam_addr = (self.oam_addr + 1) & 0xFF
        elif r == 5:
            if not self.w:
                self.t = (self.t & 0xFFE0) | (v >> 3)
                self.x = v & 7
            else:
                self.t = (self.t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2)
            self.w ^= 1
        elif r == 6:
            if not self.w:
                self.t = (self.t & 0x00FF) | ((v & 0x3F) << 8)
            else:
                self.t = (self.t & 0xFF00) | v
                self.v = self.t
            self.w ^= 1
        elif r == 7:
            self.vram_write(self.v, v)
            self.v = (self.v + (32 if self.ctrl & 4 else 1)) & 0x7FFF


class NES:
    def __init__(self, rom):
        assert rom[:4] == b'NES\x1a'
        prg_size = rom[4] * 0x4000
        chr_size = rom[5] * 0x2000
        self.prg = rom[16:16 + prg_size]
        self.chr = rom[16 + prg_size:16 + prg_size + chr_size]
        self.prg_crc = zlib.crc32(self.prg)
        self.cdl = bytearray(prg_size)
        self.ram = bytearray(0x800)
        self.mapper = Mapper()
        self.ppu = PPU(self)
        self.joy = 0          # buttons currently held
        self.joy2 = 0         # pad 2
        self.joy_shift = 0
        self.joy2_shift = 0
        self.joy_strobe = 0
        self.nmi_pending = False
        self.cycles = 0
        self.dot = 0          # PPU dot within the frame
        self.frame = 0
        self.a = self.x = self.y = 0
        self.s = 0xFD
        self.c = self.z = self.v = self.n = self.d = 0
        self.i = 1
        self.pc = self.rd16(0xFFFC)
        self.sprite0_dot = None

    # --- bus ---
    def rd(self, a):
        if a < 0x2000:
            return self.ram[a & 0x7FF]
        if a >= 0x8000:
            self.cdl[a - 0x8000] |= 0x02
            return self.prg[a - 0x8000]
        if a < 0x4000:
            return self.ppu.read(a & 7)
        if a == 0x4016:
            if self.joy_strobe:
                return 0x40 | (self.joy & 1)
            v = self.joy_shift & 1
            self.joy_shift = (self.joy_shift >> 1) | 0x80
            return 0x40 | v
        if a == 0x4017:
            if self.joy_strobe:
                return 0x40 | (self.joy2 & 1)
            v = self.joy2_shift & 1
            self.joy2_shift = (self.joy2_shift >> 1) | 0x80
            return 0x40 | v
        return 0

    def wr(self, a, v):
        if a < 0x2000:
            self.ram[a & 0x7FF] = v
        elif a < 0x4000:
            self.ppu.write(a & 7, v)
        elif a == 0x4014:
            base = v << 8
            oam = self.ppu.oam
            for i in range(256):
                oam[(self.ppu.oam_addr + i) & 0xFF] = self.rd(base + i)
            self.cycles += 513
            self.dot += 513 * 3
        elif a == 0x4016:
            self.joy_strobe = v & 1
            self.joy_shift = self.joy
            self.joy2_shift = self.joy2
        elif a >= 0x8000:
            self.mapper.write(a, v)

    def rd16(self, a):
        return self.rd(a) | self.rd((a + 1) & 0xFFFF) << 8

    def push(self, v):
        self.ram[0x100 | self.s] = v
        self.s = (self.s - 1) & 0xFF

    def pull(self):
        self.s = (self.s + 1) & 0xFF
        return self.ram[0x100 | self.s]

    def get_p(self, b):
        return (self.c | self.z << 1 | self.i << 2 | self.d << 3 | b << 4 | 0x20 |
                self.v << 6 | self.n << 7)

    def set_p(self, p):
        self.c = p & 1; self.z = p >> 1 & 1; self.i = p >> 2 & 1; self.d = p >> 3 & 1
        self.v = p >> 6 & 1; self.n = p >> 7 & 1

    def nz(self, v):
        self.z = 1 if v == 0 else 0
        self.n = v >> 7
        return v

    # --- CPU ---
    def nmi(self):
        self.push(self.pc >> 8)
        self.push(self.pc & 0xFF)
        self.push(self.get_p(0))
        self.i = 1
        self.pc = self.rd16(0xFFFA)
        return 7

    def step(self):
        pc = self.pc
        if pc < 0x8000:
            raise RuntimeError('executing outside ROM at $%04X' % pc)
        prg = self.prg
        op = prg[pc - 0x8000]
        dec = DECODE[op]
        if dec is None:
            raise RuntimeError('invalid opcode $%02X at $%04X' % (op, pc))
        mn, mode, size, cyc, pen = dec
        o = pc - 0x8000
        cdl = self.cdl
        cdl[o] |= 0x05
        if size > 1:
            cdl[o + 1] |= 0x01
            b1 = prg[o + 1]
            if size > 2:
                cdl[o + 2] |= 0x01
                b2 = prg[o + 2]
        self.pc = (pc + size) & 0xFFFF

        # effective address
        addr = None
        if mode == 'imm':
            addr = -1  # value in b1
        elif mode == 'zp':
            addr = b1
        elif mode == 'zpx':
            addr = (b1 + self.x) & 0xFF
        elif mode == 'zpy':
            addr = (b1 + self.y) & 0xFF
        elif mode == 'abs':
            addr = b1 | b2 << 8
        elif mode == 'absx':
            base = b1 | b2 << 8
            addr = (base + self.x) & 0xFFFF
            if pen and (base ^ addr) & 0xFF00:
                cyc += 1
        elif mode == 'absy':
            base = b1 | b2 << 8
            addr = (base + self.y) & 0xFFFF
            if pen and (base ^ addr) & 0xFF00:
                cyc += 1
        elif mode == 'indx':
            p = (b1 + self.x) & 0xFF
            addr = self.ram[p] | self.ram[(p + 1) & 0xFF] << 8
        elif mode == 'indy':
            base = self.ram[b1] | self.ram[(b1 + 1) & 0xFF] << 8
            addr = (base + self.y) & 0xFFFF
            if pen and (base ^ addr) & 0xFF00:
                cyc += 1
        elif mode == 'ind':
            p = b1 | b2 << 8
            addr = self.rd(p) | self.rd((p & 0xFF00) | ((p + 1) & 0xFF)) << 8
        elif mode == 'rel':
            addr = (self.pc + (b1 - 256 if b1 & 0x80 else b1)) & 0xFFFF

        def val():
            return b1 if addr == -1 else self.rd(addr)

        if mn == 'lda':
            self.a = self.nz(val())
        elif mn == 'ldx':
            self.x = self.nz(val())
        elif mn == 'ldy':
            self.y = self.nz(val())
        elif mn == 'sta':
            self.wr(addr, self.a)
        elif mn == 'stx':
            self.wr(addr, self.x)
        elif mn == 'sty':
            self.wr(addr, self.y)
        elif mode == 'rel':
            f = {'bcc': not self.c, 'bcs': self.c, 'beq': self.z, 'bne': not self.z,
                 'bmi': self.n, 'bpl': not self.n, 'bvc': not self.v, 'bvs': self.v}[mn]
            if f:
                cyc += 1 + (1 if (self.pc ^ addr) & 0xFF00 else 0)
                self.pc = addr
        elif mn in ('adc', 'sbc'):
            m = val()
            if mn == 'sbc':
                m ^= 0xFF
            r = self.a + m + self.c
            self.c = 1 if r > 0xFF else 0
            r &= 0xFF
            self.v = 1 if (~(self.a ^ m) & (self.a ^ r) & 0x80) else 0
            self.a = self.nz(r)
        elif mn == 'and':
            self.a = self.nz(self.a & val())
        elif mn == 'ora':
            self.a = self.nz(self.a | val())
        elif mn == 'eor':
            self.a = self.nz(self.a ^ val())
        elif mn in ('cmp', 'cpx', 'cpy'):
            r = {'cmp': self.a, 'cpx': self.x, 'cpy': self.y}[mn]
            m = val()
            self.c = 1 if r >= m else 0
            self.nz((r - m) & 0xFF)
        elif mn == 'bit':
            m = val()
            self.z = 1 if (self.a & m) == 0 else 0
            self.n = m >> 7
            self.v = m >> 6 & 1
        elif mn in RMW:
            m = self.a if mode == 'acc' else self.rd(addr)
            if mn == 'asl':
                self.c = m >> 7; r = (m << 1) & 0xFF
            elif mn == 'lsr':
                self.c = m & 1; r = m >> 1
            elif mn == 'rol':
                r = ((m << 1) | self.c) & 0xFF; self.c = m >> 7
            elif mn == 'ror':
                r = (m >> 1) | (self.c << 7); self.c = m & 1
            elif mn == 'inc':
                r = (m + 1) & 0xFF
            else:
                r = (m - 1) & 0xFF
            self.nz(r)
            if mode == 'acc':
                self.a = r
            else:
                if addr >= 0x8000:
                    self.wr(addr, m)  # RMW dummy write (resets MMC1 on INC $FF9E)
                self.wr(addr, r)
        elif mn == 'jmp':
            self.pc = addr
        elif mn == 'jsr':
            ret = (self.pc - 1) & 0xFFFF
            self.push(ret >> 8)
            self.push(ret & 0xFF)
            self.pc = addr
        elif mn == 'rts':
            lo = self.pull()
            self.pc = ((self.pull() << 8 | lo) + 1) & 0xFFFF
        elif mn == 'rti':
            self.set_p(self.pull())
            lo = self.pull()
            self.pc = self.pull() << 8 | lo
        elif mn == 'pha':
            self.push(self.a)
        elif mn == 'php':
            self.push(self.get_p(1))
        elif mn == 'pla':
            self.a = self.nz(self.pull())
        elif mn == 'plp':
            self.set_p(self.pull())
        elif mn == 'tax':
            self.x = self.nz(self.a)
        elif mn == 'tay':
            self.y = self.nz(self.a)
        elif mn == 'txa':
            self.a = self.nz(self.x)
        elif mn == 'tya':
            self.a = self.nz(self.y)
        elif mn == 'tsx':
            self.x = self.nz(self.s)
        elif mn == 'txs':
            self.s = self.x
        elif mn == 'inx':
            self.x = self.nz((self.x + 1) & 0xFF)
        elif mn == 'iny':
            self.y = self.nz((self.y + 1) & 0xFF)
        elif mn == 'dex':
            self.x = self.nz((self.x - 1) & 0xFF)
        elif mn == 'dey':
            self.y = self.nz((self.y - 1) & 0xFF)
        elif mn == 'clc':
            self.c = 0
        elif mn == 'sec':
            self.c = 1
        elif mn == 'cli':
            self.i = 0
        elif mn == 'sei':
            self.i = 1
        elif mn == 'cld':
            self.d = 0
        elif mn == 'sed':
            self.d = 1
        elif mn == 'clv':
            self.v = 0
        elif mn == 'nop':
            pass
        elif mn == 'brk':
            raise RuntimeError('BRK at $%04X' % pc)
        return cyc

    # --- frame loop ---
    def run_frame(self):
        """Run from the start of scanline 0 to the end of the pre-render line."""
        ppu = self.ppu
        # sprite 0 hit: approximate as the line below sprite 0's top when rendering is on
        s0y = ppu.oam[0]
        self.sprite0_dot = (s0y + 2) * DOTS_PER_LINE + ppu.oam[3] if s0y < 239 else None
        vblank_done = False
        while self.dot < FRAME_DOTS:
            if self.nmi_pending:
                self.nmi_pending = False
                c = self.nmi()
            else:
                c = self.step()
            self.cycles += c
            self.dot += c * 3
            if self.sprite0_dot is not None and self.dot >= self.sprite0_dot:
                if ppu.mask & 0x18 == 0x18:
                    ppu.status |= 0x40
                self.sprite0_dot = None
            if not vblank_done and self.dot >= VBLANK_DOT:
                vblank_done = True
                ppu.status |= 0x80
                ppu.frame_ctrl, ppu.frame_t, ppu.frame_x = ppu.ctrl, ppu.t, ppu.x
                self.snapshot = (bytes(ppu.ciram), bytes(ppu.pal), bytes(ppu.oam),
                                 ppu.ctrl, ppu.mask, ppu.t, ppu.x,
                                 self.mapper.chr0, self.mapper.chr1, self.mapper.ctrl)
                if ppu.ctrl & 0x80:
                    self.nmi_pending = True
        self.dot -= FRAME_DOTS
        ppu.status &= 0x1F  # pre-render line clears vblank, sprite 0, overflow
        self.frame += 1

    # --- output ---
    def write_cdl(self, path):
        with open(path, 'wb') as f:
            f.write(b'CDLv2' + struct.pack('<I', self.prg_crc) + bytes(self.cdl) +
                    bytes(len(self.chr)))

    def screenshot(self, path):
        render_png(self, path)


# FCEUX-style 2C02 palette
PALETTE = [int(x, 16) for x in (
    '7C7C7C 0000FC 0000BC 4428BC 940084 A80020 A81000 881400 503000 007800 006800 005800 004058 000000 000000 000000 '
    'BCBCBC 0078F8 0058F8 6844FC D800CC E40058 F83800 E45C10 AC7C00 00B800 00A800 00A844 008888 000000 000000 000000 '
    'F8F8F8 3CBCFC 6888FC 9878F8 F878F8 F85898 F87858 FCA044 F8B800 B8F818 58D854 58F898 00E8D8 787878 000000 000000 '
    'FCFCFC A4E4FC B8B8F8 D8B8F8 F8B8F8 F8A4C0 F0D0B0 FCE0A8 F8D878 D8F878 B8F8B8 B8F8D8 00FCFC F8D8F8 000000 000000'
).split()]


def render_png(nes, path):
    """Render all four nametables (512x480) with sprites at the frame's scroll
    position, plus a box showing the 256x240 viewport. Taken at vblank."""
    snap = getattr(nes, 'snapshot', None)
    if snap is None:
        return
    ciram, pal, oam, ctrl, mask, t, fx, chr0, chr1, mctrl = snap
    m = copy.copy(nes.mapper)
    m.chr0, m.chr1, m.ctrl = chr0, chr1, mctrl
    chr_ = nes.chr

    def tile_row(table, tile, row):
        off = m.chr_offset(table + tile * 16 + row) % len(chr_)
        lo, hi = chr_[off], chr_[(off + 8) % len(chr_)]
        return [((lo >> (7 - i)) & 1) | (((hi >> (7 - i)) & 1) << 1) for i in range(8)]

    W, H = 512, 480
    img = [[0] * W for _ in range(H)]
    bg_table = 0x1000 if ctrl & 0x10 else 0
    for nt in range(4):
        ox, oy = (nt & 1) * 256, (nt >> 1) * 240
        base = 0x2000 + nt * 0x400
        for ty in range(30):
            for tx in range(32):
                tile = ciram[m.mirror(base + ty * 32 + tx)]
                at = ciram[m.mirror(base + 0x3C0 + (ty // 4) * 8 + tx // 4)]
                shift = ((ty % 4) // 2) * 4 + ((tx % 4) // 2) * 2
                pi = (at >> shift) & 3
                for r in range(8):
                    px = tile_row(bg_table, tile, r)
                    row = img[oy + ty * 8 + r]
                    for i, c in enumerate(px):
                        row[ox + tx * 8 + i] = PALETTE[pal[pi * 4 + c] if c else pal[0]]
    sx = ((t & 0x1F) << 3 | fx) + ((t >> 10) & 1) * 256
    sy = ((t >> 5) & 0x1F) * 8 + ((t >> 12) & 7) + ((t >> 11) & 1) * 240
    tall = ctrl & 0x20
    sp_table = 0x1000 if ctrl & 0x08 else 0
    for i in range(63, -1, -1):
        y, tile, attr, x = oam[i * 4:i * 4 + 4]
        if y >= 0xEF:
            continue
        for r in range(16 if tall else 8):
            rr = r if not attr & 0x80 else (15 if tall else 7) - r
            if tall:
                table = (tile & 1) * 0x1000
                tt = (tile & 0xFE) + (1 if rr >= 8 else 0)
                px = tile_row(table, tt, rr & 7)
            else:
                px = tile_row(sp_table, tile, rr)
            if attr & 0x40:
                px = px[::-1]
            for j, c in enumerate(px):
                if c:
                    X, Y = (sx + x + j) % W, (sy + y + 1 + r) % H
                    img[Y][X] = PALETTE[pal[16 + (attr & 3) * 4 + c]]
    for k in range(256):
        for (X, Y) in ((sx + k, sy), (sx + k, sy + 239)):
            img[Y % H][X % W] = 0xFF0000
    for k in range(240):
        for (X, Y) in ((sx, sy + k), (sx + 255, sy + k)):
            img[Y % H][X % W] = 0xFF0000
    raw = b''.join(b'\x00' + b''.join(struct.pack('>I', p)[1:] for p in row) for row in img)

    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def parse_buttons(s):
    v = 0
    for b in s.lower().split('+'):
        if b not in ('', 'none'):
            v |= BUTTONS[b]
    return v


def run_script(nes, lines, shots=None, states=None, rng=None):
    states = {} if states is None else states
    rng = rng or _random.Random(1)
    i = 0
    while i < len(lines):
        f = lines[i].split('#', 1)[0].split()
        i += 1
        if not f:
            continue
        cmd, args = f[0], f[1:]
        if cmd == 'repeat':
            depth, j = 1, i
            while depth:
                g = lines[j].split('#', 1)[0].split()
                if g and g[0] == 'repeat':
                    depth += 1
                elif g and g[0] == 'end':
                    depth -= 1
                j += 1
            body = lines[i:j - 1]
            for _ in range(int(args[0])):
                nes = run_script(nes, body, shots, states, rng)
            i = j
        elif cmd == 'wait':
            nes.joy = 0
            for _ in range(int(args[0])):
                nes.run_frame()
        elif cmd == 'hold':
            nes.joy = parse_buttons(args[0])
            for _ in range(int(args[1])):
                nes.run_frame()
            nes.joy = 0
        elif cmd == 'tap':
            nes.joy = parse_buttons(args[0])
            for _ in range(4):
                nes.run_frame()
            nes.joy = 0
            for _ in range(8):
                nes.run_frame()
        elif cmd == 'random':
            if len(args) > 1:
                rng.seed(int(args[1]))
            n = int(args[0])
            while n > 0:
                k = rng.randint(4, 30)
                nes.joy = rng.choice([0x10, 0x20, 0x40, 0x80]) | (0x01 if rng.random() < 0.15 else 0)
                for _ in range(min(k, n)):
                    nes.run_frame()
                n -= k
            nes.joy = 0
        elif cmd == 'shot':
            if shots:
                nes.screenshot(os.path.join(shots, args[0] + '.png'))
        elif cmd == 'poke':
            nes.ram[int(args[0], 16) & 0x7FF] = int(args[1], 16)
        elif cmd == 'peek':
            a, n = int(args[0], 16), int(args[1]) if len(args) > 1 else 1
            print('frame %d $%04X:' % (nes.frame, a), ' '.join('%02X' % nes.ram[(a + k) & 0x7FF] for k in range(n)))
        elif cmd == 'save':
            cdl = nes.cdl
            nes.cdl = None
            states[args[0]] = copy.deepcopy(nes)
            nes.cdl = cdl
        elif cmd == 'load':
            cdl = nes.cdl
            nes = copy.deepcopy(states[args[0]])
            nes.cdl = cdl
        elif cmd == 'print':
            print('frame %d: %s' % (nes.frame, ' '.join(args)))
        else:
            raise SystemExit('unknown script command: ' + cmd)
    return nes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('script')
    ap.add_argument('--cdl', help='write a Code/Data Log here')
    ap.add_argument('--merge', action='store_true', help='OR the new log into an existing --cdl file')
    ap.add_argument('--shots', help='directory for screenshots')
    args = ap.parse_args()

    nes = NES(open(args.rom, 'rb').read())
    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
    lines = open(args.script, encoding='utf-8').read().split('\n')
    try:
        nes = run_script(nes, lines, args.shots)
    finally:
        if args.cdl:
            if args.merge and os.path.exists(args.cdl):
                old = open(args.cdl, 'rb').read()[9:9 + len(nes.cdl)]
                for k, b in enumerate(old):
                    nes.cdl[k] |= b
            nes.write_cdl(args.cdl)
        code = sum(1 for b in nes.cdl if b & 1)
        data = sum(1 for b in nes.cdl if b & 2 and not b & 1)
        print('frames %d, code bytes %d (%.1f%%), data-only bytes %d' % (
            nes.frame, code, 100.0 * code / len(nes.cdl), data))


if __name__ == '__main__':
    main()

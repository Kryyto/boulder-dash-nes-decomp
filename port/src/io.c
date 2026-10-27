/* The hardware the game talks to: every PPU, APU, joypad and mapper access
 * of the ported routines goes through io_write / io_read, as the 6502's
 * STA/LDA to $2000-$4017 and $8000+ do.
 *
 * Behind them is a model of the hardware (hw): MMC1 (serial writes, CHR
 * banks, mirroring), the PPU (2 KB of nametable RAM, palette RAM, OAM, the
 * address/scroll latches, the PPUDATA read buffer, CHR ROM read through the
 * banks) and the joypads. APU writes are handed to bd_apu_write (bdview
 * plugs its APU emulation in there). ppu_render draws a frame from the model
 * the way the PPU does, scanline by scanline from the scroll registers,
 * including the mid-frame split the title screen makes at sprite 0.
 *
 * bd_io_trace sees every access, and bd_io_replay can answer reads instead of
 * the model: the RAM-diff test uses them to compare the port's accesses with
 * the original's and to replay the values the original read.
 */
#include <string.h>

#include "bd.h"

BdHw hw;

static void apu_nop(uint16_t addr, uint8_t value) { (void)addr; (void)value; }
void (*bd_apu_write)(uint16_t addr, uint8_t value) = apu_nop;
void (*bd_io_trace)(uint16_t addr, uint8_t value, bool read);
bool (*bd_io_replay)(uint16_t addr, uint8_t *value);

void hw_reset(void)
{
    memset(&hw, 0, sizeof hw);
    hw.mmc1_shift = 0x10;
    hw.mmc1_ctrl = 0x0C;
    hw.split_line = -1;
}

/* ---- MMC1 ---- */

static void mmc1_write(uint16_t addr, uint8_t v)
{
    if (v & 0x80) {
        hw.mmc1_shift = 0x10;
        hw.mmc1_ctrl |= 0x0C;
        return;
    }
    uint8_t done = hw.mmc1_shift & 1;
    hw.mmc1_shift = (uint8_t)((hw.mmc1_shift >> 1) | ((v & 1) << 4));
    if (!done)
        return;
    uint8_t r = hw.mmc1_shift;
    hw.mmc1_shift = 0x10;
    if (addr < 0xA000)
        hw.mmc1_ctrl = r;
    else if (addr < 0xC000)
        hw.mmc1_chr0 = r;
    else if (addr < 0xE000)
        hw.mmc1_chr1 = r;
    else
        hw.mmc1_prg = r;
}

static uint32_t chr_offset(uint16_t a)
{
    if (hw.mmc1_ctrl & 0x10) /* two 4 KB banks */
        return (uint32_t)((a < 0x1000 ? hw.mmc1_chr0 : hw.mmc1_chr1) * 0x1000 + (a & 0x0FFF));
    return (uint32_t)((hw.mmc1_chr0 & 0x1E) * 0x1000 + (a & 0x1FFF));
}

/* nametable address ($2000-$2FFF) -> offset in the 2 KB of nametable RAM */
static uint16_t nt_mirror(uint16_t a)
{
    a &= 0x0FFF;
    switch (hw.mmc1_ctrl & 3) {
    case 0: return a & 0x3FF;                           /* one screen, lower */
    case 1: return (uint16_t)(0x400 | (a & 0x3FF));     /* one screen, upper */
    case 2: return a & 0x7FF;                           /* vertical */
    default: return (uint16_t)(((a >> 1) & 0x400) | (a & 0x3FF)); /* horizontal */
    }
}

/* ---- PPU ---- */

static uint8_t pal_index(uint16_t a)
{
    uint8_t i = a & 0x1F;
    return (i & 0x13) == 0x10 ? (uint8_t)(i - 0x10) : i; /* $10/$14/$18/$1C mirror $00/$04/... */
}

uint8_t ppu_peek(uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return chr(chr_offset(a));
    if (a < 0x3F00)
        return hw.ciram[nt_mirror(a)];
    return hw.pal[pal_index(a)];
}

static void ppu_poke(uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return; /* CHR ROM */
    if (a < 0x3F00)
        hw.ciram[nt_mirror(a)] = v;
    else
        hw.pal[pal_index(a)] = v & 0x3F;
}

static void ppu_inc(void) { hw.v = (uint16_t)((hw.v + ((hw.ctrl & 0x04) ? 32 : 1)) & 0x7FFF); }

static void ppu_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0:
        hw.ctrl = v;
        hw.t = (uint16_t)((hw.t & 0xF3FF) | ((v & 3) << 10));
        break;
    case 1: hw.mask = v; break;
    case 3: hw.oam_addr = v; break;
    case 4: hw.oam[hw.oam_addr++] = v; break;
    case 5:
        if (!hw.w) {
            hw.t = (uint16_t)((hw.t & 0xFFE0) | (v >> 3));
            hw.x = v & 7;
        } else {
            hw.t = (uint16_t)((hw.t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2));
        }
        hw.w ^= 1;
        break;
    case 6:
        if (!hw.w) {
            hw.t = (uint16_t)((hw.t & 0x00FF) | ((v & 0x3F) << 8));
        } else {
            hw.t = (uint16_t)((hw.t & 0xFF00) | v);
            hw.v = hw.t;
        }
        hw.w ^= 1;
        break;
    case 7:
        ppu_poke(hw.v, v);
        ppu_inc();
        break;
    default: break;
    }
}

static uint8_t ppu_read(uint8_t r)
{
    switch (r) {
    case 2: {
        uint8_t v = hw.status;
        hw.status &= 0x7F;
        hw.w = 0;
        return v;
    }
    case 4: return hw.oam[hw.oam_addr];
    case 7: {
        uint16_t a = hw.v & 0x3FFF;
        uint8_t v;
        if (a < 0x3F00) {
            v = hw.buf;
            hw.buf = ppu_peek(a);
        } else {
            v = ppu_peek(a);
            hw.buf = ppu_peek((uint16_t)(a - 0x1000));
        }
        ppu_inc();
        return v;
    }
    default: return 0;
    }
}

/* ---- the bus ---- */

void io_write(uint16_t addr, uint8_t value)
{
    if (bd_io_trace)
        bd_io_trace(addr, value, false);
    if (addr >= 0x8000) {
        mmc1_write(addr, value);
    } else if (addr >= 0x2000 && addr < 0x4000) {
        ppu_write(addr & 7, value);
    } else if (addr == 0x4014) { /* OAM DMA from page value */
        for (int i = 0; i < 256; i++)
            hw.oam[(uint8_t)(hw.oam_addr + i)] = R((uint16_t)(value << 8 | i));
    } else if (addr == 0x4016) {
        hw.strobe = value & 1;
        hw.pad_shift[0] = hw.pad[0];
        hw.pad_shift[1] = hw.pad[1];
    } else if (addr >= 0x4000 && addr <= 0x4017) {
        bd_apu_write(addr, value);
    }
}

uint8_t io_read(uint16_t addr)
{
    uint8_t v = 0;
    if (addr >= 0x2000 && addr < 0x4000) {
        v = ppu_read(addr & 7);
    } else if (addr == 0x4016 || addr == 0x4017) {
        int p = addr & 1;
        if (hw.strobe) {
            v = 0x40 | (hw.pad[p] & 1);
        } else {
            v = 0x40 | (hw.pad_shift[p] & 1);
            hw.pad_shift[p] = (uint8_t)((hw.pad_shift[p] >> 1) | 0x80);
        }
    }
    if (bd_io_replay)
        bd_io_replay(addr, &v);
    if (bd_io_trace)
        bd_io_trace(addr, v, true);
    return v;
}

/* ---- frames ---- */

void ppu_frame_start(void)
{
    hw.frame_t = hw.t;
    hw.frame_x = hw.x;
    hw.frame_ctrl = hw.ctrl;
    hw.frame_mask = hw.mask;
    hw.split_line = -1;
}

void ppu_sprite0_split(void)
{
    /* the hit comes on the first line of sprite 0 (drawn from y + 1); the
     * writes that follow take effect from the next line */
    hw.split_line = hw.oam[0] + 2;
}

static uint8_t pattern_pixel(uint16_t table, uint8_t tile, uint8_t row, uint8_t col)
{
    uint16_t a = (uint16_t)(table + tile * 16 + row);
    uint8_t lo = ppu_peek(a), hi = ppu_peek((uint16_t)(a + 8));
    return (uint8_t)(((lo >> (7 - col)) & 1) | (((hi >> (7 - col)) & 1) << 1));
}

/* one background line from v (coarse/fine scroll, nametable bits) and fine x */
static void render_bg_line(uint16_t v, uint8_t fx, uint8_t ctrl, uint8_t out[256])
{
    uint16_t table = (ctrl & 0x10) ? 0x1000 : 0x0000;
    uint8_t fine_y = (v >> 12) & 7;
    for (int px = 0; px < 256; px++) {
        int sx = fx + px;
        int cx = (v & 0x1F) + (sx >> 3);
        uint16_t nt = (uint16_t)(v & 0x0C00);
        if (cx >= 32) {
            cx -= 32;
            nt ^= 0x0400;
        }
        uint16_t na = (uint16_t)(0x2000 | nt | (v & 0x03E0) | cx);
        uint8_t tile = ppu_peek(na);
        uint8_t cy = (v >> 5) & 0x1F;
        uint8_t at = ppu_peek((uint16_t)(0x23C0 | nt | ((cy >> 2) << 3) | (cx >> 2)));
        uint8_t pi = (at >> (((cy & 2) << 1) | (cx & 2))) & 3;
        uint8_t c = pattern_pixel(table, tile, fine_y, (uint8_t)(sx & 7));
        out[px] = c ? (uint8_t)(pi << 2 | c) : 0;
    }
}

static uint16_t next_line(uint16_t v) /* the PPU's vertical increment */
{
    if ((v & 0x7000) != 0x7000)
        return (uint16_t)(v + 0x1000);
    v &= 0x0FFF;
    uint16_t y = (v >> 5) & 0x1F;
    if (y == 29) {
        y = 0;
        v ^= 0x0800;
    } else if (y == 31) {
        y = 0;
    } else {
        y++;
    }
    return (uint16_t)((v & ~0x03E0) | (y << 5));
}

void ppu_render(uint8_t out[240][256])
{
    uint8_t mask = hw.frame_mask & hw.mask;
    uint8_t backdrop = hw.pal[0];
    if (!(mask & 0x18)) {
        memset(out, backdrop, 240 * 256);
        return;
    }
    static uint8_t bg[256];
    static uint8_t opaque[240][256];
    /* background: from the frame's scroll; after the sprite-0 split, from
     * where the game pointed v */
    uint16_t v = hw.frame_t;
    uint8_t fx = hw.frame_x, ctrl = hw.frame_ctrl;
    for (int line = 0; line < 240; line++) {
        if (line == hw.split_line) {
            v = hw.v;
            fx = hw.x;
            ctrl = hw.ctrl;
        } else if (line) { /* down a line; the horizontal scroll again from t */
            uint16_t t = hw.split_line >= 0 && line > hw.split_line ? hw.t : hw.frame_t;
            v = (uint16_t)((next_line(v) & ~0x041F) | (t & 0x041F));
        }
        if (mask & 0x08) {
            render_bg_line(v, fx, ctrl, bg);
            if (!(mask & 0x02))
                memset(bg, 0, 8);
        } else {
            memset(bg, 0, 256);
        }
        for (int px = 0; px < 256; px++) {
            opaque[line][px] = bg[px] & 3;
            out[line][px] = hw.pal[bg[px] & 3 ? bg[px] : 0];
        }
    }
    if (!(mask & 0x10))
        return;
    /* sprites: at most 8 per line, lower OAM index in front */
    bool tall = hw.frame_ctrl & 0x20;
    int h = tall ? 16 : 8;
    for (int line = 0; line < 240; line++) {
        int found[8], n = 0;
        for (int i = 0; i < 64 && n < 8; i++) {
            int y = hw.oam[i * 4] + 1;
            if (line >= y && line < y + h)
                found[n++] = i;
        }
        for (int k = n - 1; k >= 0; k--) {
            const uint8_t *s = &hw.oam[found[k] * 4];
            int row = line - (s[0] + 1);
            uint8_t attr = s[2];
            if (attr & 0x80)
                row = h - 1 - row;
            uint16_t table;
            uint8_t tile = s[1];
            if (tall) {
                table = (tile & 1) ? 0x1000 : 0x0000;
                tile = (uint8_t)((tile & 0xFE) + (row >= 8));
            } else {
                table = (hw.frame_ctrl & 0x08) ? 0x1000 : 0x0000;
            }
            for (int col = 0; col < 8; col++) {
                int px = s[3] + col;
                if (px > 255 || (px < 8 && !(mask & 0x04)))
                    continue;
                uint8_t c = pattern_pixel(table, tile, (uint8_t)(row & 7), (uint8_t)((attr & 0x40) ? 7 - col : col));
                if (!c)
                    continue;
                if ((attr & 0x20) && opaque[line][px])
                    continue; /* behind the background */
                out[line][px] = hw.pal[0x10 | (attr & 3) << 2 | c];
            }
        }
    }
}

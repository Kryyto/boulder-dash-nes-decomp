/* The game's PPU, mapper and joypad helpers ($A0A2-$A4F4, $DA64, $DA79),
 * faithful ports on top of io_write / io_read. */
#include "bd.h"

#define PPUCTRL   0x2000
#define PPUMASK   0x2001
#define PPUSCROLL 0x2005
#define PPUADDR   0x2006
#define PPUDATA   0x2007

void SetPPUAddr(uint8_t hi, uint8_t lo) /* $A1A6: X = high, Y = low */
{
    io_write(PPUADDR, hi);
    io_write(PPUADDR, lo);
}

void RestorePPUCtrlScroll(void) /* $A1AD */
{
    io_write(PPUCTRL, ram.ppuctrl_shadow);
    io_write(PPUSCROLL, ram.scroll_x);
    io_write(PPUSCROLL, ram.scroll_y);
}

void UploadPalette(void) /* $A0A2 */
{
    SetPPUAddr(0x3F, 0x00);
    for (int x = 0; x < 0x20; x++)
        io_write(PPUDATA, ram.palette_buf[x]);
    io_write(PPUADDR, 0x3F);
    io_write(PPUADDR, 0x00);
    io_write(PPUADDR, 0x00);
    io_write(PPUADDR, 0x00);
    RestorePPUCtrlScroll();
}

void HideAllSprites(void) /* $A0DD */
{
    for (int x = 0; x < 0x100; x += 4) {
        ram.oam_buf[x] = 0xF0;
        ram.oam_buf[x + 1] = 0;
        ram.oam_buf[x + 2] = 0;
        ram.oam_buf[x + 3] = 0;
    }
}

static void FillNametablePage(uint8_t hi) /* $A115: 1 KB of $10 from hi:00 */
{
    SetPPUAddr(hi, 0x00);
    for (int i = 0; i < 0x400; i++)
        io_write(PPUDATA, R(0x10));
    RestorePPUCtrlScroll();
}

void ClearNametables(void) /* $A0F9: $2000, and $2400 or $2800 (by mirroring) */
{
    R(0x10) = 0xFF;
    FillNametablePage(0x20);
    R(0x10) = 0xFF;
    FillNametablePage((ram.mmc1_ctrl_shadow & 0x01) ? 0x28 : 0x24);
}

void ClearScreen(void) /* $A0D6 */
{
    ClearNametables();
    HideAllSprites();
}

void DisableRendering(void) /* $A193: also disables the NMI */
{
    ram.ppuctrl_shadow &= 0x7F;
    io_write(PPUCTRL, ram.ppuctrl_shadow);
    ram.ppumask_shadow &= 0xE7;
    io_write(PPUMASK, ram.ppumask_shadow);
}

/* $A1BD: X/Y = pointer. Stream: lo hi = address, then tile bytes; FE lo hi =
 * new address, FD = next row, FC = toggle +32 increment, FB n = n raw bytes,
 * FF = end. */
void DrawPPUString(uint8_t hi, uint8_t lo)
{
    R(0x12) = hi;
    R(0x11) = lo;
    uint8_t y = 0;
    goto set_addr;
    for (;;) {
        uint8_t a = rd((uint16_t)(zp_ptr(0x11) + y));
        y++;
        if (a == 0xFF)
            break;
        if (a == 0xFE)
            goto set_addr;
        if (a == 0xFD) { /* next row */
            R(0x10) = y;
            uint16_t row = (uint16_t)((R(0x13) | R(0x14) << 8) + 0x20);
            R(0x13) = (uint8_t)row;
            R(0x14) = (uint8_t)(row >> 8);
            SetPPUAddr(R(0x14), R(0x13));
            goto advance;
        }
        if (a == 0xFC) {
            ram.ppuctrl_shadow ^= 0x04;
            io_write(PPUCTRL, ram.ppuctrl_shadow);
            continue;
        }
        if (a == 0xFB) { /* n raw bytes */
            R(0x10) = rd((uint16_t)(zp_ptr(0x11) + y));
            y++;
            set_zp_ptr(0x11, (uint16_t)(zp_ptr(0x11) + y));
            y = 0;
            do {
                io_write(PPUDATA, rd((uint16_t)(zp_ptr(0x11) + y)));
                y++;
            } while (--R(0x10) != 0);
            continue;
        }
        io_write(PPUDATA, a);
        continue;
    set_addr: {
            uint8_t alo = rd((uint16_t)(zp_ptr(0x11) + y));
            y++;
            uint8_t ahi = rd((uint16_t)(zp_ptr(0x11) + y));
            y++;
            R(0x10) = y;
            R(0x13) = alo;
            R(0x14) = ahi;
            SetPPUAddr(ahi, alo);
        }
    advance:
        set_zp_ptr(0x11, (uint16_t)(zp_ptr(0x11) + R(0x10)));
        y = 0;
    }
    ram.ppuctrl_shadow &= 0xFB;
    io_write(PPUCTRL, ram.ppuctrl_shadow);
    RestorePPUCtrlScroll();
}

static void mmc1_serial(uint16_t reg, uint8_t a) /* five writes, low bit first */
{
    for (int i = 0; i < 5; i++) {
        io_write(reg, a);
        a >>= 1;
    }
}

void MMC1_SetCtrl(uint8_t a) { mmc1_serial(0x9FFF, a); } /* $A263 */
void MMC1_SetCHR0(uint8_t a) { mmc1_serial(0xBFFF, a); } /* $A277 */
void MMC1_SetCHR1(uint8_t a) { mmc1_serial(0xDFFF, a); } /* $A28B */

void ResetScroll(void) /* $A2F4 */
{
    ram.camera_x[0] = ram.camera_x[1] = 0;
    ram.camera_y[0] = ram.camera_y[1] = 0;
    ram.scroll_x = ram.scroll_y = 0;
    ram.mmc1_ctrl_shadow = 0x13;
    MMC1_SetCtrl(0x13);
    ram.ppuctrl_shadow &= 0xFC;
    io_write(PPUCTRL, ram.ppuctrl_shadow);
}

/* $A313: 512 bytes of the PPU (CHR bank a, address hi:lo) to $0600 */
void ReadVramTo600(uint8_t bank, uint8_t hi, uint8_t lo)
{
    MMC1_SetCHR1(bank);
    SetPPUAddr(hi, lo);
    io_read(PPUDATA); /* the read buffer's stale byte */
    for (int x = 0; x < 0x200; x++)
        R(0x600 + x) = io_read(PPUDATA);
    RestorePPUCtrlScroll();
}

/* $A334: 1 KB from hi:lo to nametable a (0/1/2: $2000/$2400/$2800) */
void LoadNametable(uint8_t a, uint8_t hi, uint8_t lo)
{
    R(0x55) = lo;
    R(0x56) = hi;
    SetPPUAddr(a == 0 ? 0x20 : a == 1 ? 0x24 : 0x28, 0x00);
    for (int x = 4;;) {
        uint8_t y = 0;
        do {
            io_write(PPUDATA, *ind_y(0x55, y));
        } while (++y != 0);
        if (--x == 0)
            break;
        R(0x56)++;
    }
    RestorePPUCtrlScroll();
}

static void LoadPalette16(uint8_t dest, uint8_t hi, uint8_t lo) /* $A421 */
{
    R(0x68) = 0;
    R(0x67) = dest;
    R(0x69) = lo;
    R(0x6A) = hi;
    for (int y = 0x0F; y >= 0; y--)
        *ind_y(0x67, (uint8_t)y) = *ind_y(0x69, (uint8_t)y);
    for (int x = 0; x < 0x10; x += 4) /* every sprite palette starts with the backdrop */
        R(0x3B + x) = ram.palette_buf[0];
}

void LoadBGPalette(uint8_t hi, uint8_t lo) { LoadPalette16(0x2B, hi, lo); }     /* $A40E */
void LoadSpritePalette(uint8_t hi, uint8_t lo) { LoadPalette16(0x3B, hi, lo); } /* $A419 */

void BlackPaletteBuf(void) /* $A43D */
{
    for (int x = 0; x < 0x20; x += 4)
        ram.palette_buf[x] = 0x0F;
}

void EnableNMI(void) /* $A44C */
{
    ram.ppuctrl_shadow |= 0x80;
    io_write(PPUCTRL, ram.ppuctrl_shadow);
}

void FadeIn(void) /* $A456: from black in 4 steps, 4 frames each */
{
    for (int x = 0x1F; x >= 0; x--)
        R(0x116 + x) = ram.palette_buf[x];
    R(0x67) = 0x30;
    do {
        for (int x = 0x1F; x >= 0; x--) {
            uint8_t v = (uint8_t)(R(0x116 + x) - R(0x67));
            ram.palette_buf[x] = (v & 0x80) ? 0x0F : v;
        }
        ram.nmi_requests |= 0x01;
        WaitFrames(4);
        R(0x67) = (uint8_t)(R(0x67) - 0x10);
    } while (!(R(0x67) & 0x80));
}

/* $A4E1: wait until the sprite 0 flag clears, then until it is set again.
 * The PPU isn't emulated cycle by cycle: the model notes that the writes
 * that follow happen at sprite 0's line, for the renderer. */
void WaitSprite0Hit(void)
{
    ppu_sprite0_split();
}

void RequestRenderingOn(void) { ram.nmi_requests |= 0x02; } /* $A4EE */

/* $DA64: 1 KB of a at hi:lo (a nametable and its attributes) */
void FillVram1K(uint8_t a, uint8_t hi, uint8_t lo)
{
    R(0x67) = a;
    SetPPUAddr(hi, lo);
    for (int i = 0; i < 0x400; i++)
        io_write(PPUDATA, R(0x67));
}

/* $DA79: 64 bytes of a at hi:lo (an attribute table) */
void FillVram64(uint8_t a, uint8_t hi, uint8_t lo)
{
    R(0x67) = a;
    SetPPUAddr(hi, lo);
    for (int i = 0; i < 0x40; i++)
        io_write(PPUDATA, R(0x67));
}

/* $A12B: both pads read twice; kept only when both reads agree (a DPCM
 * glitch guard), then the newly pressed buttons */
void ReadJoypads(void)
{
    ram.joy1_prev = ram.joy1_held;
    ram.joy2_prev = ram.joy2_held;
    R(0x10) = 0x01;
    for (;;) {
        io_write(0x4016, 0x01);
        io_write(0x4016, 0x00);
        for (int x = 8; x > 0; x--) {
            uint8_t a = io_read(0x4016);
            uint8_t bit = (a & 1) | ((a >> 1) & 1); /* ror; bcs; ror: bit 0 or bit 1 */
            R(0x11) = (uint8_t)((R(0x11) >> 1) | (bit << 7));
            a = io_read(0x4017);
            bit = (a & 1) | ((a >> 1) & 1);
            R(0x13) = (uint8_t)((R(0x13) >> 1) | (bit << 7));
        }
        if ((int8_t)--R(0x10) < 0)
            break;
        R(0x12) = R(0x11);
        R(0x14) = R(0x13);
    }
    if (R(0x11) != R(0x12) || R(0x13) != R(0x14))
        return;
    ram.joy1_held = R(0x11);
    ram.joy2_held = R(0x13);
    R(0x27) = ram.joy1_held ^ ram.joy1_prev;
    R(0x28) = ram.joy2_held ^ ram.joy2_prev;
    ram.joy1_pressed = ram.joy1_held & R(0x27);
    ram.joy2_pressed = ram.joy2_held & R(0x28);
}

/* The NMI handler ($A2C7) with its PPU updates ($B001), frame waits, fades.
 * Faithful ports: the same RAM writes and the same PPU / mapper accesses. */
#include "bd.h"

#define PPUCTRL   0x2000
#define PPUMASK   0x2001
#define PPUSTATUS 0x2002
#define PPUSCROLL 0x2005
#define PPUADDR   0x2006
#define PPUDATA   0x2007

static void default_frame(void)
{
    NmiHandler();
    ppu_frame_start();
}
void (*bd_frame_hook)(void) = default_frame;

static void Nmi_FlushVramBuffer(void) /* $B097: (hi lo count bytes...)... FF */
{
    uint8_t x = 0;
    do {
        io_write(PPUADDR, R(0x117 + x));
        io_write(PPUADDR, R(0x116 + x));
        x = (uint8_t)(x + 2);
        uint8_t y = R(0x116 + x);
        x++;
        do {
            io_write(PPUDATA, R(0x116 + x));
            x++;
        } while (--y != 0);
    } while (R(0x116 + x) != 0xFF);
    RestorePPUCtrlScroll();
}

/* one queued metatile: 2 tiles at hi:lo, 2 more a row below */
static void flush_metatile(uint8_t x)
{
    io_write(PPUADDR, R(0xC8 + x));
    io_write(PPUADDR, R(0xC7 + x));
    io_write(PPUDATA, R(0xC9 + x));
    io_write(PPUDATA, R(0xCA + x));
    uint16_t below = (uint16_t)((R(0xC7 + x) | R(0xC8 + x) << 8) + 0x20);
    io_write(PPUADDR, (uint8_t)(below >> 8));
    io_write(PPUADDR, (uint8_t)below);
    io_write(PPUDATA, R(0xCB + x));
    io_write(PPUDATA, R(0xCC + x));
}

static void flush_attribute(uint8_t x)
{
    io_write(PPUADDR, R(0xEC + x));
    io_write(PPUADDR, R(0xEB + x));
    io_write(PPUDATA, R(0xED + x));
}

void Nmi_FlushMetatiles4(void) /* $B0C1: up to 4 metatiles, each with its attribute */
{
    R(0x63) = R(0x64) = 0;
    do {
        if (R(0xC8 + R(0x63)) & 0x80)
            break;
        flush_metatile(R(0x63));
        flush_attribute(R(0x64));
        R(0x63) = (uint8_t)(R(0x63) + 6);
        R(0x64) = (uint8_t)(R(0x64) + 3);
    } while (R(0x64) < 0x0C);
    RestorePPUCtrlScroll();
}

static void Nmi_FlushMetatiles6(void) /* $B129: 6 metatiles, then 6 attributes */
{
    R(0x63) = R(0x64) = 0;
    do {
        if (!(R(0xC8 + R(0x63)) & 0x80))
            flush_metatile(R(0x63));
        R(0x63) = (uint8_t)(R(0x63) + 6);
    } while (R(0x63) < 0x24);
    do {
        if (!(R(0xEC + R(0x64)) & 0x80))
            flush_attribute(R(0x64));
        R(0x64) = (uint8_t)(R(0x64) + 3);
    } while (R(0x64) < 0x12);
    RestorePPUCtrlScroll();
}

/* $B001: scroll, PPUCTRL, the mapper's state from the shadows, then one
 * handler per set bit of nmi_requests */
static void NmiPpuUpdate(void)
{
    io_read(PPUSTATUS);
    io_write(PPUSCROLL, ram.scroll_x);
    io_write(PPUSCROLL, ram.scroll_y);
    io_write(PPUCTRL, ram.ppuctrl_shadow);
    MMC1_SetCtrl(ram.mmc1_ctrl_shadow);
    MMC1_SetCHR0(ram.chr0_bank);
    MMC1_SetCHR1(ram.chr1_bank);
    uint8_t req = ram.nmi_requests;
    for (uint8_t x = 0; x < 8; x++, req >>= 1) {
        if (!(req & 1))
            continue;
        uint8_t i = (uint8_t)(x << 1);
        R(0x65) = prg((uint16_t)(0xB06E + i)); /* the handler address, as the jump leaves it */
        R(0x66) = prg((uint16_t)(0xB06F + i));
        switch (x) {
        case 0: UploadPalette(); break;
        case 1: /* rendering on */
            ram.ppumask_shadow |= 0x18;
            io_write(PPUMASK, ram.ppumask_shadow);
            break;
        case 2: /* rendering off */
            ram.ppumask_shadow &= 0xE7;
            io_write(PPUMASK, ram.ppumask_shadow);
            break;
        case 3: Nmi_FlushVramBuffer(); break;
        case 4: Nmi_FlushMetatiles4(); break;
        case 5: Nmi_FlushMetatiles6(); break;
        default: break;
        }
    }
    ram.nmi_requests = 0;
}

void NmiHandler(void)
{
    if (ram.nmi_ppu_enable) {
        io_read(PPUSTATUS);
        io_write(0x2003, 0x00);
        io_write(0x4014, 0x02); /* OAM DMA from oam_buf */
        NmiPpuUpdate();
        ram.nmi_ppu_enable = 0xFF;
    }
    Sound_Update();
    ram.nmi_done = 0xFF;
    ram.frame_count++;
}

void WaitForNMI(void) /* $A0C9 */
{
    ram.nmi_done = 0;
    bd_frame_hook();
    ram.nmi_done = 0;
}

void WaitFrames(uint8_t n) /* $A4C0 */
{
    do
        WaitForNMI();
    while (--n != 0);
}

void FadeOut(void) /* $A48A: 3 steps darker, 4 frames each */
{
    for (int x = 0x1F; x >= 0; x--)
        R(0x116 + x) = ram.palette_buf[x];
    R(0x67) = 0x10;
    do {
        for (int x = 0x1F; x >= 0; x--) {
            uint8_t v = (uint8_t)(R(0x116 + x) - R(0x67));
            ram.palette_buf[x] = (v & 0x80) ? 0x0F : v;
        }
        ram.nmi_requests |= 0x01;
        WaitFrames(4);
        R(0x67) = (uint8_t)(R(0x67) + 0x10);
    } while (R(0x67) < 0x40);
}

void BlackoutNow(void) /* $A4F5 */
{
    for (int x = 0x1F; x >= 0; x--)
        ram.palette_buf[x] = 0x0F;
    ram.nmi_requests |= 0x05;
}

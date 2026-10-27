/* Boot, the main loop's state dispatch, the boot screen and the title:
 * Reset ($FF8C), MainInit / InitGame ($A000 / $A022), State00_BootScreen
 * ($A618) and State01_Title ($A67E: $D7D9 and the title's helpers, the two
 * Rockfords running about, the high scores, the attract demo).
 * Faithful ports: same RAM writes and hardware accesses as the original.
 */
#include "bd.h"

static uint16_t word_at(uint16_t a) { return (uint16_t)(prg(a) | prg((uint16_t)(a + 1)) << 8); }

static void add16(uint16_t lo_addr, uint16_t value) /* 16-bit add to RAM lo, lo + 1 */
{
    uint16_t v = (uint16_t)((R(lo_addr) | R((uint16_t)(lo_addr + 1)) << 8) + value);
    R(lo_addr) = (uint8_t)v;
    R((uint16_t)(lo_addr + 1)) = (uint8_t)(v >> 8);
}

/* ---- boot ---- */

void ResetPlayerScore(uint8_t x) /* $AFF3: score and next extra life of the block at x */
{
    for (uint8_t y = 0; y < 6; y++, x++)
        R(0x78 + x) = prg((uint16_t)(0xDB41 + y));
}

void InitGame(void) /* $A022 */
{
    ram.nmi_done = 0;
    ram.mmc1_ctrl_shadow = 0x13;
    MMC1_SetCtrl(0x13);
    ram.chr0_bank = 0;
    ram.chr1_bank = 0;
    ram.ppuctrl_shadow = 0xB0;
    io_write(0x2000, 0xB0);
    ram.ppumask_shadow = 0x1E;
    ram.camera_x[0] = ram.camera_x[1] = 0;
    ram.scroll_x = 0;
    ram.camera_y[0] = ram.camera_y[1] = 0;
    ram.scroll_y = 0;
    Sound_Init();
    ClearScreen();
    BlackPaletteBuf();
    ram.nmi_requests = 0;
    ram.nmi_ppu_enable = 0xFF;
    SetGameState(0x01);
    /* a warm boot ("SAS" still in RAM) keeps the scores and skips the boot screen */
    if (!(R(0x176) == 0x53 && R(0x177) == 0x41 && R(0x178) == 0x53)) {
        ram.high_score[0] = ram.high_score[1] = ram.high_score[2] = 0;
        ResetPlayerScore(0x00);
        ResetPlayerScore(0x0A);
        R(0x73) = 0xFF; /* the demo to play next */
        SetGameState(0x00);
    }
    R(0x176) = 0x53;
    R(0x177) = 0x41;
    R(0x178) = 0x53;
}

void Reset(void)
{
    /* INC on a ROM byte with bit 7 set: MMC1 sees $80 then $81, a reset */
    uint16_t reset = word_at(0xFFFC);
    uint16_t reset_byte = word_at((uint16_t)(reset + 2));
    io_write(reset_byte, prg(reset_byte));
    io_write(reset_byte, (uint8_t)(prg(reset_byte) + 1));
    io_write(0x2000, 0x00);
    io_write(0x2001, 0x00);
    ram.ppuctrl_shadow = 0;
    ram.ppumask_shadow = 0;
    /* then two waits for vblank (the PPU warming up), which the model skips */
    io_write(0x4017, 0x80); /* MainInit: APU frame counter, 4-step, no IRQ */
    InitGame();
}

/* ---- the main loop ($A00B) ---- */

void RunState(void)
{
    static void (*const table[])(void) = {
        State00_BootScreen, State01_Title, State02_TitleMenu, State03_Password,
        State04_NextStage, State05_WorldSelectInit, State06_WorldSelect, State07_StageIntroInit,
        State08_StageIntro, State09_CaveSetup, State0A_CaveIntro, State0B_CavePlay,
        State0C_CaveCleared, State0D_LostLife, State0E_GameOver, State0F_Ending,
        State10_EndingScene, State11_Credits, State12_NextLevel,
    };
    R(0x11) = prg((uint16_t)(0xA5F2 + (uint8_t)(ram.game_state << 1)));
    R(0x12) = prg((uint16_t)(0xA5F3 + (uint8_t)(ram.game_state << 1)));
    if (ram.game_state < sizeof table / sizeof table[0])
        table[ram.game_state]();
}

void MainLoopFrame(void)
{
    WaitForNMI();
    ReadJoypads();
    RunState();
}

/* ---- the boot screen ---- */

void State00_BootScreen(void)
{
    if (ram.game_substate == 0) {
        DisableRendering();
        ResetScroll();
        ClearScreen();
        FillVram1K(0x17, 0x20, 0x00);
        FillVram64(0xFF, 0x23, 0xC0);
        ram.chr1_bank = 0x05;
        ReadVramTo600(0x05, 0x1E, 0x00); /* the screen's text, kept in CHR ROM */
        DrawPPUString(0x06, 0x00);
        LoadBGPalette(0xA6, 0x6E);
        ram.nmi_requests = 0x01;
        RequestRenderingOn();
        EnableNMI();
    }
    /* a button, or 256 frames, go to the title */
    if (!(ram.joy1_pressed & 0x0F) && ++ram.game_substate != 0)
        return;
    SetGameState(0x01);
}

/* ---- the title ($D7D9) ---- */

/* $DA89: below the logo (sprite 0), the score panel from the third
 * nametable, at a fixed scroll */
static void TitleSplit(void)
{
    WaitSprite0Hit();
    io_write(0x2000, ram.ppuctrl_shadow | 0x02);
    SetPPUAddr(0x2B, 0x40);
    io_write(0x2005, 0x00);
    io_write(0x2005, 0xD0);
}

/* $D368: the 24-bit $4D-$4F divided by $50, the remainder in $67 */
static void DivMod24(void)
{
    uint32_t n = (uint32_t)(R(0x4D) | R(0x4E) << 8 | R(0x4F) << 16);
    R(0x67) = (uint8_t)(n % R(0x50));
    n /= R(0x50);
    R(0x4D) = (uint8_t)n;
    R(0x4E) = (uint8_t)(n >> 8);
    R(0x4F) = (uint8_t)(n >> 16);
}

/* $D337: the 24-bit number in $AB-$AD to 6 decimal digits at $0100 (ones first) */
static void ScoreToDigits(void)
{
    R(0x4D) = R(0xAB);
    R(0x4E) = R(0xAC);
    R(0x4F) = R(0xAD);
    R(0x50) = 0x0A;
    uint8_t x = 0;
    do {
        DivMod24();
        R(0x100 + x) = R(0x67);
        x++;
    } while (R(0x4D) | R(0x4E) | R(0x4F));
    for (; x < 6; x++)
        R(0x100 + x) = 0;
}

static void LoadScore(uint8_t hi, uint8_t lo) /* $DAAA: 3 bytes at hi:lo -> $AB-$AD, digits */
{
    R(0x55) = lo;
    R(0x56) = hi;
    R(0xAB) = *ind_y(0x55, 0);
    R(0xAC) = *ind_y(0x55, 1);
    R(0xAD) = *ind_y(0x55, 2);
    ScoreToDigits();
}

static void DigitsToBuffer(uint8_t x) /* $DAC2: the 6 digits as tiles into vram_buffer + x */
{
    for (R(0x4D) = 5; !(R(0x4D) & 0x80); R(0x4D)--) {
        R(0x116 + x) = prg((uint16_t)(0xDAD8 + R(0x100 + R(0x4D))));
        x++;
    }
}

static void PlayerCursor(void) /* $DB47: the 1 PLAYER / 2 PLAYERS cursor */
{
    uint8_t x = R(0x53) ? 0x86 : 0x37;
    R(0x204) = 0x76;
    R(0x205) = 0x8E;
    R(0x206) = 0x01;
    R(0x207) = x;
}

static void LogoSprites(void) /* $DB62: the 4 logo pieces, then they move */
{
    ram.oam_index = 0x08;
    for (R(0x4D) = 3; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = (uint8_t)(R(0x4D) << 1);
        R(0x55) = prg((uint16_t)(0xDBA6 + x));
        R(0x57) = R(0x3E1 + x);
        R(0x56) = 0;
        R(0x58) = 0;
        DrawMetaspriteScreen(prg((uint16_t)(0xDBAF + x)), prg((uint16_t)(0xDBAE + x)));
    }
    for (int x = 6; x >= 0; x -= 2)
        add16((uint16_t)(0x3E0 + x), word_at((uint16_t)(0xDBD0 + x)));
}

static void FallingDiamonds(void) /* $DBD8: 6 diamonds tumbling down */
{
    for (R(0x4D) = 5; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = (uint8_t)(R(0x4D) << 2);
        R(0x55) = R(0x3E8 + x);
        R(0x56) = R(0x3E9 + x);
        R(0x57) = R(0x3EA + x);
        R(0x58) = R(0x3EB + x);
        uint8_t t = ram.frame_phase & 0x06;
        DrawMetaspriteWorld(0x00, prg((uint16_t)(0xDC35 + t)), prg((uint16_t)(0xDC34 + t)));
        add16((uint16_t)(0x3E8 + x), word_at((uint16_t)(0xDC60 + x)));
        add16((uint16_t)(0x3EA + x), word_at((uint16_t)(0xDC62 + x)));
    }
}

static void PanelSlide(void) /* $DC78: the score panel's slide ($0400: 1 out, 2 back) */
{
    if (R(0x400) == 0x02) {
        R(0x401) = (uint8_t)(R(0x401) - 4);
        if (R(0x401) < 0xA4)
            R(0x400) = 0;
        return;
    }
    if (R(0x400) != 0x01)
        return;
    add16(0x401, 4);
    if (R(0x402) == 0 && R(0x401) >= 0xD4)
        R(0x400)++;
}

/* The two Rockfords: 12-byte blocks at $0403 + x (state, timer, ?, y 24-bit
 * at $0406, x 24-bit at $0409, y speed 24-bit at $040C). */

static void RockfordSprites(void) /* $DCB5 */
{
    for (R(0x4D) = 1; !(R(0x4D) & 0x80); R(0x4D)--) {
        R(0x67) = (uint8_t)(R(0x4D) << 2);
        uint8_t x = (uint8_t)((R(0x67) << 1) + R(0x67));
        R(0x55) = R(0x407 + x);
        R(0x56) = R(0x408 + x);
        R(0x57) = R(0x40A + x);
        R(0x58) = R(0x40B + x);
        uint8_t s = R(0x403 + x) & 0x0F, hi, lo;
        if (s == 1 || s == 5 || s == 6 || s == 2 || s == 3 || s == 4) {
            set_zp_ptr(0x59, (R(0x403 + x) & 0x10) ? 0xD532 : 0xD506); /* facing left / right */
            uint8_t y = (s == 1 || s == 5 || s == 6) ? (uint8_t)((ram.frame_phase & 0x18) >> 2) : 0;
            lo = *ind_y(0x59, y);
            hi = *ind_y(0x59, (uint8_t)(y + 1));
        } else { /* standing: blinks */
            hi = 0xD4;
            lo = (ram.frame_phase & 0x20) ? 0x9D : 0x94;
        }
        DrawMetaspriteWorld(R(0x4D), hi, lo);
    }
}

static void RockfordFall(void) /* $DF05: y += y speed; speed += gravity */
{
    uint8_t x = R(0x4E);
    uint32_t y = (uint32_t)(R(0x406 + x) | R(0x407 + x) << 8 | R(0x408 + x) << 16);
    uint32_t s = (uint32_t)(R(0x40C + x) | R(0x40D + x) << 8 | R(0x40E + x) << 16);
    y += s;
    s += 0x38;
    R(0x406 + x) = (uint8_t)y;
    R(0x407 + x) = (uint8_t)(y >> 8);
    R(0x408 + x) = (uint8_t)(y >> 16);
    R(0x40C + x) = (uint8_t)s;
    R(0x40D + x) = (uint8_t)(s >> 8);
    R(0x40E + x) = (uint8_t)(s >> 16);
}

static void RockfordDrift(void) /* $DF3D: x += the jump's sideways speed ($DF67, by facing) */
{
    uint8_t x = R(0x4E);
    uint8_t y = (R(0x403 + x) & 0x10) ? 3 : 0;
    uint32_t p = (uint32_t)(R(0x409 + x) | R(0x40A + x) << 8 | R(0x40B + x) << 16);
    p += (uint32_t)(prg((uint16_t)(0xDF67 + y)) | prg((uint16_t)(0xDF68 + y)) << 8 |
                    prg((uint16_t)(0xDF69 + y)) << 16);
    R(0x409 + x) = (uint8_t)p;
    R(0x40A + x) = (uint8_t)(p >> 8);
    R(0x40B + x) = (uint8_t)(p >> 16);
}

static void RockfordWalkStep(uint8_t x) /* 16-bit x -1 or +1 by facing */
{
    add16((uint16_t)(0x40A + x), (R(0x403 + x) & 0x10) ? 1 : 0xFFFF);
}

static void RockfordFootstep(void) /* $DEE7 */
{
    if (!(ram.frame_phase & 0x0F))
        Sound_Play(0x0F);
}

static void RockfordsUpdate(void) /* $DD5F */
{
    for (R(0x4D) = 1; !(R(0x4D) & 0x80); R(0x4D)--) {
        R(0x67) = (uint8_t)(R(0x4D) << 2);
        uint8_t x = (uint8_t)((R(0x67) << 1) + R(0x67));
        R(0x4E) = x;
        uint8_t s = R(0x403 + x) & 0x0F;
        JumpTableInline(0xDD78, s);
        switch (s) {
        case 0: /* standing: wait, then walk (random wait from ROM bytes) */
            if (--R(0x404 + x) == 0) {
                R(0x403 + x)++;
                uint8_t y = (uint8_t)((R(0x403 + x) & 0x10) + ram.frame_phase);
                R(0x404 + x) = prg((uint16_t)(0xDD9D + y)) & 0x60;
            }
            break;
        case 1: /* walking: until the timer ends; turn at the edges or near the other one */
            if (--R(0x404 + x) == 0) {
                R(0x403 + x)--;
                uint8_t y = (uint8_t)((R(0x403 + x) & 0x10) + ram.frame_phase);
                R(0x404 + x) = prg((uint16_t)(0xDDBA + y)) & 0xC0;
                break;
            }
            {
                bool turn;
                if (R(0x403 + x) & 0x10)
                    turn = R(0x40A + x) >= 0xC0;
                else
                    turn = R(0x40A + x) < 0x38;
                if (!turn) {
                    uint8_t d = (uint8_t)((uint8_t)(R(0x40A) + 0x0C) - R(0x416));
                    turn = d < 0x18;
                }
                if (turn)
                    R(0x403 + x) ^= 0x10;
                R(0x40A + x)--;
                if (R(0x403 + x) & 0x10)
                    R(0x40A + x) = (uint8_t)(R(0x40A + x) + 2);
            }
            RockfordFootstep();
            break;
        case 2:
            break;
        case 3: /* jumping up from the panel: lands at y $47 */
            RockfordFall();
            RockfordDrift();
            x = R(0x4E);
            if (R(0x40E + x) == 0 && R(0x407 + x) >= 0x47) {
                R(0x403 + x) &= 0xF0;
                R(0x404 + x) = 0x20;
            }
            break;
        case 4: /* jumping off the top: comes down at y $D8 */
            RockfordFall();
            RockfordDrift();
            x = R(0x4E);
            if (R(0x40E + x) == 0 && R(0x407 + x) >= 0xD8) {
                R(0x403 + x) = (R(0x403 + x) & 0xF0) | 0x06;
                R(0x407 + x) = 0xD9;
            }
            break;
        case 5: /* walking in to the jump point */
            x = R(0x4E);
            RockfordWalkStep(x);
            if (R(0x40B + x) == 0 &&
                ((R(0x403 + x) & 0x10) ? R(0x40A + x) >= 0x70 : R(0x40A + x) < 0x90)) {
                R(0x403 + x) = (R(0x403 + x) & 0xF0) | 0x03;
                R(0x40C + x) = 0x80;
                R(0x40D + x) = 0xF7;
                R(0x40E + x) = 0xFF;
                Sound_Play(0x03);
                break;
            }
            RockfordFootstep();
            break;
        case 6: /* walking along the bottom */
            RockfordWalkStep(R(0x4E));
            RockfordFootstep();
            break;
        }
    }
}

void State01_Title(void)
{
    JumpTableInline(0xD7DE, ram.game_substate);
    switch (ram.game_substate) {
    case 0: /* draw the title */
        DisableRendering();
        ResetScroll();
        ClearScreen();
        ram.chr1_bank = 0x05;
        ram.chr0_bank = 0x04;
        LoadBGPalette(0xDA, 0xE2);
        LoadSpritePalette(0xDA, 0xF2);
        ram.nmi_requests = 0x01;
        FillVram1K(0x17, 0x20, 0x00);
        FillVram1K(0x17, 0x28, 0x00);
        /* the logo: 10 rows of 20 tiles from $F450 to $2206, then its attributes */
        set_zp_ptr(0x55, 0xF450);
        set_zp_ptr(0x57, 0x2206);
        for (R(0x4D) = 0x0A; R(0x4D) != 0; R(0x4D)--) {
            SetPPUAddr(R(0x58), R(0x57));
            for (uint8_t y = 0; y < 0x14; y++)
                io_write(0x2007, *ind_y(0x55, y));
            set_zp_ptr(0x55, (uint16_t)(zp_ptr(0x55) + 0x14));
            set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x57) + 0x20));
        }
        SetPPUAddr(0x23, 0xC0);
        for (uint8_t y = 0; y < 0x40; y++)
            io_write(0x2007, *ind_y(0x55, y));
        DrawPPUString(0xF5, 0x58);
        ram.oam_buf[0] = 0xD8; /* sprite 0: where the split comes */
        ram.oam_buf[1] = 0xE2;
        ram.oam_buf[2] = 0x00;
        ram.oam_buf[3] = 0x00;
        for (int x = 7; x >= 0; x--)
            R(0x3E0 + x) = prg((uint16_t)(0xDB02 + x));
        for (int x = 0; x < 0x18; x += 4) {
            R(0x3E8 + x) = 0;
            R(0x3E9 + x) = 0;
            R(0x3EA + x) = 0;
            R(0x3EB + x) = 0x02;
        }
        R(0x400) = 0x00;
        R(0x401) = 0xF0;
        R(0x402) = 0xFF;
        for (int x = 0x17; x >= 0; x--)
            R(0x403 + x) = prg((uint16_t)(0xDB22 + x));
        ram.scroll_y = 0xE0;
        RequestRenderingOn();
        EnableNMI();
        Sound_Play(0x12);
        ram.game_substate++;
        break;
    case 1: /* the logo scrolls up, then settles */
        LogoSprites();
        FallingDiamonds();
        PanelSlide();
        HideUnusedSprites();
        TitleSplit();
        if (R(0x21) == 0) {
            ram.scroll_y = (uint8_t)(ram.scroll_y - 3);
            if (ram.scroll_y >= 0x04)
                break;
            R(0x21)++;
            for (int x = 0; x < 0x18; x++)
                R(0x3E8 + x) = prg((uint16_t)(0xDB0A + x));
            R(0x400)++;
            Sound_Play(0x04);
            break;
        }
        {
            uint8_t a = ++ram.scroll_y;
            if (a < 0x48) {
                ram.scroll_y++;
                if (a < 0x38)
                    ram.scroll_y++;
            }
        }
        if (ram.scroll_y >= 0x58) {
            ram.game_substate++;
            R(0x21) = 0;
        }
        break;
    case 2: /* the panel: scores, the 1P / 2P choice */
        LogoSprites();
        PanelSlide();
        HideUnusedSprites();
        if (R(0x400) == 0) {
            TextBoxRow(0xF6, 0x21);
            if (R(0x21) == 0x01) {
                LoadScore(0x00, 0x78);
                DigitsToBuffer(0x03);
                LoadScore(0x00, 0x8A);
                DigitsToBuffer(0x0C);
                LoadScore(0x00, 0x82);
                DigitsToBuffer(0x15);
            }
            if (++R(0x21) >= 0x0D) {
                ram.play_mode = 0x90;
                ram.loop_count = 0;
                ram.world_stage = 0;
                ram.stages_cleared = 0;
                R(0x7E) = R(0x7F) = R(0x80) = 0;
                ram.lives = 3;
                R(0x81) = 3;
                R(0x88) = 0x06;
                R(0x89) = 0x06;
                ram.game_substate++;
                R(0x53) = 0;
            }
        }
        TitleSplit();
        break;
    case 3: /* waiting: Select / Up / Down choose, Start starts; idle runs the demo */
        PlayerCursor();
        LogoSprites();
        PanelSlide();
        RockfordSprites();
        HideUnusedSprites();
        RockfordsUpdate();
        TitleSplit();
        if (++R(0x21) == 0 && ++R(0x22) == 0x05) { /* the Rockfords jump in */
            R(0x403) = 0x04;
            R(0x40F) = 0x14;
            R(0x40C) = 0x00;
            R(0x418) = 0x00;
            R(0x40D) = 0xFE;
            R(0x419) = 0xFE;
            R(0x40E) = 0xFF;
            R(0x41A) = 0xFF;
            Sound_Play(0x03);
        }
        if (R(0x22) >= 0x06 && R(0x21) >= 0x63) { /* the attract demo */
            if (++R(0x73) >= 0x03)
                R(0x73) = 0;
            ram.world_stage = prg((uint16_t)(0xDB3A + R(0x73)));
            R(0x9E) = 0xFF;
            R(0x9F) = 0x00;
            Sound_Play(0x01);
            BlackoutNow();
            SetGameState(0x09);
            break;
        }
        if ((ram.joy1_pressed & 0xC0) || (ram.joy1_pressed & 0x04)) {
            R(0x53) ^= 0x01;
            R(0x21) = 0;
            Sound_Play(0x06);
        }
        if (ram.joy1_pressed & 0x08) {
            ram.play_mode = R(0x53) ? 0x30 : 0x10;
            Sound_Play(0x0C);
            for (int x = 9; x >= 0; x--) {
                R(0x74 + x) = prg((uint16_t)(0xDB3D + x));
                R(0x7E + x) = prg((uint16_t)(0xDB3D + x));
            }
            SetGameState(0x02);
        }
        break;
    }
    ram.frame_phase++;
}

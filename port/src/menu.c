/* The screens between caves, on the world map's nametable: the title menu
 * (State02: lives, COLOR SELECT), the password (State03), world select
 * (State05/06), the stage intro (State07/08), the cave setup (State09:
 * the first screenful drawn by scrolling across it) and the game over
 * screen (State0E: CONTINUE / END, with the password).
 * Faithful ports: same RAM writes and hardware accesses as the original.
 */
#include "bd.h"

/* ---- shared ---- */

/* $A773: the map screen, with rendering off, then on at the next NMI */
static void MapScreenInit(void)
{
    DisableRendering();
    ClearScreen();
    MapScreenSetup();
    ram.ppuctrl_shadow &= 0xFC;
    Sound_Play(0x13);
    RequestRenderingOn();
    EnableNMI();
}

void MapScreenSetup(void) /* $A78E */
{
    ram.camera_x[0] = ram.camera_x[1] = 0;
    ram.camera_y[0] = ram.camera_y[1] = 0;
    ram.scroll_x = ram.scroll_y = 0;
    ram.chr1_bank = 0x06;
    ram.chr0_bank = 0x04;
    LoadNametable(0, 0xF7, 0x7A);
    LoadBGPalette(0xA7, 0xCC);
    LoadSpritePalette(0xA7, 0xDC);
    ram.palette_buf[0x12] = prg((uint16_t)(0xA763 + R(0x88 + (ram.play_mode & 0x01)))); /* COLOR SELECT */
    ram.nmi_requests = 0x01;
}

void TextBoxRow(uint8_t hi, uint8_t lo)
{
    R(0x55) = lo;
    R(0x56) = hi;
    uint8_t y = (uint8_t)(R(0x21) << 1);
    R(0x59) = *ind_y(0x55, y);
    R(0x5A) = *ind_y(0x55, (uint8_t)(y + 1));
    for (int k = 0x31; k >= 0; k--)
        R(0x116 + k) = *ind_y(0x59, (uint8_t)k);
    ram.nmi_requests = 0x08;
}

/* $A7EC: the box opening, 5 rows, then the next substate */
static void TextBoxOpen(void)
{
    TextBoxRow(0xFB, 0x7A);
    if (++R(0x21) >= 0x05) {
        ram.game_substate++;
        R(0x21) = 0;
    }
}

/* $A8FF / $AFCE: the box closing, then on to state `next` (from the map) */
static void TextBoxClose(uint8_t next)
{
    TextBoxRow(0xFC, 0x75);
    if (++R(0x21) >= 0x05) {
        R(0x74 + PlayerOffset()) |= 0x80;
        SetGameState(next);
    }
}

static void ColorSelect(void) /* $AFAC-like: the player colour from COLOR SELECT */
{
    ram.palette_buf[0x12] = prg((uint16_t)(0xA763 + R(0x88 + (ram.play_mode & 0x01))));
    ram.nmi_requests = 0x01;
}

static void MapTitles(void) /* $D27E: the two title sprites of the map */
{
    SetScreenPos(0x50, 0x48);
    DrawMetaspriteScreen(0xD2, 0xA4);
    SetScreenPos(0xA0, 0xC8);
    DrawMetaspriteScreen(0xD2, 0x9B);
}

static void MapCursor(void) /* $D195: the cursor on world $4D, stage $4E (0 = the world) */
{
    ram.oam_index = 0;
    R(0x67) = (uint8_t)(R(0x4D) << 1);
    R(0x67) = (uint8_t)((R(0x67) << 2) + R(0x67));
    uint8_t x = (uint8_t)((R(0x4E) << 1) + R(0x67));
    SetScreenPos(prg((uint16_t)(0xD238 + x)), prg((uint16_t)(0xD239 + x)));
    if (ram.frame_phase & 0x08)
        DrawMetaspriteScreen(0xD1, 0xD1);
    else
        DrawMetaspriteScreen(0xD1, 0xC8);
}

static void StageMark(void) /* $D21A */
{
    if (ram.frame_phase & 0x10)
        DrawMetaspriteScreen(0xD2, 0x79);
    else
        DrawMetaspriteScreen(0xD2, 0x74);
}

static void MapStageMarks(void) /* $D1DA: a mark on each stage of the world not cleared yet */
{
    uint8_t x = PlayerOffset();
    uint8_t y = (uint8_t)((R(0x75 + x) & 0xF0) >> 3);
    R(0x59) = prg((uint16_t)(0xD22C + y));
    R(0x5A) = prg((uint16_t)(0xD22D + y));
    R(0x54) = R(0x76 + x);
    for (R(0x53) = 1; R(0x53) < 5; R(0x53)++) {
        bool cleared = R(0x54) & 1;
        R(0x54) >>= 1;
        if (cleared)
            continue;
        y = (uint8_t)(R(0x53) << 1);
        R(0x55) = *ind_y(0x59, y);
        R(0x57) = *ind_y(0x59, (uint8_t)(y + 1));
        R(0x56) = 0;
        R(0x58) = 0;
        StageMark();
    }
}

static void PasswordDigits(void) /* $D152: the 6 digits $4D-$52 as sprites */
{
    uint8_t x = 0;
    for (R(0x67) = 0; R(0x67) < 6; R(0x67)++) {
        R(0x260 + x) = 0x64;
        R(0x261 + x) = (uint8_t)((R(0x4D + R(0x67)) << 1) + 0x76);
        R(0x262 + x) = 0x02;
        R(0x263 + x) = (uint8_t)((R(0x67) << 3) + 0x68);
        x = (uint8_t)(x + 4);
    }
}

static void PasswordCursor(void) /* $D184: the digit at the cursor blinks */
{
    if (ram.frame_phase & 0x10)
        R(0x260 + (uint8_t)(R(0x53) << 2)) = 0xF0;
}

/* ---- title menu ---- */

void State02_TitleMenu(void)
{
    CopyPlayerInput();
    JumpTableInline(0xA68E, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        MapScreenInit();
        MapTitles();
        ram.game_substate++;
        break;
    case 1:
        TextBoxOpen();
        break;
    case 2: /* the box with the lives */
        TextBoxRow(0xFC, 0x34);
        if (R(0x21) == 0) {
            if (ram.play_mode & 0x01)
                R(0x119) = 0x83; /* "2" in PLAYER 2 */
            R(0x126) = (uint8_t)(R(0x77 + PlayerOffset()) + 0x81);
        }
        if (++R(0x21) >= 0x02) {
            ram.game_substate++;
            ram.player_phase = 0x21;
            ram.player_y[0] = 0x28;
            ram.player_x[0] = 0x28;
            ram.player_y[1] = 0;
            ram.player_x[1] = 0;
        }
        break;
    case 3: { /* COLOR SELECT with Left / Right (repeating), A goes on */
        uint8_t held = ram.joy_held;
        ram.joy_held = 0x40;
        PlayerSprite();
        ram.joy_held = held;
        if (R(0x21) == 0) {
            if (!(ram.joy_held & 0xC0))
                goto confirm;
            R(0x22) = (ram.joy_held & 0xC0) | 0x0F;
            R(0x21)++;
        } else {
            R(0x67) = ram.joy_held & 0xC0;
            if ((R(0x22) & 0xC0) != R(0x67)) {
                R(0x21) = 0;
                break;
            }
            R(0x22)--;
            if (R(0x22) & 0x0F)
                break;
            R(0x22) = (R(0x22) & 0xC0) | 0x0F;
        }
        {
            uint8_t x = ram.play_mode & 0x01;
            uint8_t y = (uint8_t)(R(0x88 + x) + 1);
            if (!(R(0x22) & 0x80))
                y = (uint8_t)(y - 2);
            R(0x88 + x) = y & 0x0F;
            ColorSelect();
            Sound_Play(0x06);
        }
    confirm:
        if (ram.joy_pressed & 0x01) {
            Sound_Play(0x06);
            SetGameState(0x03);
        }
        break;
    }
    }
    ram.frame_phase++;
}

/* ---- password ---- */

void State03_Password(void)
{
    CopyPlayerInput();
    JumpTableInline(0xA835, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        TextBoxRow(0xFC, 0x5C);
        ram.game_substate++;
        R(0x53) = 0;
        for (int x = 5; x >= 0; x--)
            R(0x4D + x) = 0;
        break;
    case 1: {
        ram.joy_held = 0x40;
        PlayerSprite();
        PasswordDigits();
        PasswordCursor();
        uint8_t p = ram.joy_pressed, x = R(0x53);
        bool beep = true;
        if (p & 0x80) { /* Right: next digit */
            if (++R(0x53) >= 6)
                R(0x53) = 0;
        } else if (p & 0x40) { /* Left */
            if ((int8_t)--R(0x53) < 0)
                R(0x53) = 5;
        } else if (p & 0x20) { /* Down */
            if ((int8_t)--R(0x4D + x) < 0)
                R(0x4D + x) = 9;
        } else if (p & 0x10) { /* Up */
            if (++R(0x4D + x) >= 0x0A)
                R(0x4D + x) = 0;
        } else {
            beep = false;
        }
        if (beep)
            Sound_Play(0x0A);
        if (!(ram.joy_pressed & 0x01))
            break;
        Sound_Play(0x06);
        for (R(0x67) = 0; R(0x67) < 0x18; R(0x67)++) { /* 24 passwords: 6 digits, loop, world_stage */
            uint8_t t = (uint8_t)(R(0x67) << 3), y;
            for (y = 0; y < 6; y++)
                if (R(0x4D + y) != prg((uint16_t)(0xA921 + t + y)))
                    break;
            if (y < 6)
                continue;
            x = PlayerOffset();
            R(0x74 + x) = prg((uint16_t)(0xA927 + t));
            R(0x75 + x) = prg((uint16_t)(0xA928 + t));
            R(0x76 + x) = 0;
            break;
        }
        ram.game_substate++;
        R(0x21) = 0;
        HideAllSprites();
        break;
    }
    case 2:
        TextBoxClose(0x04);
        break;
    }
    ram.frame_phase++;
}

/* ---- world select ---- */

void State05_WorldSelectInit(void)
{
    MapScreenInit();
    SetGameState(0x06);
}

/* $B6AA: the d-pad moves the cursor between stages ($B700: per world, for
 * each stage two (buttons, stage) pairs); A picks a stage not cleared yet */
static void WorldSelect_Input(void)
{
    uint8_t x = (uint8_t)(R(0x4D) << 1);
    R(0x55) = prg((uint16_t)(0xB700 + x));
    R(0x56) = prg((uint16_t)(0xB701 + x));
    uint8_t y = (uint8_t)(R(0x4E) << 2);
    uint8_t a = ram.joy_pressed & 0xF0;
    bool move = a == *ind_y(0x55, y);
    if (!move) {
        y = (uint8_t)(y + 2);
        move = a == *ind_y(0x55, y);
    }
    if (move) {
        R(0x4E) = *ind_y(0x55, (uint8_t)(y + 1));
        Sound_Play(0x06);
    }
    if (!(ram.joy_pressed & 0x01) || R(0x4E) == 0)
        return;
    x = PlayerOffset();
    if (R(0x76 + x) & prg((uint16_t)(0xB784 + R(0x4E))))
        return;
    R(0x4E)--;
    R(0x75 + x) = (R(0x75 + x) & 0xF0) | R(0x4E);
    Sound_Play(0x06);
    SetGameState(0x08);
}

void State06_WorldSelect(void)
{
    CopyPlayerInput();
    JumpTableInline(0xA9FA, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        R(0x4D) = R(0x75 + PlayerOffset()) >> 4;
        R(0x4E) = 0;
        ram.game_substate++;
        break;
    case 1:
        MapCursor();
        MapStageMarks();
        MapTitles();
        HideUnusedSprites();
        WorldSelect_Input();
        break;
    case 2:
        break;
    }
    ram.frame_phase++;
}

/* ---- stage intro ---- */

void State07_StageIntroInit(void)
{
    MapScreenInit();
    SetGameState(0x08);
}

void State08_StageIntro(void)
{
    CopyPlayerInput();
    JumpTableInline(0xAA3E, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        TextBoxOpen();
        break;
    case 1: /* the box: lives, then world and stage */
        TextBoxRow(0xFD, 0x2F);
        if (R(0x21) == 0) {
            if (ram.play_mode & 0x01)
                R(0x119) = 0x83;
            R(0x126) = (uint8_t)(R(0x77 + PlayerOffset()) + 0x81);
        } else {
            uint8_t x = PlayerOffset();
            R(0x67) = (uint8_t)((R(0x74 + x) & 0x03) << 2);
            R(0x67) = (uint8_t)((R(0x67) << 1) + R(0x67));
            uint8_t y = (uint8_t)(((R(0x75 + x) >> 4) << 1) + R(0x67));
            R(0x121) = prg((uint16_t)(0xAAE4 + y));
            R(0x122) = prg((uint16_t)(0xAAE5 + y));
            R(0x126) = (uint8_t)((R(0x75 + x) & 0x03) + 0x82);
        }
        if (++R(0x21) >= 0x02) {
            ram.game_substate++;
            R(0x21) = 0;
            HideAllSprites();
            ram.player_phase = 0;
            ram.player_y[0] = 0x28;
            ram.player_x[0] = 0x28;
            ram.player_y[1] = 0;
            ram.player_x[1] = 0;
            ram.joy_repeat_timer = 0;
            uint8_t x = PlayerOffset();
            R(0x4D) = R(0x75 + x) >> 4;
            R(0x4E) = (uint8_t)((R(0x75 + x) & 0x0F) + 1);
            MapCursor();
            MapStageMarks();
            MapTitles();
        }
        break;
    case 2: /* A starts the cave */
        ram.joy_held = 0;
        PlayerSprite();
        if (ram.joy_pressed & 0x01) {
            BlackoutNow();
            SetGameState(0x09);
        }
        break;
    }
    ram.frame_phase++;
}

/* ---- cave setup ---- */

/* $BB8A: the first screenful, drawn by scrolling the camera across it from
 * 256 pixels to the left (the camera's own column strips), then the palettes */
static void CaveFirstScreen(void)
{
    ram.chr0_bank = 0x04;
    MMC1_SetCHR0(0x04);
    ram.chr1_bank = 0x00;
    MMC1_SetCHR1(0x00);
    uint8_t t = (uint8_t)((ram.cave_index << 3) + ram.cave_index);
    ram.camera_y[0] = prg((uint16_t)(0xBC92 + t));
    ram.camera_y[1] = prg((uint16_t)(0xBC93 + t));
    ram.camera_x[0] = prg((uint16_t)(0xBC94 + t));
    ram.camera_x[1] = prg((uint16_t)(0xBC95 + t));
    uint8_t end = ram.camera_x[1];
    ram.camera_x[1]--;
    R(0xC5) = 0;
    R(0xC6) = 0;
    do {
        if (R(0xC5) == 0) {
            R(0xC5) = 0x08;
            R(0xC6) = 0;
            R(0xC3) = ram.camera_x[0];
            Camera_FillStrip();
        }
        Camera_Scroll();
        ram.scroll_x = ram.camera_x[0];
        ram.scroll_y = ram.camera_y[0];
        if (ram.nmi_requests & 0x30) {
            ram.nmi_requests = 0;
            Nmi_FlushMetatiles4();
        }
    } while (end != ram.camera_x[1]);
    uint8_t x = (uint8_t)((ram.cave_index >> 2) << 1);
    LoadBGPalette(prg((uint16_t)(0xBC17 + x)), prg((uint16_t)(0xBC16 + x)));
    LoadSpritePalette(0xBC, 0x82);
    ram.palette_buf[0x12] = prg((uint16_t)(0xA763 + R(0x88 + (ram.play_mode & 0x01))));
}

void State09_CaveSetup(void)
{
    ram.nmi_ppu_enable = 0;
    ram.mmc1_ctrl_shadow = 0x10;
    MMC1_SetCtrl(0x10);
    ClearScreen();
    LoadCave();
    ApplyCavePatches();
    R(0xC4) = 0;
    CaveFirstScreen();
    ram.nmi_ppu_enable = 0xFF;
    EnableNMI();
    RequestRenderingOn();
    FadeIn();
    SetGameState(0x0A);
}

/* ---- game over ---- */

static void GameOverCursor(void) /* $D137: CONTINUE / END */
{
    R(0x280) = R(0x53) ? 0x48 : 0x40;
    R(0x281) = 0x8E;
    R(0x282) = 0x02;
    R(0x283) = 0x58;
}

void State0E_GameOver(void)
{
    CopyPlayerInput();
    JumpTableInline(0xAEAB, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        MapScreenInit();
        ram.game_substate++;
        break;
    case 1:
        TextBoxOpen();
        break;
    case 2: /* the box, with the password of this world */
        TextBoxRow(0xFD, 0x57);
        if (R(0x21) == 0 && (ram.play_mode & 0x01))
            R(0x119) = 0x83;
        if (++R(0x21) >= 0x02) {
            uint8_t x = PlayerOffset();
            R(0x67) = (uint8_t)((R(0x74 + x) & 0x03) << 4);
            R(0x67) = (uint8_t)((R(0x67) << 1) + R(0x67));
            uint8_t y = (uint8_t)(((R(0x75 + x) & 0xF0) >> 1) + R(0x67));
            for (x = 0; x < 6; x++, y++)
                R(0x4D + x) = prg((uint16_t)(0xA921 + y));
            PasswordDigits();
            x = PlayerOffset();
            R(0x4D) = R(0x75 + x) >> 4;
            R(0x4E) = 0;
            MapCursor();
            MapStageMarks();
            MapTitles();
            ram.game_substate++;
            R(0x53) = 0;
        }
        break;
    case 3: /* Up / Down choose, A confirms */
        GameOverCursor();
        if (ram.joy_pressed & 0x30) {
            R(0x53) ^= 0x01;
            Sound_Play(0x06);
        }
        if (!(ram.joy_pressed & 0x01))
            return;
        if (R(0x53)) { /* END: this player is out */
            Sound_Play(0x11);
            WaitFrames(0x80);
            uint8_t x = ram.play_mode & 0x01;
            ram.play_mode &= prg((uint16_t)(0xEEA2 + x));
            if (!(ram.play_mode & 0x30)) {
                BlackoutNow();
                SetGameState(0x01);
                return;
            }
            ram.play_mode ^= 0x01; /* the other player goes on */
        } else { /* CONTINUE: the world again from its first stage */
            Sound_Play(0x0C);
            uint8_t x = PlayerOffset();
            R(0x74 + x) |= 0x40;
            R(0x75 + x) &= 0xF0;
            R(0x76 + x) &= 0xF0;
            R(0x77 + x) = 0x03;
            ResetPlayerScore(x);
            if ((ram.play_mode & 0x30) == 0x30)
                ram.play_mode ^= 0x01;
        }
        R(0x74 + PlayerOffset()) &= 0xBF;
        ColorSelect();
        ram.game_substate++;
        R(0x21) = 0;
        return;
    case 4:
        TextBoxClose(0x06);
        return;
    }
    ram.frame_phase++;
}

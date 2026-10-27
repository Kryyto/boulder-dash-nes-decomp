/* The cave states, one frame per call, as the main loop runs them:
 * State0A_CaveIntro ($AB5E: the camera pans across the cave) and
 * State0B_CavePlay ($AB76: play, the pause box, the time-up box, the score
 * popups), plus BuildMetatileQueue ($CA4D) and the popup sprites.
 * Faithful ports: same RAM writes as the original.
 */
#include "bd.h"

/* ---- metasprites in screen coordinates ($A5AA) ---- */

void DrawMetaspriteScreen(uint8_t ptr_hi, uint8_t ptr_lo)
{
    R(0x12) = ptr_hi;
    R(0x11) = ptr_lo;
    uint8_t x = ram.oam_index, y = 0;
    for (;;) {
        uint8_t dy = rd((uint16_t)(zp_ptr(0x11) + y));
        if (dy == 0x7F) {
            ram.oam_index = x;
            return;
        }
        R(0x200 + x) = (uint8_t)(dy + R(0x55));
        y++;
        R(0x201 + x) = rd((uint16_t)(zp_ptr(0x11) + y));
        y++;
        R(0x202 + x) = rd((uint16_t)(zp_ptr(0x11) + y));
        y++;
        R(0x203 + x) = (uint8_t)(rd((uint16_t)(zp_ptr(0x11) + y)) + R(0x57));
        y++;
        x = (uint8_t)(x + 4);
    }
}

void SetScreenPos(uint8_t y, uint8_t x) /* $E996 */
{
    R(0x55) = y;
    R(0x57) = x;
    R(0x56) = 0;
    R(0x58) = 0;
}

static uint16_t word_at(uint16_t a) { return (uint16_t)(prg(a) | prg((uint16_t)(a + 1)) << 8); }

/* ---- popups ---- */

static void DrawBox(uint16_t sprite) /* $D693: pause / time-up box at $5F-$62 (world) */
{
    R(0x55) = R(0x5F);
    R(0x56) = R(0x60);
    R(0x57) = R(0x61);
    R(0x58) = R(0x62);
    ram.oam_index = 0x60;
    DrawMetaspriteWorld(0x00, (uint8_t)(sprite >> 8), (uint8_t)sprite);
}

static void ExtraLifePopup(void) /* $D6CE: rises, palette cycles */
{
    if (!R(0xA0))
        return;
    SetScreenPos(R(0xA1), R(0xA2));
    uint8_t x = (uint8_t)((ram.frame_phase & 0x18) >> 2);
    uint16_t s = word_at((uint16_t)(0xD6EF + x));
    DrawMetaspriteScreen((uint8_t)(s >> 8), (uint8_t)s);
    R(0xA1)++;
    R(0xA0)--;
}

static void ExitOpenPopup(void) /* $D71E */
{
    if (!R(0xA3))
        return;
    SetScreenPos(R(0xA4), R(0xA5));
    DrawMetaspriteScreen(0xD7, 0x35);
    R(0xA4)++;
    R(0xA3)--;
}

static void EnemyScorePopup(void) /* $D746: the points, drifting up */
{
    if (!R(0xA6))
        return;
    R(0x55) = R(0xA7);
    R(0x56) = R(0xA8);
    R(0x57) = R(0xA9);
    R(0x58) = R(0xAA);
    uint8_t x = (uint8_t)((ram.cave_index >> 2) << 1);
    uint16_t s = word_at((uint16_t)(0xD771 + x));
    DrawMetaspriteWorld(0x01, (uint8_t)(s >> 8), (uint8_t)s);
    R(0xA7)--;
    R(0xA6)--;
}

/* ---- BuildMetatileQueue ($CA4D) ---- */

/* $D0F2: next cell of the redraw list; false when it's empty */
static bool DequeueCellRedraw(uint8_t *tile, uint8_t *row, uint8_t *col)
{
    if (!ram.redraw_count)
        return false;
    uint8_t x = (uint8_t)(R(0x752) * 3);
    R(0x67) = R(0x753 + x);
    *row = R(0x754 + x);
    *col = R(0x755 + x);
    ram.redraw_count--;
    if (++R(0x752) >= 0x37)
        R(0x752) = 0;
    *tile = R(0x67);
    return true;
}

/* Is the dequeued cell ($4F row, $50 col) on screen? In nametable cells
 * (row + 2, col + 4) against the camera ($51 rows, $52 cols). While the
 * camera scrolls, the edge it is drawing is left out. The comparisons are
 * made in the original's order, leaving $67 as it does (the last bound it
 * stored; still the tile from the dequeue if the first test fails). */
static bool CellOnScreen(void)
{
    uint8_t y = (uint8_t)(R(0x4F) + 2), x = (uint8_t)(R(0x50) + 4);
    uint8_t r0 = R(0x51), c0 = R(0x52);
    switch (R(0xC5) & 0x0F) {
    case 0x08: /* scrolling right */
        if (y < r0) return false;
        R(0x67) = (uint8_t)(r0 + 0x0F);
        if (y >= R(0x67)) return false;
        R(0x67) = (uint8_t)(c0 + 1);
        if (x < R(0x67)) return false;
        R(0x67) = (uint8_t)(c0 + 0x11);
        return x < R(0x67);
    case 0x02: /* scrolling down */
        R(0x67) = (uint8_t)(r0 + 1);
        if (y < R(0x67)) return false;
        R(0x67) = (uint8_t)(r0 + 0x10);
        if (y >= R(0x67)) return false;
        if (x < c0) return false;
        R(0x67) = (uint8_t)(c0 + 0x10);
        return x < R(0x67);
    default: /* still, up or left */
        if (y < r0) return false;
        R(0x67) = (uint8_t)(r0 + 0x0F);
        if (y >= R(0x67)) return false;
        if (x < c0) return false;
        R(0x67) = (uint8_t)(c0 + 0x10);
        return x < R(0x67);
    }
}

static uint8_t camera_cell(uint8_t lo, uint8_t hi) /* (16-bit >> 4) & $FF */
{
    return (uint8_t)(((uint16_t)(lo | hi << 8)) >> 4);
}

void BuildMetatileQueue(void)
{
    if (ram.nmi_requests & 0x10) /* the camera queued a strip this frame */
        return;
    for (int i = 0x35; i >= 0; i--)
        R(0xC7 + i) = 0xFF;
    R(0x51) = camera_cell(ram.camera_y[0], ram.camera_y[1]);
    R(0x52) = camera_cell(ram.camera_x[0], ram.camera_x[1]);
    for (R(0x4D) = 0; R(0x4D) < 6; R(0x4D)++) {
        uint8_t tile, row, col;
        if (!DequeueCellRedraw(&tile, &row, &col)) {
            if (R(0x4D) == 0)
                return;
            break;
        }
        R(0x4E) = tile;
        R(0x4F) = row;
        R(0x50) = col;
        if (!CellOnScreen())
            continue;
        /* $CB9D: queue the metatile and its attribute */
        uint8_t t2 = (uint8_t)((uint8_t)(R(0x4F) + 2) << 1);
        uint16_t addr = (uint16_t)(word_at((uint16_t)(0xCC26 + t2)) + (uint8_t)(((R(0x50) + 4) & 0x0F) << 1));
        R(0x57) = (uint8_t)addr;
        R(0x58) = (uint8_t)(addr >> 8);
        R(0x50) = prg((uint16_t)(0xF440 + (R(0x4E) >> 4)));
        Camera_AttributeMerge(R(0x57), R(0x58));
        uint8_t q = (uint8_t)(R(0x4D) * 6);
        R(0x67) = (uint8_t)(R(0x4D) << 1);
        R(0xC7 + q) = R(0x57);
        R(0xC8 + q) = R(0x58);
        R(0x69) = R(0x4E);
        uint8_t type = R(0x4E) & 0xF0;
        set_zp_ptr(0x67, word_at((uint16_t)(0xF370 + (type >> 3))));
        Camera_MetatileVariant(type);
        for (uint8_t k = 0; k < 4; k++)
            R(0xC9 + q + k) = rd((uint16_t)(zp_ptr(0x67) + k));
        uint8_t a = (uint8_t)(R(0x4D) * 3);
        R(0xEB + a) = R(0x55);
        R(0xEC + a) = R(0x56);
        R(0xED + a) = R(0x50);
    }
    ram.nmi_requests = 0x20;
}

/* ---- end of a play frame ($AC7C / $AC98) ---- */

static void EndFrameTimers(void) /* $AC7C */
{
    ram.chr1_bank = (uint8_t)((ram.frame_phase & 0x18) >> 3); /* animates tiles */
    for (int i = 7; i >= 0; i--)
        if (ram.explosion_timers[i])
            ram.explosion_timers[i]--;
    if (R(0xB6))
        R(0xB6)--;
}

static void EndFrame(void) /* $AC98 */
{
    ram.nmi_ppu_enable = 0xFF;
    ram.frame_phase++;
}

/* $BF07: the attract demo's buttons: (buttons, duration in 8-frame units) pairs */
static void DemoInput(void)
{
    set_zp_ptr(0x55, word_at((uint16_t)(0xBF36 + (uint8_t)(R(0x73) << 1))));
    if (R(0x9F)) {
        ram.joy_held = *ind_y(0x55, (uint8_t)(R(0x9E) << 1));
        if (!(ram.frame_phase & 0x07))
            R(0x9F)--;
        return;
    }
    R(0x9E)++;
    R(0x9F) = *ind_y(0x55, (uint8_t)((R(0x9E) << 1) + 1));
}

static void ShiftBox(uint8_t zp, int delta) /* 16-bit add to a box coordinate */
{
    set_zp_ptr(zp, (uint16_t)(zp_ptr(zp) + delta));
}

void State0B_CavePlay(void)
{
    ram.nmi_ppu_enable = 0;
    UpdateStatusSprites();
    PlayerSprite();
    ExtraLifePopup();
    EnemyScorePopup();
    ExitOpenPopup();
    HideUnusedSprites();
    JumpTableInline(0xAB91, ram.pause_state);
    switch (ram.pause_state) {
    case 0: /* playing */
        CopyPlayerInput();
        if (ram.play_mode & 0x80) { /* attract demo */
            if (ram.joy1_pressed & 0x08) {
                ram.nmi_ppu_enable = 0xFF;
                SetGameState(0x01);
                return;
            }
            DemoInput();
        }
        if (ram.joy1_pressed & 0x08) { /* Start: pause, the box slides in */
            ram.pause_state++;
            R(0x71) = 0;
            ram.game_substate = 0x0A;
            Sound_Play(0x19);
            set_zp_ptr(0x5F, (uint16_t)(zp_ptr(0xC0) + 0x80));
            set_zp_ptr(0x61, (uint16_t)(zp_ptr(0xBE) + 0x120));
            break;
        }
        PlayerHazards();
        ScanCave();
        PlayerInput();
        MagicWallTimer();
        AmoebaUpdate();
        TickTime();
        AmoebaSound();
        UpdateCamera();
        BuildMetatileQueue();
        break;
    case 1: /* paused */
        DrawBox(0xD65A);
        HideUnusedSprites();
        JumpTableInline(0xAC0E, R(0x71));
        switch (R(0x71)) {
        case 0: /* slide in */
            ShiftBox(0x61, -0x10);
            if (--ram.game_substate == 0)
                R(0x71)++;
            break;
        case 1: /* wait for Start */
            if (!(ram.joy1_pressed & 0x08))
                break;
            R(0x71)++;
            ram.game_substate = 0x0A;
            Sound_Play(0x1A);
            /* fall through */
        case 2: /* slide out */
            ShiftBox(0x61, -0x10);
            if (--ram.game_substate == 0)
                ram.pause_state = 0;
            break;
        }
        EndFrame();
        return;
    case 2: /* time up */
        DrawBox(0xD676);
        HideUnusedSprites();
        PlayerInput();
        BuildMetatileQueue();
        if (R(0x71) == 0) {
            ShiftBox(0x5F, +0x10);
            if (--ram.game_substate == 0)
                R(0x71)++;
        }
        break;
    }
    EndFrameTimers();
    EndFrame();
}

/* ---- cave intro ---- */

/* $BE3A: one camera step in direction $0750+x of the pan */
static void IntroStep(uint8_t x)
{
    if (!R(0xC5)) {
        uint8_t d = R(0x750 + x);
        R(0xC5) = prg((uint16_t)(0xBE76 + d));
        R(0xC6) = 0;
        R(0xC2) = ram.camera_y[0];
        R(0xC3) = ram.camera_x[0];
        if ((R(0x750 + x) & 0x02) && (R(0x750 + x) & 0x01))
            R(0xC3) = (uint8_t)(ram.camera_x[0] - 0x10);
        Camera_FillStrip();
    }
    Camera_Scroll();
    ram.scroll_x = ram.camera_x[0];
    ram.scroll_y = ram.camera_y[0];
}

/* the camera reached the leg's target ($0750+dir_at: direction, then target) */
static void CheckLeg(uint16_t dir_at, uint16_t target)
{
    uint8_t y = ram.camera_y[0], x = ram.camera_y[1];
    if (R(dir_at) & 0x02) {
        y = ram.camera_x[0];
        x = ram.camera_x[1];
    }
    if (y == R(target) && x == R((uint16_t)(target + 1)))
        ram.game_substate++;
}

static void PlacePlayer(void) /* $BE7A: the player at its start cell, position to pixels */
{
    ram.player_phase = 0;
    ram.player_phase_timer = 0;
    SetCell(0xE0, ram.player_y[0], ram.player_x[0]);
    uint16_t py = (uint16_t)((ram.player_y[0] | ram.player_y[1] << 8) << 4);
    uint16_t px = (uint16_t)((ram.player_x[0] | ram.player_x[1] << 8) << 4);
    ram.player_y[0] = (uint8_t)py; ram.player_y[1] = (uint8_t)(py >> 8);
    ram.player_x[0] = (uint8_t)px; ram.player_x[1] = (uint8_t)(px >> 8);
    ram.joy_repeat_timer = 0;
    ram.giveup_counter = 0;
}

static void IntroPan(void) /* $BD6A */
{
    R(0x4D) = (uint8_t)(ram.cave_index * 9);
    JumpTableInline(0xBD79, ram.game_substate);
    uint16_t t = (uint16_t)(0xBC92 + R(0x4D)); /* + 4: legs */
    switch (ram.game_substate) {
    case 0: /* set up the two legs */
        ram.redraw_count = 0;
        ram.redraw_index = prg((uint16_t)(t + 4)) >> 4;
        R(0x752) = prg((uint16_t)(t + 5));
        R(0x753) = prg((uint16_t)(t + 6));
        R(0x754) = prg((uint16_t)(t + 4)) & 0x0F;
        R(0x755) = prg((uint16_t)(t + 7));
        R(0x756) = prg((uint16_t)(t + 8));
        ram.game_substate++;
        break;
    case 1:
        IntroStep(1);
        CheckLeg(0x751, 0x752);
        break;
    case 2:
        IntroStep(4);
        CheckLeg(0x754, 0x755);
        break;
    case 3: /* $BDF7: start playing */
        set_zp_ptr(0x55, 0x0750); /* $D124: clear $0750-$07FF */
        do {
            *ind_y(0x55, 0) = 0;
            R(0x55)++;
        } while (R(0x55) != 0);
        PlacePlayer();
        R(0xA0) = 0;
        R(0xA6) = 0;
        R(0xA3) = 0;
        for (int i = 0;; i++) {
            ram.oam_buf[i] = prg((uint16_t)(0xBEB0 + i));
            if (ram.oam_buf[i] == 0xFF)
                break;
        }
        if (ram.cave_index == 0x06 || ram.cave_index == 0x0C)
            Sound_Play(0x0B);
        if (!(ram.play_mode & 0x80))
            Sound_Play(prg((uint16_t)(0xBF01 + (ram.cave_index >> 2))));
        ram.frame_phase = 0;
        SetGameState(0x0B);
        break;
    }
}

void State0A_CaveIntro(void)
{
    if ((ram.play_mode & 0x80) && (ram.joy1_pressed & 0x08)) { /* demo: Start goes to the title */
        SetGameState(0x01);
        return;
    }
    IntroPan();
}

/* ---- cave cleared ($ACA1) ---- */

static void BonusSprite(void) /* $D5E2: the time bonus rising from the exit */
{
    uint8_t attr = prg((uint16_t)(0xD612 + ((ram.frame_phase & 0x18) >> 3)));
    uint16_t s = word_at((uint16_t)(0xD616 + (uint8_t)(R(0x54) << 1)));
    R(0x55) = R(0x5F);
    R(0x56) = R(0x60);
    R(0x57) = R(0x61);
    R(0x58) = R(0x62);
    ram.oam_index = 0x58;
    DrawMetaspriteWorld(attr, (uint8_t)(s >> 8), (uint8_t)s);
}

/* time left <= the 3-digit threshold at $ADEA+x (ones, tens, hundreds)? */
static bool TimeAtMost(uint8_t x)
{
    const uint8_t *t = ram.time_left;
    for (int d = 2; d >= 0; d--) {
        uint8_t th = prg((uint16_t)(0xADEA + x + d));
        if (t[d] != th)
            return t[d] < th;
    }
    return true;
}

void State0C_CaveCleared(void)
{
    UpdateStatusSprites();
    PlayerSprite();
    ExtraLifePopup();
    HideUnusedSprites();
    PlayerInput(); /* finishes the step into the exit (phase 4) */
    UpdateCamera();
    BuildMetatileQueue();
    JumpTableInline(0xACBB, ram.game_substate);
    switch (ram.game_substate) {
    case 0: /* once in the exit: the bonus tier from the time left */
        if ((ram.player_phase & 0x0F) == 0x04) {
            R(0x54) = 0;
            for (uint8_t x = 0; x < 6; x = (uint8_t)(x + 3)) {
                if (!TimeAtMost(x))
                    break;
                R(0x54)++;
            }
            set_zp_ptr(0x5F, (uint16_t)((ram.player_y[0] | ram.player_y[1] << 8) + 0x2F));
            set_zp_ptr(0x61, (uint16_t)((ram.player_x[0] | ram.player_x[1] << 8) + 0x48));
            Sound_Play(0x18);
            ram.game_substate++;
        }
        break;
    case 1: /* the bonus sprite rises for 24 steps */
        BonusSprite();
        HideUnusedSprites();
        if (ram.frame_phase & 0x01) {
            set_zp_ptr(0x5F, (uint16_t)(zp_ptr(0x5F) - 1));
            if (++R(0x21) >= 0x18) {
                ram.player_phase++;
                ram.game_substate++;
                goto animate;
            }
        }
        break;
    case 2: /* count the time left into the score */
        if ((ram.time_left[0] | ram.time_left[1] | ram.time_left[2]) == 0) {
            ram.game_substate++;
            R(0x21) = 0;
        } else if (!(ram.frame_phase & 0x01)) {
            DecrementTime();
            AddScore(1);
            if (!(ram.frame_phase & 0x02))
                Sound_Play(0x0A);
        }
        goto animate;
    case 3: /* wait, fade out, mark the stage cleared */
        if (++R(0x21) < 0x80)
            break;
        FadeOut();
        if (ram.play_mode & 0x80) { /* attract demo: back to the title */
            SetGameState(0x01);
            return;
        }
        BlackoutNow();
        {
            uint8_t x = PlayerOffset();
            uint8_t *ws = &R(0x75 + x), *cleared = &R(0x76 + x);
            *cleared |= prg((uint16_t)(0xADF0 + (*ws & 0x03)));
            uint8_t next = 0x05; /* back to the world map */
            if ((*cleared & 0x0F) == 0x0F) { /* the whole world: next world */
                next = 0x04;
                uint8_t w = (uint8_t)(*ws + 0x10);
                *ws = w;
                *cleared = 0;
                if (w >= 0x60) { /* after world 6: the next loop, and the ending */
                    *ws = 0;
                    R(0x74 + x)++;
                    R(0x408) = 0;
                    if ((R(0x74 + x) & 0x07) >= 0x04)
                        R(0x408)++;
                    next = 0x0F;
                }
            }
            SetGameState(next);
        }
        return;
    }
    ram.frame_phase++;
    return;
animate:
    ram.chr1_bank = (uint8_t)((ram.frame_phase & 0x0C) >> 2);
    ram.frame_phase++;
}

/* ---- lost a life ($ADF4) ---- */

void State0D_LostLife(void)
{
    JumpTableInline(0xADF9, ram.game_substate);
    switch (ram.game_substate) {
    case 0: {
        uint8_t x = PlayerOffset();
        if ((int8_t)--R(0x77 + x) < 0) { /* no lives left: GAME OVER box */
            set_zp_ptr(0x5F, (uint16_t)(zp_ptr(0xC0) + 0x120));
            set_zp_ptr(0x61, (uint16_t)(zp_ptr(0xBE) + 0x80));
            R(0x22) = PlayerOnBoxRow() ? 0x09 : 0x0A;
            ram.game_substate = (uint8_t)(ram.game_substate + 2);
            return;
        }
        if ((ram.play_mode & 0x30) == 0x30) /* two players: the other one's turn */
            ram.play_mode ^= 0x01;
        x = PlayerOffset();
        R(0x4D) = (R(0x74 + x) & 0x80) ? 0x05 : 0x02; /* next state: map or lives screen */
        R(0x74 + x) &= 0xBF;
        ram.game_substate++;
        return;
    }
    case 1: /* wait, then fade out */
        if (!(++R(0x21) & 0x80))
            return;
        SetGameState(R(0x4D));
        FadeOut();
        BlackoutNow();
        return;
    case 2: /* GAME OVER: the box comes down, wait, fade out */
        DrawBox(0xD6AD);
        if (R(0x22)) {
            R(0x22)--;
            set_zp_ptr(0x5F, (uint16_t)(zp_ptr(0x5F) - 0x10));
        }
        if (++R(0x21) & 0x80) {
            FadeOut();
            BlackoutNow();
            R(0x76 + PlayerOffset()) &= 0xF0;
            SetGameState(0x0E);
        }
        return;
    }
}

/* After a world: State04_NextStage ($A9E1 -> $DF6D, the world's cutscene
 * with its typed text, then the screen scrolls over to the map), and after
 * the last one: State0F ($EABB: the ending, Rockford jumping about),
 * State10 ($EC82) and State11 ($ED09: the credits, a scene per world),
 * State12 ($EFF5: "try the next level" with the password).
 * The cutscene number is kept in cave_grid[0] ($03E0), the scene's objects
 * after it ($03E8..), the text typer's state at $03E1-$03E7.
 * Faithful ports: same RAM writes and hardware accesses as the original.
 */
#include "bd.h"

#define SCENE R(0x3E0)

static void add16(uint16_t lo_addr, uint16_t value)
{
    uint16_t v = (uint16_t)((R(lo_addr) | R((uint16_t)(lo_addr + 1)) << 8) + value);
    R(lo_addr) = (uint8_t)v;
    R((uint16_t)(lo_addr + 1)) = (uint8_t)(v >> 8);
}

/* (hi, lo) of a pointer in a ROM table of words */
#define TABLE_PTR(base, i) prg((uint16_t)((base) + (i) + 1)), prg((uint16_t)((base) + (i)))

/* ---- helpers ---- */

/* $E942: like DrawMetaspriteScreen, but behind the background unless inside
 * the window x $40-$BF, y $30-$8F */
static void DrawMetaspriteWindow(uint8_t hi, uint8_t lo)
{
    R(0x12) = hi;
    R(0x11) = lo;
    uint8_t x = ram.oam_index, y = 0;
    for (;;) {
        uint8_t dy = rd((uint16_t)(zp_ptr(0x11) + y));
        if (dy == 0x7F) {
            ram.oam_index = x;
            return;
        }
        uint8_t sy = (uint8_t)(dy + R(0x55));
        R(0x200 + x) = sy;
        R(0x10) = sy;
        y++;
        R(0x201 + x) = rd((uint16_t)(zp_ptr(0x11) + y));
        y++;
        R(0x202 + x) = rd((uint16_t)(zp_ptr(0x11) + y)) | 0x20;
        y++;
        uint8_t sx = (uint8_t)(rd((uint16_t)(zp_ptr(0x11) + y)) + R(0x57));
        R(0x203 + x) = sx;
        if (sx < 0xC0 && sx >= 0x40 && R(0x10) < 0x90 && R(0x10) >= 0x30)
            R(0x202 + x) &= 0xDF;
        y++;
        x = (uint8_t)(x + 4);
    }
}

static void DrawAtActor(uint8_t hi, uint8_t lo) /* $E791: at x = $03F3 (y in $55) */
{
    R(0x57) = R(0x3F3);
    R(0x56) = 0;
    R(0x58) = 0;
    DrawMetaspriteWindow(hi, lo);
}

static void DrawActor(uint8_t hi, uint8_t lo) /* $E78D: y $80, x = $03F3 */
{
    R(0x55) = 0x80;
    DrawAtActor(hi, lo);
}

/* $E853: every 4th frame, one character of the text at $03E2 into the
 * VRAM buffer at $03E6 (FC lo hi = jump, FD = next line, FE lo hi = new
 * position, FF = done: $03E1 set) */
static void TypeText(void)
{
    if (R(0x3E1) || (ram.frame_phase & 0x03))
        return;
    R(0x55) = R(0x3E2);
    R(0x56) = R(0x3E3);
    uint8_t y = 0, a = *ind_y(0x55, 0);
    switch (a) {
    case 0xFC:
        R(0x3E2) = *ind_y(0x55, 1);
        R(0x3E3) = *ind_y(0x55, 2);
        return;
    case 0xFD:
        add16(0x3E4, 0x20);
        R(0x3E6) = R(0x3E4);
        R(0x3E7) = R(0x3E5);
        y = 1;
        break;
    case 0xFE:
        R(0x3E4) = R(0x3E6) = *ind_y(0x55, 1);
        R(0x3E5) = R(0x3E7) = *ind_y(0x55, 2);
        y = 3;
        break;
    case 0xFF:
        R(0x3E1)++;
        return;
    default:
        R(0x119) = a;
        R(0x118) = 0x01;
        R(0x11A) = 0xFF;
        R(0x116) = R(0x3E6);
        R(0x117) = R(0x3E7);
        ram.nmi_requests = 0x08;
        y = 1;
        add16(0x3E6, 1);
        break;
    }
    add16(0x3E2, y);
}

static uint8_t pressed_a_or_b(void) /* $E012 (leaves X = the current player) */
{
    return R(0x29 + (ram.play_mode & 0x01)) & 0x03;
}

/* $E9A1: the scene's frame: the second nametable, its text from CHR ROM */
static void SceneFrame(void)
{
    FillVram1K(0x80, 0x24, 0x00);
    FillVram64(0xFF, 0x27, 0xC0);
    ReadVramTo600(0x07, 0x1D, 0xC0);
    DrawPPUString(0x06, 0x00);
}

static void ScenePalette(void) /* $EA15: the scene's sprite palette (keeps COLOR SELECT) */
{
    uint8_t keep = ram.palette_buf[0x12];
    uint8_t x = (uint8_t)(SCENE << 1);
    LoadSpritePalette(TABLE_PTR(0xEA7F, x));
    ram.palette_buf[0x12] = keep;
    ram.nmi_requests = 0x01;
}

/* $E9C4: the scene's picture and attributes from CHR ROM */
static void ScenePicture(void)
{
    uint8_t x = (uint8_t)(SCENE << 2);
    ReadVramTo600(prg((uint16_t)(0xEA5B + x)), prg((uint16_t)(0xEA5E + x)), prg((uint16_t)(0xEA5D + x)));
    DrawPPUString(0x06, 0x00);
    x = (uint8_t)(SCENE << 1);
    ReadVramTo600(0x03, prg((uint16_t)(0xEA74 + x)), prg((uint16_t)(0xEA73 + x)));
    /* $E9FB: 32 attribute bytes from $0600 to $27C8 */
    R(0x55) = 0x00;
    R(0x56) = 0x06;
    SetPPUAddr(0x27, 0xC8);
    for (uint8_t y = 0; y < 0x20; y++)
        io_write(0x2007, *ind_y(0x55, y));
    RestorePPUCtrlScroll();
    ScenePalette();
}

static void SceneObjects(void) /* $EA2F: the scene's object table to $03E8.. ($FF ends) */
{
    uint8_t x = (uint8_t)(SCENE << 1);
    R(0x55) = prg((uint16_t)(0xE06A + x));
    R(0x56) = prg((uint16_t)(0xE06B + x));
    R(0x57) = prg((uint16_t)(0xE05E + x));
    R(0x58) = prg((uint16_t)(0xE05F + x));
    for (uint8_t y = 0;; y++) {
        uint8_t a = *ind_y(0x55, y);
        if (a == 0xFF)
            break;
        *ind_y(0x57, y) = a;
    }
    ram.oam_index = 0;
}

static void StartText(uint16_t text, uint8_t lo) /* the typer at text, to $26:lo */
{
    R(0x3E1) = 0;
    R(0x3E2) = (uint8_t)text;
    R(0x3E3) = (uint8_t)(text >> 8);
    R(0x3E4) = R(0x3E6) = lo;
    R(0x3E5) = R(0x3E7) = 0x26;
}

static void SceneOn(void) /* the common end of a scene setup */
{
    ram.ppuctrl_shadow |= 0x03;
    RequestRenderingOn();
    EnableNMI();
    ram.nmi_ppu_enable = 0xFF;
    FadeIn();
}

static void SceneScreenInit(void) /* nmi off, MMC1 $12, the map screen's base */
{
    ram.nmi_ppu_enable = 0;
    ram.mmc1_ctrl_shadow = 0x12;
    MMC1_SetCtrl(0x12);
    ClearScreen();
    MapScreenSetup();
}

/* ---- the six world scenes ($E0D7) ---- */

static void TitleLetters(void) /* $E128: the four logo pieces passing by */
{
    for (R(0x4D) = 3; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = (uint8_t)(R(0x4D) << 1);
        SetScreenPos(prg((uint16_t)(0xE16F + x)), R(0x3E9 + x));
        DrawMetaspriteWindow(TABLE_PTR(0xDBAE, x));
    }
    for (R(0x67) = 3; !(R(0x67) & 0x80); R(0x67)--) {
        uint8_t x = R(0x67);
        add16((uint16_t)(0x3E8 + (uint8_t)(x << 1)), prg((uint16_t)(0xE16B + x)));
    }
}

static uint8_t anim4(void) { return (uint8_t)((ram.frame_phase & 0x18) >> 2); }

static void SceneWalk(void) /* $E229: walk in, stop, walk on, then the scene ends */
{
    if (R(0x3F0) == 0x02) {
        R(0x3F3)--;
        if (R(0x3F3) < 0x20)
            ram.game_substate++;
        return;
    }
    if (R(0x3F0) == 0x01) {
        if (--R(0x3F1) == 0)
            R(0x3F0)++;
        return;
    }
    R(0x3F3)--;
    if (R(0x3F3) < 0x80) {
        R(0x3F0)++;
        R(0x3F1) = 0x80;
    }
}

static void Scene0(void) /* $E0E9 */
{
    TitleLetters();
    SetScreenPos(0x60, R(0x3F3));
    if (ram.frame_phase & 0x08)
        DrawMetaspriteWindow(0xE1, 0x98);
    else
        DrawMetaspriteWindow(0xE1, 0x77);
    uint8_t hi = 0xE1, lo = 0xB9;
    if (R(0x3F0) != 0x00 && R(0x3F0) != 0x02) {
        uint8_t x = anim4();
        lo = prg((uint16_t)(0xE1E9 + x));
        hi = prg((uint16_t)(0xE1EA + x));
    }
    DrawMetaspriteWindow(hi, lo);
    SceneWalk();
}

static void Scene1(void) /* $E1F1 */
{
    uint8_t hi = 0xE2, lo = 0x87;
    if (R(0x3F0) != 0x00 && R(0x3F0) != 0x02) {
        uint8_t x = anim4();
        lo = prg((uint16_t)(0xE290 + x));
        hi = prg((uint16_t)(0xE291 + x));
    }
    DrawActor(hi, lo);
    if (R(0x3F0) != 0x01 && (ram.frame_phase & 0x08))
        DrawMetaspriteWindow(0xE2, 0x72);
    else
        DrawMetaspriteWindow(0xE2, 0x5D);
    SceneWalk();
}

static void WalkLeftTo84(void) /* $E2D8 */
{
    uint8_t x = anim4();
    DrawActor(TABLE_PTR(0xD506, x));
    R(0x3F3)--;
    if (R(0x3F3) < 0x84) {
        R(0x3F0)++;
        R(0x3F1) = 0x40;
    }
}

static void Scene2(void) /* $E2C4 */
{
    JumpTableInline(0xE2CA, R(0x3F0));
    switch (R(0x3F0)) {
    case 0:
        WalkLeftTo84();
        break;
    case 1:
        if (ram.frame_phase & 0x10)
            DrawActor(0xE3, 0x8D);
        else
            DrawActor(0xE3, 0x80);
        if (--R(0x3F1) == 0) {
            R(0x3F0)++;
            R(0x3F1) = 0x20;
        }
        break;
    case 2:
        DrawActor(0xE3, 0x9A);
        if (--R(0x3F1) == 0)
            R(0x3F0)++;
        break;
    case 3: {
        uint8_t x = (uint8_t)((ram.frame_phase & 0x0C) >> 1);
        DrawActor(TABLE_PTR(0xE3A3, x));
        R(0x3E9)++;
        R(0x3F3)++;
        if (R(0x3F3) >= 0xF0)
            ram.game_substate++;
        break;
    }
    }
    SetScreenPos(0x80, R(0x3E9));
    if (R(0x3F0) < 0x02)
        DrawMetaspriteWindow(0xE3, (ram.frame_phase & 0x10) ? 0xED : 0xE4);
    else
        DrawMetaspriteWindow(0xE3, (ram.frame_phase & 0x08) ? 0xDB : 0xD2);
}

static void Scene3(void) /* $E3F6 */
{
    for (R(0x4E) = 3; !(R(0x4E) & 0x80); R(0x4E)--) {
        uint8_t x = (uint8_t)(R(0x4E) << 1);
        SetScreenPos(prg((uint16_t)(0xE48E + x)), R(0x3E9 + x));
        DrawMetaspriteWindow(0xE4, (ram.frame_phase & 0x08) ? 0x9F : 0x96);
    }
    SetScreenPos(R(0x3F4), R(0x3F3));
    uint8_t x = R(0x3F0) ? 2 : 0;
    if (ram.frame_phase & 0x08)
        x++;
    x = (uint8_t)(x << 1);
    DrawMetaspriteWindow(TABLE_PTR(0xE4A8, x));
    if (R(0x3F0) == 0) { /* flying in, bobbing */
        if (!(ram.frame_phase & 0x0F)) {
            R(0x3F4)--;
            if (ram.frame_phase & 0x40)
                R(0x3F4) = (uint8_t)(R(0x3F4) + 2);
        }
        add16(0x3F2, 0xFF80);
        if (R(0x3F3) < 0x20)
            R(0x3F0)++;
        return;
    }
    for (int k = 6; k >= 0; k--)
        R(0x3E9 + k)++;
    R(0x3F3)++;
    if (R(0x3F3) == 0x18)
        ram.game_substate++;
}

static void Scene4Bubble(void) /* $E5E1 */
{
    SetScreenPos(0x70, 0x78);
    uint8_t t = ram.frame_phase & 0x06;
    DrawMetaspriteWindow(TABLE_PTR(0xDC34, t));
}

static void Scene4(void) /* $E4F4 */
{
    for (R(0x4D) = 1; !(R(0x4D) & 0x80); R(0x4D)--) {
        SetScreenPos(0x80, R(0x3E9 + (uint8_t)(R(0x4D) << 1)));
        DrawMetaspriteWindow(0xE6, (ram.frame_phase & 0x08) ? 0xB4 : 0xAB);
    }
    JumpTableInline(0xE51F, R(0x3F0));
    switch (R(0x3F0)) {
    case 0: {
        uint8_t x = anim4();
        DrawActor(TABLE_PTR(0xE5F8, x));
        R(0x3F3)--;
        if (R(0x3F3) < 0x80) {
            R(0x3F0)++;
            R(0x3F1) = 0x50;
        }
        return;
    }
    case 1:
        DrawActor(0xE6, (ram.frame_phase & 0x20) ? 0x33 : 0x00);
        if (--R(0x3F1) == 0) {
            R(0x3F0)++;
            R(0x3F1) = 0;
            Sound_Play(0x0E);
        }
        break;
    case 2: {
        Scene4Bubble();
        uint8_t x = (uint8_t)(R(0x3F1) << 1);
        R(0x55) = prg((uint16_t)(0xE656 + x));
        R(0x3F3) = prg((uint16_t)(0xE657 + x));
        DrawAtActor(0xE6, 0x5E);
        if (++R(0x3F1) >= 0x04) {
            R(0x3F0)++;
            R(0x3F1) = 0x38;
            R(0x3F3) = 0xA4;
        }
        break;
    }
    case 3:
        if (R(0x3F1) >= 0x30)
            Scene4Bubble();
        DrawActor(0xE6, 0x6E);
        if (--R(0x3F1) == 0)
            R(0x3F0)++;
        break;
    case 4:
        R(0x55) = 0x70;
        DrawAtActor(0xE6, 0x6E);
        R(0x3F3)++;
        if (R(0x3F3) >= 0xF0)
            ram.game_substate++;
        break;
    }
    R(0x3E9)++;
    R(0x3EB)++;
}

static void Scene5Gems(void) /* $E7A0 */
{
    for (R(0x4D) = 2; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = R(0x3E9 + (uint8_t)(R(0x4D) << 1));
        SetScreenPos(R(0x3F0) == 0x03 ? 0x70 : 0x80, x);
        DrawMetaspriteWindow(0xE6, (ram.frame_phase & 0x08) ? 0xB4 : 0xAB);
    }
    uint8_t s = R(0x3F0);
    if (s == 0x03)
        return;
    if (s > 0x03) {
        R(0x3E9)--;
        R(0x3EB)++;
        R(0x3ED)++;
        return;
    }
    if (ram.frame_phase & 0x01) {
        R(0x3E9)++;
        R(0x3EB)--;
        R(0x3ED)--;
    }
}

static void Scene5Diamonds(void) /* $E7F2: 5 diamonds flying off */
{
    for (R(0x4D) = 4; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = (uint8_t)(R(0x4D) << 2);
        add16((uint16_t)(0x3F4 + x), (uint16_t)(prg((uint16_t)(0xE83F + x)) | prg((uint16_t)(0xE840 + x)) << 8));
        add16((uint16_t)(0x3F6 + x), (uint16_t)(prg((uint16_t)(0xE841 + x)) | prg((uint16_t)(0xE842 + x)) << 8));
        SetScreenPos(R(0x3F5 + x), R(0x3F7 + x));
        uint8_t t = ram.frame_phase & 0x06;
        DrawMetaspriteWindow(TABLE_PTR(0xDC34, t));
    }
}

static void Scene5Leave(void) /* $E769 */
{
    uint8_t x = anim4();
    DrawActor(TABLE_PTR(0xE688, x));
    Scene5Gems();
    Scene5Diamonds();
    R(0x3F3)++;
    if (R(0x3F3) == 0xD8)
        ram.game_substate++;
}

static void Scene5(void) /* $E6BD */
{
    JumpTableInline(0xE6C3, R(0x3F0));
    switch (R(0x3F0)) {
    case 0:
        WalkLeftTo84();
        return;
    case 1: {
        uint8_t x = anim4();
        DrawActor(TABLE_PTR(0xE2AA, x));
        if (--R(0x3F1) == 0) {
            R(0x3F0)++;
            R(0x3F1) = 0x80;
        }
        return;
    }
    case 2:
        DrawActor(0xE6, (ram.frame_phase & 0x20) ? 0x4D : 0x44);
        Scene5Gems();
        if (--R(0x3F1) == 0) {
            R(0x3F0)++;
            R(0x3F1) = 0x10;
            Sound_Play(0x07);
        }
        return;
    case 3: {
        uint8_t x = anim4();
        R(0x55) = 0x70;
        DrawAtActor(TABLE_PTR(0xD55E, x));
        Scene5Gems();
        if (--R(0x3F1) == 0)
            R(0x3F0)++;
        return;
    }
    case 4:
        if (R(0x3F3) == 0xA8) {
            R(0x3F0)++;
            R(0x3F1) = 0x18;
            Sound_Play(0x0E);
        }
        Scene5Leave();
        return;
    case 5:
        DrawActor(0xE6, 0x7F);
        Scene5Gems();
        Scene5Diamonds();
        if (--R(0x3F1) == 0)
            R(0x3F0)++;
        return;
    case 6:
        Scene5Leave();
        return;
    }
}

static void PlayScene(void) /* $E0D7 */
{
    JumpTableInline(0xE0DD, SCENE);
    switch (SCENE) {
    case 0: Scene0(); break;
    case 1: Scene1(); break;
    case 2: Scene2(); break;
    case 3: Scene3(); break;
    case 4: Scene4(); break;
    case 5: Scene5(); break;
    }
}

/* ---- a world cleared ---- */

void State04_NextStage(void) /* $A9E1 -> $DF6D */
{
    ram.frame_phase++;
    JumpTableInline(0xDF74, ram.game_substate);
    switch (ram.game_substate) {
    case 0: /* the world's scene, on the nametable right of the map */
        FadeOut();
        BlackoutNow();
        WaitForNMI();
        SceneScreenInit();
        SceneFrame();
        DrawPPUString(0xE0, 0x1C);
        SCENE = (R(0x75 + PlayerOffset()) & 0xF0) >> 4;
        ScenePicture();
        ScenePalette();
        StartText((uint16_t)(prg((uint16_t)(0xE8F6 + (uint8_t)(SCENE << 1))) |
                             prg((uint16_t)(0xE8F7 + (uint8_t)(SCENE << 1))) << 8), 0xA9);
        SceneObjects();
        ram.scroll_x = 0xFF;
        Sound_Play(prg((uint16_t)(0xE0D1 + SCENE)));
        if (ram.music_request == 0)
            Sound_Play(0x13);
        RequestRenderingOn();
        ram.nmi_ppu_enable = 0xFF;
        FadeIn();
        ram.game_substate++;
        return;
    case 1:
        PlayScene();
        HideUnusedSprites();
        TypeText();
        if (pressed_a_or_b())
            ram.game_substate++;
        return;
    case 2: {
        HideAllSprites();
        uint8_t keep = ram.palette_buf[0x12];
        LoadSpritePalette(0xA7, 0xDC);
        ram.palette_buf[0x12] = keep;
        ram.nmi_requests = 0x01;
        ram.game_substate++;
        return;
    }
    case 3: /* scroll left onto the map */
        ram.scroll_x = (uint8_t)(ram.scroll_x - 3);
        if (ram.scroll_x == 0) {
            ram.ppuctrl_shadow &= 0xFC;
            SetGameState(0x06);
        }
        return;
    }
}

/* ---- the ending ---- */

static void EndingDiamonds(void) /* $EB95: six diamonds around the text */
{
    for (R(0x4D) = 5; !(R(0x4D) & 0x80); R(0x4D)--) {
        uint8_t x = (uint8_t)(R(0x4D) << 1);
        SetScreenPos(prg((uint16_t)(0xEBCA + x)), prg((uint16_t)(0xEBCB + x)));
        if (ram.frame_phase & 0x10)
            DrawMetaspriteScreen(0xD2, 0x79);
        else
            DrawMetaspriteScreen(0xD2, 0x74);
    }
}

static void JumpStep(void) /* $EC52: y += speed, speed += gravity */
{
    add16(0x40D, (uint16_t)(R(0x40F) | R(0x410) << 8));
    add16(0x40F, 0x60);
}

static void JumpStart(void) /* $EC77 */
{
    R(0x40F) = 0x00;
    R(0x410) = 0xFB;
}

static void EndingRockford(void) /* $EBD6: runs in, jumps three times, then bounces */
{
    JumpTableInline(0xEBDC, R(0x409));
    switch (R(0x409)) {
    case 0:
        R(0x40C)++;
        if (--R(0x40A) == 0) {
            R(0x409)++;
            R(0x40A) = 0x1A;
        }
        break;
    case 1: case 2: case 3:
        R(0x40C)++;
        JumpStep();
        if (--R(0x40A) == 0) {
            R(0x409)++;
            R(0x40A) = prg((uint16_t)(0xEC30 + R(0x409)));
            JumpStart();
        }
        break;
    default:
        JumpStep();
        if (R(0x40E) >= 0x5F)
            JumpStart();
        SetScreenPos(R(0x40E), R(0x40C));
        DrawMetaspriteWindow(0xD5, 0x80);
        return;
    }
    SetScreenPos(R(0x40E), R(0x40C));
    uint8_t x = anim4();
    DrawMetaspriteWindow(TABLE_PTR(0xD532, x));
}

void State0F_Ending(void)
{
    ram.frame_phase++;
    JumpTableInline(0xEAC2, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        DisableRendering();
        SceneScreenInit();
        SCENE = 0;
        SceneFrame();
        ReadVramTo600(0x07, 0x1F, 0x2E);
        DrawPPUString(0x06, 0x00);
        LoadBGPalette(0xEB, 0xAF);
        ScenePalette();
        for (int x = 7; x >= 0; x--)
            R(0x409 + x) = prg((uint16_t)(0xEB49 + x));
        ram.oam_index = 0;
        StartText(0xEBBF, 0xAB);
        ram.ppuctrl_shadow |= 0x03;
        RequestRenderingOn();
        EnableNMI();
        ram.nmi_ppu_enable = 0xFF;
        FadeIn();
        Sound_Play(0x23);
        ram.game_substate++;
        return;
    case 1:
        EndingDiamonds();
        EndingRockford();
        HideUnusedSprites();
        TypeText();
        if (R(0x3E1)) {
            ram.game_substate++;
            R(0x21) = 0;
        }
        return;
    case 2:
        EndingDiamonds();
        EndingRockford();
        HideUnusedSprites();
        if (!(R(0x21) & 0x80)) {
            R(0x21)++;
            return;
        }
        if (!pressed_a_or_b())
            return;
        BlackoutNow();
        SetGameState(R(0x408) ? 0x11 : 0x10);
        return;
    }
}

void State10_EndingScene(void)
{
    ram.frame_phase++;
    JumpTableInline(0xEC89, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        SceneScreenInit();
        SCENE = 0;
        SceneFrame();
        ScenePalette();
        SceneObjects();
        StartText(0xECFC, 0xAA);
        SceneOn();
        ram.game_substate++;
        return;
    case 1:
        PlayScene();
        HideUnusedSprites();
        TypeText();
        return;
    case 2:
        SetGameState(0x12);
        BlackoutNow();
        return;
    }
}

void State11_Credits(void) /* the credits: each world's scene with its names */
{
    ram.frame_phase++;
    JumpTableInline(0xED10, ram.game_substate);
    switch (ram.game_substate) {
    case 0:
        SCENE = 0;
        ram.game_substate++;
        return;
    case 1: {
        FadeOut();
        BlackoutNow();
        WaitForNMI();
        SceneScreenInit();
        SceneFrame();
        ScenePicture();
        ScenePalette();
        SceneObjects();
        uint8_t x = (uint8_t)(SCENE << 1);
        StartText((uint16_t)(prg((uint16_t)(0xEEA4 + x)) | prg((uint16_t)(0xEEA5 + x)) << 8), 0xA6);
        R(0x21) = prg((uint16_t)(0xED9B + x));
        R(0x22) = prg((uint16_t)(0xED9C + x));
        ram.ppuctrl_shadow |= 0x03;
        ram.nmi_ppu_enable = 0xFF;
        RequestRenderingOn();
        EnableNMI();
        FadeIn();
        ram.game_substate++;
        return;
    }
    case 2:
        PlayScene();
        HideUnusedSprites();
        TypeText();
        add16(0x21, 0xFFFF);
        if (R(0x21) | R(0x22))
            return;
        LoadSpritePalette(0xED, 0xEA);
        LoadBGPalette(0xED, 0xEA);
        R(0x38) = 0x20;
        R(0x39) = 0x3A;
        R(0x3A) = 0x1A;
        ram.nmi_requests = 0x01;
        ram.game_substate++;
        R(0x21) = 0x80;
        return;
    case 3:
        TypeText();
        if (--R(0x21) == 0) {
            ram.game_substate++;
            R(0x21) = 0x80;
        }
        return;
    case 4:
        if (--R(0x21) != 0)
            return;
        if (++SCENE >= 0x06) {
            ram.game_substate++;
            return;
        }
        ram.game_substate = 0x01;
        return;
    case 5: /* the last screen */
        FadeOut();
        BlackoutNow();
        WaitForNMI();
        ram.nmi_ppu_enable = 0;
        SCENE = 0;
        ClearScreen();
        MapScreenSetup();
        SceneFrame();
        ReadVramTo600(0x07, 0x1F, 0x2E);
        DrawPPUString(0x06, 0x00);
        DrawPPUString(0xEF, 0xE2);
        LoadBGPalette(0xEB, 0xAF);
        ScenePalette();
        ram.nmi_ppu_enable = 0xFF;
        RequestRenderingOn();
        EnableNMI();
        FadeIn();
        ram.game_substate++;
        return;
    case 6: { /* stays here, except in a 2-player game: the other player's turn */
        EndingDiamonds();
        SetScreenPos(0x60, 0x80);
        DrawMetaspriteScreen(0xF2, (ram.frame_phase & 0x10) ? 0x17 : 0x0E);
        if (!pressed_a_or_b() || (ram.play_mode & 0x30) != 0x30)
            return;
        uint8_t x = ram.play_mode & 0x01;
        ram.play_mode = (ram.play_mode & prg((uint16_t)(0xEEA2 + x))) ^ 0x01;
        SetGameState((R(0x74 + PlayerOffset()) & 0x80) ? 0x05 : 0x02);
        return;
    }
    }
}

/* ---- the next level ---- */

static void NextLevelRockford(uint8_t y, uint8_t x) /* $F1F9 */
{
    SetScreenPos(y, x);
    DrawMetaspriteScreen(0xF2, (ram.frame_phase & 0x10) ? 0x17 : 0x0E);
}

static void NextLevelSprites(void) /* $F19B: by the new loop */
{
    uint8_t loop = R(0x74 + PlayerOffset()) & 0x03;
    JumpTableInline(0xF1A5, loop);
    switch (loop) {
    case 0:
        return;
    case 1:
        NextLevelRockford(0x70, 0x80);
        return;
    case 2:
        NextLevelRockford(0x80, 0x80);
        for (R(0x4D) = 2; !(R(0x4D) & 0x80); R(0x4D)--) {
            SetScreenPos(0x80, prg((uint16_t)(0xF1E0 + R(0x4D))));
            DrawMetaspriteScreen(0xE6, (ram.frame_phase & 0x08) ? 0xB4 : 0xAB);
        }
        return;
    case 3:
        NextLevelRockford(0x88, 0x88);
        SetScreenPos(0x88, 0x78);
        DrawMetaspriteScreen(0xE1, 0x77);
        return;
    }
}

void State12_NextLevel(void)
{
    ram.frame_phase++;
    JumpTableInline(0xEFFC, ram.game_substate);
    switch (ram.game_substate) {
    case 0: {
        SceneScreenInit();
        SCENE = 0;
        SceneFrame();
        uint8_t t = (uint8_t)((R(0x74 + PlayerOffset()) & 0x03) << 1);
        DrawPPUString(TABLE_PTR(0xFD86, t));
        LoadBGPalette(TABLE_PTR(0xF0AF, t));
        SCENE = 0x02;
        ScenePalette();
        StartText((uint16_t)(prg((uint16_t)(0xF0E7 + t)) | prg((uint16_t)(0xF0E8 + t)) << 8), 0x86);
        SceneOn();
        ram.oam_index = 0;
        ram.game_substate++;
        return;
    }
    case 1:
        NextLevelSprites();
        HideUnusedSprites();
        TypeText();
        if (R(0x3E1))
            ram.game_substate++;
        return;
    case 2:
        NextLevelSprites();
        HideUnusedSprites();
        if (R(0x29 + (ram.play_mode & 0x01)) & 0x08)
            SetGameState(0x05);
        return;
    }
}

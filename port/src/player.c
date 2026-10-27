/* Player: PlayerInput ($C06B), PlayerHazards ($C373), CollectDiamond ($CC8F)
 * and TickTime ($CFC6). Same RAM writes as the original, scratch included.
 *
 * player_phase: bits 4-5 direction (0 up, 1 down, 2 left, 3 right), low
 * nibble phase (0 idle, 1 walking, 2 pushing, 3 dying, 4 in exit,
 * 6-8 out of time). The position is in pixels; a step is 8 frames x 2 px.
 */
#include "bd.h"

enum { BTN_A = 0x01, BTN_B = 0x02, BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80 };

/* The d-pad as the original decodes it: Right, Left, Down, Up in that order.
 * Returns the direction (3 right, 2 left, 1 down, 0 up) and moves row/col;
 * -1 if no direction is held. $4D is left holding the direction either way. */
static int DecodeDirection(uint8_t *row, uint8_t *col)
{
    uint8_t j = ram.joy_held;
    R(0x4D) = 3;
    if (j & BTN_RIGHT) { (*col)++; return 3; }
    R(0x4D) = 2;
    if (j & BTN_LEFT) { (*col)--; return 2; }
    R(0x4D) = 1;
    if (j & BTN_DOWN) { (*row)++; return 1; }
    R(0x4D) = 0;
    if (j & BTN_UP) { (*row)--; return 0; }
    return -1;
}

static void Player_Walking(void); /* $C209 */

/* $C0B8: A or B held: dig/collect the next cell without moving, or push a
 * boulder sideways with A + Left/Right. */
static void Player_Snap(void)
{
    uint8_t row, col;
    GetPlayerCell(&row, &col);
    R(0x4F) = col;
    int d = DecodeDirection(&row, &col);
    if (d < 0)
        return;
    if (d == 3)
        R(0x4F) = (uint8_t)(col + 1); /* the cell beyond, for a push */
    else if (d == 2)
        R(0x4F) = (uint8_t)(col - 1);
    ram.player_phase = (uint8_t)(R(0x4D) << 4);
    R(0x4D) = row;
    R(0x4E) = col;
    uint8_t t = GetCell(row, col);
    if (t == 0x70) {
        if (ram.joy_held != (BTN_A | BTN_LEFT) && ram.joy_held != (BTN_A | BTN_RIGHT))
            return;
        if (GetCell(R(0x4D), R(0x4F)) != 0x00)
            return;
        SetCell(0x70, R(0x4D), R(0x4F));
        QueueCellRedraw(0x70, R(0x4D), R(0x4F));
    } else if (t == 0x80) {
        CollectDiamond();
    } else if (t == 0x20) {
        PlaySoundEffect(0);
    } else {
        return;
    }
    SetCell(0x10, R(0x4D), R(0x4E));
    QueueCellRedraw(0x00, R(0x4D), R(0x4E));
}

/* $C150: start a step into the next cell */
static void Player_Walk(void)
{
    uint8_t row, col;
    GetPlayerCell(&row, &col);
    if (DecodeDirection(&row, &col) < 0)
        return;
    ram.player_phase = (uint8_t)(R(0x4D) << 4);
    R(0x4E) = row; /* note: here $4E/$4F are the target row/col */
    R(0x4F) = col;
    uint8_t t = GetCell(row, col);
    switch (t) {
    case 0x40: /* exit */
        if (!ram.exit_open)
            return;
        Sound_Play(1);
        SetGameState(0x0C);
        break;
    case 0x70: { /* boulder: push left/right only, after a delay */
        uint8_t dir;
        if (ram.joy_held == BTN_LEFT)
            dir = 0x20;
        else if (ram.joy_held == BTN_RIGHT)
            dir = 0x30;
        else
            return;
        ram.player_phase = dir | 0x02;
        ram.player_phase_timer = 0x18;
        return;
    }
    case 0x20: /* dirt */
        PlaySoundEffect(0);
        break;
    case 0x00: case 0xA0: case 0x10:
        break;
    case 0x80: case 0x90:
        CollectDiamond();
        break;
    default:
        return;
    }
    GetPlayerCell(&row, &col);
    SetCell(0x10, row, col);
    SetCell(0xE0, R(0x4E), R(0x4F));
    QueueCellRedraw(0x00, R(0x4E), R(0x4F)); /* the player itself is a sprite */
    ram.player_phase = (uint8_t)(R(0x4D) << 4 | 0x01);
    ram.player_phase_timer = 0x08;
    Player_Walking();
}

static void Player_Idle(void) /* $C0A8 */
{
    if (ram.frame_phase & 0x07)
        return;
    if (ram.joy_held & (BTN_A | BTN_B))
        Player_Snap();
    else
        Player_Walk();
}

static void add16(uint8_t *lo_hi, int delta)
{
    uint16_t v = (uint16_t)((lo_hi[0] | lo_hi[1] << 8) + delta);
    lo_hi[0] = (uint8_t)v;
    lo_hi[1] = (uint8_t)(v >> 8);
}

static void Player_Walking(void)
{
    uint8_t d = (uint8_t)((ram.player_phase & 0x30) >> 4);
    JumpTableInline(0xC214, d);
    switch (d) {
    case 0: add16(ram.player_y, -2); break;
    case 1: add16(ram.player_y, +2); break;
    case 2: add16(ram.player_x, -2); break;
    case 3: add16(ram.player_x, +2); break;
    }
    if (--ram.player_phase_timer == 0) {
        R(0x67) = ram.game_state == 0x0C ? 0x04 : 0x00; /* in the exit, or idle */
        ram.player_phase = (uint8_t)((ram.player_phase & 0xF0) | R(0x67));
    }
}

static void Player_Pushing(void) /* $C272 */
{
    uint8_t row, col;
    if (--ram.player_phase_timer != 0) {
        /* keep holding toward the boulder, and it must still be there */
        GetPlayerCell(&row, &col);
        if ((ram.player_phase & 0x30) == 0x30) {
            if (ram.joy_held != BTN_RIGHT)
                goto cancel;
            col++;
        } else {
            if (ram.joy_held != BTN_LEFT)
                goto cancel;
            col--;
        }
        if (GetCell(row, col) != 0x00)
            return;
    cancel:
        ram.player_phase &= 0xF0;
        return;
    }
    GetPlayerCell(&row, &col);
    if ((ram.player_phase & 0x30) == 0x30) {
        col++;
        R(0x4F) = (uint8_t)(col + 1);
        R(0x50) = 0x30;
    } else {
        col--;
        R(0x4F) = (uint8_t)(col - 1);
        R(0x50) = 0x20;
    }
    R(0x4D) = row;
    R(0x4E) = col;
    if (GetCell(row, R(0x4F)) != 0x00)
        return;
    SetCell(0x70, R(0x4D), R(0x4F));
    QueueCellRedraw(0x70, R(0x4D), R(0x4F));
    SetCell(0xE0, R(0x4D), R(0x4E));
    QueueCellRedraw(0x00, R(0x4D), R(0x4E));
    GetPlayerCell(&row, &col);
    SetCell(0x10, row, col);
    ram.player_phase = R(0x50) | 0x01;
    ram.player_phase_timer = 0x08;
    PlaySoundEffect(0);
    Player_Walking();
}

static void Player_Dying(void) /* $C30D: the sprite jumps up and falls off screen */
{
    if (--ram.player_phase_timer == 0) {
        if (ram.play_mode & 0x80) { /* attract demo: back to the title */
            ram.nmi_ppu_enable = 0xFF;
            SetGameState(0x01);
        } else {
            SetGameState(0x0D);
        }
        return;
    }
    if (ram.player_y[1] == 0x01 && (uint8_t)(ram.player_y[0] - ram.camera_y[0]) >= 0xD8)
        return; /* already below the screen */
    add16(ram.player_y, (int16_t)(R(0x98) | R(0x99) << 8)); /* velocity, 8.8 */
    if (ram.frame_phase & 0x0F) { /* gravity: velocity += $40/256 */
        unsigned v = R(0x97) + 0x40u;
        R(0x97) = (uint8_t)v;
        v = R(0x98) + (v >> 8);
        R(0x98) = (uint8_t)v;
        R(0x99) = (uint8_t)(R(0x99) + (v >> 8));
    }
}

static void Player_OutOfTime(void) /* $C35C: phases 6, 7 */
{
    if (--ram.player_phase_timer == 0) {
        ram.player_phase++;
        ram.player_phase_timer = 0x20;
    }
}

static void Player_OutOfTimeEnd(void) /* $C367: phase 8 */
{
    if (--ram.player_phase_timer == 0) {
        ram.player_phase++;
        SetGameState(0x0D);
    }
}

void PlayerInput(void)
{
    if (ram.joy_held & 0xF0)
        ram.joy_repeat_timer = 0x80;
    else if (ram.joy_repeat_timer)
        ram.joy_repeat_timer--;
    if ((ram.joy_held & (BTN_A | BTN_B)) == (BTN_A | BTN_B))
        ram.giveup_counter++;
    else
        ram.giveup_counter = 0;

    uint8_t phase = ram.player_phase & 0x0F;
    JumpTableInline(0xC096, phase);
    switch (phase) {
    case 0: Player_Idle(); break;
    case 1: Player_Walking(); break;
    case 2: Player_Pushing(); break;
    case 3: Player_Dying(); break;
    case 4: case 5: break;
    case 6: case 7: Player_OutOfTime(); break;
    case 8: Player_OutOfTimeEnd(); break;
    default: break; /* beyond the original's table: never happens in the game */
    }
}

static bool IsEnemy(uint8_t row, uint8_t col) /* $C402 */
{
    uint8_t t = GetCell(row, col) & 0xF0;
    return t == 0xB0 || t == 0xC0;
}

void PlayerHazards(void)
{
    if (ram.frame_phase & 0x07)
        return;
    if ((ram.player_phase & 0x0F) >= 0x03)
        return;
    uint8_t row, col;
    GetPlayerCell(&row, &col);
    R(0x4D) = row;
    R(0x4E) = col;
    if (ram.giveup_counter >= 0x40) { /* held A+B: give up */
        ram.player_phase = (uint8_t)((ram.player_phase & 0xF0) | 0x06);
        ram.player_phase_timer = 0x40;
        PlaySoundEffect(7);
        return;
    }
    if (IsEnemy((uint8_t)(R(0x4D) - 1), R(0x4E)) || IsEnemy((uint8_t)(R(0x4D) + 1), R(0x4E)) ||
        IsEnemy(R(0x4D), (uint8_t)(R(0x4E) - 1)) || IsEnemy(R(0x4D), (uint8_t)(R(0x4E) + 1)))
        KillPlayer();
}

void CollectDiamond(void)
{
    AddScore(ram.exit_open ? ram.bonus_diamond_value : ram.diamond_value);
    if (!ram.exit_open) {
        uint8_t *d = ram.diamonds_needed; /* 3 decimal digits */
        if ((int8_t)--d[0] < 0) {
            d[0] = 9;
            if ((int8_t)--d[1] < 0) {
                d[1] = 9;
                d[2]--;
            }
        }
        if ((d[0] | d[1] | d[2]) == 0) { /* enough: open the exit */
            ram.exit_open = 1;
            uint8_t x = (uint8_t)(ram.cave_index << 1);
            QueueCellRedraw(0x40, prg((uint16_t)(ROM_ExitPositions + x)),
                            prg((uint16_t)(ROM_ExitPositions + x + 1)));
            PlaySoundEffect(5);
            R(0xA3) = 0x20;
            PlayerScreenPos(&R(0xA4), &R(0xA5));
            return;
        }
    }
    PlaySoundEffect(1);
}

void DecrementTime(void) /* $D039 */
{
    uint8_t *t = ram.time_left;
    if ((int8_t)--t[0] < 0) {
        t[0] = 9;
        if ((int8_t)--t[1] < 0) {
            t[1] = 9;
            t[2]--;
        }
    }
}

bool PlayerOnBoxRow(void) /* $D04A */
{
    return (uint8_t)(0x30 + ram.player_y[0] - ram.camera_y[0]) == 0x80;
}

void TickTime(void)
{
    if ((ram.time_left[0] | ram.time_left[1] | ram.time_left[2]) == 0)
        return;
    if (ram.frame_count & 0x3F)
        return;
    DecrementTime();
    if ((ram.time_left[0] | ram.time_left[1] | ram.time_left[2]) == 0) {
        /* out of time: player phase 6 and the time-up pause */
        ram.player_phase = (uint8_t)((ram.player_phase & 0xF0) | 0x06);
        ram.player_phase_timer = 0x40;
        PlaySoundEffect(7);
        ram.pause_state = 0x02;
        R(0x71) = 0x00;
        ram.game_substate = 0x0A;
        uint16_t cy = (uint16_t)(ram.camera_y[0] | ram.camera_y[1] << 8) - 0x20;
        uint16_t cx = (uint16_t)(ram.camera_x[0] | ram.camera_x[1] << 8) + 0x80;
        R(0x5F) = (uint8_t)cy;
        R(0x60) = (uint8_t)(cy >> 8);
        R(0x61) = (uint8_t)cx;
        R(0x62) = (uint8_t)(cx >> 8);
        if (PlayerOnBoxRow())
            ram.game_substate = 0x09;
        return;
    }
    if ((ram.time_left[0] | ram.time_left[2]) == 0 && ram.time_left[1] == 3) /* 30 left: hurry music */
        Sound_Play(prg((uint16_t)(0xD05A + (ram.cave_index >> 2))));
}

/* Helpers shared by many routines. Each one reproduces the original's RAM
 * writes, scratch bytes included, so the RAM fixtures match exactly. */
#include "bd.h"

uint8_t PlayerOffset(void) { return (ram.play_mode & 0x01) ? 0x0A : 0x00; }

void JumpTableInline(uint16_t table, uint8_t index)
{
    /* The original pulls the return address and leaves the target in $67/68. */
    uint16_t t = (uint16_t)(table + (uint8_t)(index << 1));
    R(0x67) = prg(t);
    R(0x68) = prg((uint16_t)(t + 1));
}

uint16_t CaveRowPointer(uint8_t row)
{
    uint8_t i = (uint8_t)(row << 1);
    return (uint16_t)(prg((uint16_t)(ROM_CaveRowPointers + i)) |
                      prg((uint16_t)(ROM_CaveRowPointers + i + 1)) << 8);
}

uint8_t GetCell(uint8_t row, uint8_t col)
{
    set_zp_ptr(0x67, CaveRowPointer(row));
    return *ind_y(0x67, col);
}

void GetPlayerCell(uint8_t *row, uint8_t *col)
{
    /* 16-bit pixel position / 16; $67 is left holding the high byte >> 4. */
    uint16_t y = (uint16_t)(ram.player_y[0] | ram.player_y[1] << 8);
    uint16_t x = (uint16_t)(ram.player_x[0] | ram.player_x[1] << 8);
    *row = (uint8_t)(y >> 4);
    *col = (uint8_t)(x >> 4);
    R(0x67) = (uint8_t)(ram.player_x[1] >> 4);
}

void SetCell(uint8_t tile, uint8_t row, uint8_t col)
{
    set_zp_ptr(0x67, CaveRowPointer(row));
    *ind_y(0x67, col) = tile;
}

void SetGameState(uint8_t state)
{
    ram.game_state = state;
    ram.game_substate = 0;
    R(0x21) = 0;
    R(0x22) = 0;
}

void CopyPlayerInput(void)
{
    ram.joy_held = ram.joy1_held;
    ram.joy_pressed = ram.joy1_pressed;
    if (ram.play_mode & 0x01) {
        ram.joy_held = ram.joy2_held;
        ram.joy_pressed = ram.joy2_pressed;
    }
}

void PlayerScreenPos(uint8_t *y, uint8_t *x)
{
    *y = (uint8_t)(ram.player_y[0] - ram.camera_y[0] + 0x30);
    *x = (uint8_t)(ram.player_x[0] - ram.camera_x[0] + 0x40);
}

bool QueueCellRedraw(uint8_t tile, uint8_t row, uint8_t col)
{
    R(0x67) = tile;
    R(0x68) = row;
    R(0x69) = col;
    if (ram.redraw_count >= 0x37)
        return true;
    uint8_t i = (uint8_t)(ram.redraw_index * 3);
    R(0x753 + i) = tile;
    R(0x754 + i) = row;
    R(0x755 + i) = col;
    ram.redraw_count++;
    if (++ram.redraw_index >= 0x37)
        ram.redraw_index = 0;
    return false;
}

/* ---- sound requests ($811C-$81AB) ----
 * Only the request logic: which id ends up in music_request / sfx_request.
 * Playback itself (Sound_Update) is not ported yet. */

static void SoundHeaderOf(uint8_t id) /* $817A: header of `id` -> $03/04 */
{
    uint8_t x = (uint8_t)((id & 0x7F) << 1);
    R(0x03) = prg((uint16_t)(ROM_SoundTable + x));
    R(0x04) = prg((uint16_t)(ROM_SoundTable + x + 1));
}

static uint8_t SoundAllowed(void) /* $8189: may the new id ($00) interrupt? */
{
    R(0x00)--;
    uint16_t h = zp_ptr(0x03);
    R(0x03) = rd((uint16_t)(h + 1));
    R(0x04) = rd((uint16_t)(h + 2));
    uint8_t bits = rd((uint16_t)(zp_ptr(0x03) + (R(0x00) >> 3)));
    uint8_t mask = prg((uint16_t)(0x81AC + (R(0x00) & 7)));
    R(0x00)++;
    return bits & mask;
}

void Sound_Play(uint8_t id)
{
    R(0x00) = id;
    if (id == 0) {
        ram.music_request = 0;
        ram.sfx_request = 0;
        return;
    }
    uint8_t x = (uint8_t)(id << 1);
    R(0x01) = prg((uint16_t)(ROM_SoundTable + x));
    R(0x02) = prg((uint16_t)(ROM_SoundTable + x + 1));
    uint8_t header = rd(zp_ptr(0x01));
    if (!(header & 0x80)) { /* music */
        if (ram.music_request) {
            SoundHeaderOf(ram.music_request);
            if (!SoundAllowed())
                return;
        }
        ram.music_request = R(0x00);
        return;
    }
    /* sound effect: check against the music only if they share channels */
    if (ram.music_request) {
        SoundHeaderOf(ram.music_request);
        if ((header & 0x0F) & rd(zp_ptr(0x03))) {
            if (!SoundAllowed())
                return;
        }
    }
    if (ram.sfx_request) {
        SoundHeaderOf(ram.sfx_request);
        if (!SoundAllowed())
            return;
    }
    ram.sfx_request = R(0x00);
}

void PlaySoundEffect(uint8_t effect)
{
    R(0x67) = effect;
    if (ram.sfx_request) {
        uint8_t group = prg((uint16_t)(0xC02C + (ram.sfx_request & 0x7F)));
        uint8_t x = (uint8_t)((group << 3) + R(0x67));
        if (prg((uint16_t)(0xBFEC + x)) == 0)
            return;
    }
    Sound_Play(prg((uint16_t)(0xC03C + R(0x67))));
}

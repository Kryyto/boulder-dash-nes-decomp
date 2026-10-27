/* Sprite routines: PlayerSprite ($D398), DrawMetaspriteWorld ($A505) and
 * HideUnusedSprites ($A5E0). Faithful ports (they only write RAM: oam_buf
 * and scratch), covered by the RAM fixtures. */
#include "bd.h"

void DrawMetaspriteWorld(uint8_t attr_or, uint8_t ptr_hi, uint8_t ptr_lo)
{
    R(0x10) = attr_or;
    R(0x12) = ptr_hi;
    R(0x11) = ptr_lo;
    /* world position ($55/56 y, $57/58 x) minus the camera */
    set_zp_ptr(0x55, (uint16_t)(zp_ptr(0x55) - zp_ptr(0xC0)));
    if (R(0xC1) != R(0x56))
        set_zp_ptr(0x55, (uint16_t)(zp_ptr(0x55) - 0x10));
    set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x57) - zp_ptr(0xBE)));

    uint8_t x = ram.oam_index, y = 0;
    for (;;) {
        uint8_t dy = rd((uint16_t)(zp_ptr(0x11) + y));
        if (dy == 0x7F) {
            ram.oam_index = x;
            return;
        }
        /* sign-extended offsets; a sprite whose position leaves 0-255 is dropped */
        set_zp_ptr(0x13, (uint16_t)(zp_ptr(0x55) + (int8_t)dy));
        R(0x200 + x) = R(0x13);
        y++;
        R(0x201 + x) = rd((uint16_t)(zp_ptr(0x11) + y));
        y++;
        R(0x202 + x) = rd((uint16_t)(zp_ptr(0x11) + y)) | R(0x10);
        y++;
        uint8_t dx = rd((uint16_t)(zp_ptr(0x11) + y));
        set_zp_ptr(0x15, (uint16_t)(zp_ptr(0x57) + (int8_t)dx));
        R(0x203 + x) = R(0x15);
        y++;
        x = (uint8_t)(x + 4);
        if (R(0x14) | R(0x16))
            x = (uint8_t)(x - 4);
    }
}

/* frame list at `list`, entry `index` -> metasprite pointer (lo, hi) */
static void FrameFromList(uint16_t list, uint8_t index, uint8_t *lo, uint8_t *hi)
{
    set_zp_ptr(0x59, list);
    uint8_t y = (uint8_t)(index << 1);
    *lo = rd((uint16_t)(zp_ptr(0x59) + y));
    *hi = rd((uint16_t)(zp_ptr(0x59) + (uint8_t)(y + 1)));
}

void PlayerSprite(void)
{
    set_zp_ptr(0x55, (uint16_t)((ram.player_y[0] | ram.player_y[1] << 8) + 0x2F));
    set_zp_ptr(0x57, (uint16_t)((ram.player_x[0] | ram.player_x[1] << 8) + 0x48));
    uint8_t phase = ram.player_phase & 0x0F;
    JumpTableInline(0xD3B9, phase);
    uint8_t lo = 0, hi = 0;
    uint16_t walk;
    switch (phase) {
    case 0: /* standing */
        if (ram.joy_held & 0xF0)
            goto walking;
        if (ram.joy_repeat_timer) { /* just stopped: first frame of the last direction */
            uint8_t x = (uint8_t)((ram.player_phase >> 4) << 1);
            R(0x59) = prg((uint16_t)(0xD4A6 + x));
            R(0x5A) = prg((uint16_t)(0xD4A7 + x));
            lo = rd(zp_ptr(0x59));
            hi = rd((uint16_t)(zp_ptr(0x59) + 1));
        } else { /* idle animation */
            FrameFromList(0xD48C, (uint8_t)((ram.frame_phase & 0x60) >> 5), &lo, &hi);
        }
        break;
    case 1: case 2:
    walking: { /* direction from the buttons, 4 frames */
        uint8_t x = (uint8_t)(prg((uint16_t)(0xD47C + (ram.joy_held >> 4))) << 1);
        walk = (uint16_t)(prg((uint16_t)(0xD4A6 + x)) | prg((uint16_t)(0xD4A7 + x)) << 8);
        FrameFromList(walk, (uint8_t)((ram.frame_phase & 0x18) >> 3), &lo, &hi);
        break;
    }
    case 3: FrameFromList(0xD55E, (uint8_t)((ram.frame_phase & 0x30) >> 4), &lo, &hi); break;
    case 4: lo = 0x80; hi = 0xD5; break;
    case 5: FrameFromList(0xD578, (uint8_t)((ram.frame_phase & 0x0C) >> 2), &lo, &hi); break;
    case 6: FrameFromList(0xD5A4, (uint8_t)((ram.frame_phase & 0x0C) >> 2), &lo, &hi); break;
    case 7: lo = 0xD0; hi = 0xD5; break;
    case 8: lo = 0xD9; hi = 0xD5; break;
    default: return; /* beyond the original's table */
    }
    ram.oam_index = 0x50;
    DrawMetaspriteWorld(0x00, hi, lo);
}

void HideUnusedSprites(void)
{
    /* $A5E0: park every sprite from oam_index on below the screen */
    uint8_t x = ram.oam_index;
    if (x != 0) {
        do {
            R(0x200 + x) = 0xF0;
            x = (uint8_t)(x + 4);
        } while (x != 0);
    }
    ram.oam_index = x;
}

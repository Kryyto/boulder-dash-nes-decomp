/* Cave loading: LoadCave ($B789) and ApplyCavePatches ($B8AE). */
#include "bd.h"

uint8_t WorldStageToCave(uint8_t world_stage)
{
    R(0x67) = world_stage;
    R(0x68) = (uint8_t)((world_stage & 0xF0) >> 2);
    return (uint8_t)((world_stage & 0x03) + R(0x68));
}

void BinToDec3(uint8_t value)
{
    R(0x4D) = (uint8_t)(value % 10);
    R(0x4E) = (uint8_t)(value / 10 % 10);
    R(0x4F) = (uint8_t)(value / 100);
}

void LoadCave(void)
{
    uint8_t x = PlayerOffset();
    ram.cave_index = WorldStageToCave(R(0x75 + x));

    /* 12-byte header: start row, start col, diamond values, then
     * (diamonds needed, time) for each of the 4 difficulty loops */
    uint8_t t = (uint8_t)(ram.cave_index << 1);
    R(0x55) = prg((uint16_t)(ROM_CaveHeaderTable + t));
    R(0x56) = prg((uint16_t)(ROM_CaveHeaderTable + t + 1));
    uint16_t h = zp_ptr(0x55);
    ram.player_y[0] = rd(h);
    ram.player_y[1] = 0;
    ram.player_x[0] = rd((uint16_t)(h + 1));
    ram.player_x[1] = 0;
    ram.diamond_value = rd((uint16_t)(h + 2));
    ram.bonus_diamond_value = rd((uint16_t)(h + 3));
    uint8_t y = (uint8_t)(((R(0x74 + x) & 0x03) << 1) + 4);
    BinToDec3(rd((uint16_t)(h + y)));
    ram.diamonds_needed[0] = R(0x4D);
    ram.diamonds_needed[1] = R(0x4E);
    ram.diamonds_needed[2] = R(0x4F);
    BinToDec3(rd((uint16_t)(h + y + 1)));
    ram.time_left[0] = R(0x4D);
    ram.time_left[1] = R(0x4E);
    ram.time_left[2] = R(0x4F);

    /* The layout is in CHR ROM: (CHR1 bank, PPU address) per cave, read
     * through PPUDATA after a dummy read for the read buffer. */
    uint8_t li = (uint8_t)(ram.cave_index * 3);
    MMC1_SetCHR1(prg((uint16_t)(ROM_CaveLayoutLocations + li)));
    SetPPUAddr(prg((uint16_t)(ROM_CaveLayoutLocations + li + 2)), prg((uint16_t)(ROM_CaveLayoutLocations + li + 1)));
    io_read(0x2007);

    /* 22 rows x 20 bytes, two 4-bit tiles per byte (low nibble first) */
    set_zp_ptr(0x57, 0x03E0);
    for (R(0x4D) = 0x16; R(0x4D) != 0; R(0x4D)--) {
        for (int i = 0; i < 0x14; i++) {
            uint8_t b = io_read(0x2007);
            *ind_y(0x57, 0) = (uint8_t)(b << 4);
            set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x57) + 1));
            *ind_y(0x57, 0) = b & 0xF0;
            set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x57) + 1));
        }
    }
    ram.pause_state = 0;
    R(0x71) = 0;
    ram.magic_wall_timer = 0;
    ram.magic_wall_state = 0;
    ram.exit_open = 0;
}

void ApplyCavePatches(void)
{
    uint8_t t = (uint8_t)(ram.cave_index << 1);
    R(0x55) = prg((uint16_t)(ROM_CavePatchTable + t));
    R(0x56) = prg((uint16_t)(ROM_CavePatchTable + t + 1));
    uint8_t loop = R(0x74 + PlayerOffset()) & 0x03;
    if (loop >= 2) {
        /* patch list: count for loop 2, count for loop 3, then (row, col, tile)... */
        R(0x4D) = rd((uint16_t)(zp_ptr(0x55) + (uint8_t)(loop - 2)));
        set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x55) + 2));
        R(0x4E) = 0;
        do {
            /* the patch list is in ROM: read through the bus */
            uint8_t row = rd((uint16_t)(zp_ptr(0x57) + R(0x4E)));
            set_zp_ptr(0x55, CaveRowPointer(row));
            R(0x4F) = rd((uint16_t)(zp_ptr(0x57) + (uint8_t)(R(0x4E) + 1)));
            R(0x50) = rd((uint16_t)(zp_ptr(0x57) + (uint8_t)(R(0x4E) + 2)));
            *ind_y(0x55, R(0x4F)) = R(0x50);
            R(0x4E) = (uint8_t)(R(0x4E) + 3);
        } while (--R(0x4D) != 0);
    }

    /* Find the amoeba (tile $D0), scanning rows 21..0 and columns 39..0. */
    ram.amoeba_state = 0xFF;
    for (int row = 0x15; row >= 0; row--) {
        set_zp_ptr(0x55, CaveRowPointer((uint8_t)row));
        for (int col = 0x27; col >= 0; col--) {
            if (*ind_y(0x55, (uint8_t)col) == 0xD0) {
                ram.amoeba_state = 0x50;
                ram.amoeba_grow_interval = 0xFF;
                ram.amoeba_grow_countdown = 0xFF;
                ram.amoeba_origin[0] = (uint8_t)(row | (row == 1 ? 0x40 : 0xC0));
                ram.amoeba_origin[1] = (uint8_t)col;
                return;
            }
        }
    }
}

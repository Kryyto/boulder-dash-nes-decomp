/* Camera: UpdateCamera ($B197) and its helpers, plus the status digits
 * UpdateStatusSprites ($D2AD). Faithful ports: same RAM writes as the 6502.
 *
 * The camera scrolls in steps of one cell (16 px) over 8 frames at 2 px per
 * frame. zp_C5 holds the step: $81 up, $82 down, $84 left, $88 right; bit 7
 * is cleared halfway. zp_C6 counts frames 0-7. During the first 4 frames the
 * new row or column (copied to the strip at $0106 by FillStrip) is queued for
 * the NMI as metatiles and attribute bytes, 4 cells per frame; $0136 is a
 * shadow of the attribute table.
 */
#include "bd.h"

#define C2 R(0xC2) /* camera_y when a vertical step started */
#define C3 R(0xC3) /* camera_x when a horizontal step started */
#define C4 R(0xC4) /* nonzero: camera frozen (the player died) */
#define C5 R(0xC5)
#define C6 R(0xC6)
#define STRIP(i) R(0x106 + (uint8_t)(i))

static uint16_t ptr_at(uint16_t table, uint8_t i)
{
    return (uint16_t)(prg((uint16_t)(table + i)) | prg((uint16_t)(table + i + 1)) << 8);
}

/* $B1AF: start a step when the player gets near an edge of the view */
static bool CameraStart(void)
{
    if (C4)
        return false;
    set_zp_ptr(0x55, (uint16_t)((ram.player_y[0] | ram.player_y[1] << 8) - zp_ptr(0xC0)));
    set_zp_ptr(0x57, (uint16_t)((ram.player_x[0] | ram.player_x[1] << 8) - zp_ptr(0xBE)));
    uint8_t dir;
    if ((ram.camera_y[0] | ram.camera_y[1]) && (R(0x56) || R(0x55) < 0x40)) {
        C2 = ram.camera_y[0];
        dir = 0; /* up */
    } else if (ram.camera_y[0] < 0x90 && R(0x56)) {
        return false;
    } else if (ram.camera_y[0] < 0x90 && R(0x55) >= 0x70) {
        C2 = ram.camera_y[0];
        dir = 1; /* down */
    } else if ((ram.camera_x[0] | ram.camera_x[1]) && !R(0x58) && R(0x57) < 0x10) {
        C3 = (uint8_t)(ram.camera_x[0] - 0x10);
        dir = 2; /* left */
    } else if (ram.camera_x[1] >= 2) {
        return false;
    } else if (!R(0x58) && R(0x57) < 0x60) {
        return false;
    } else {
        C3 = ram.camera_x[0];
        dir = 3; /* right */
    }
    C5 = prg((uint16_t)(0xB241 + dir));
    C6 = 0;
    return true;
}

/* camera_x >> 4 as the original computes it (leaves camera_x hi >> 4 in $4D) */
static uint8_t CameraColumn(void)
{
    uint16_t v = (uint16_t)(ram.camera_x[0] | ram.camera_x[1] << 8);
    R(0x4D) = (uint8_t)(ram.camera_x[1] >> 4);
    return (uint8_t)(v >> 4);
}

/* $B245: copy the row/column coming into view to the strip ($20 = dirt
 * outside the cave) */
void Camera_FillStrip(void)
{
    uint8_t x, y;
    if (C5 & 0x03) { /* vertical step: a row of 16 cells */
        uint8_t t = (uint8_t)((ram.camera_y[0] >> 4) << 1);
        set_zp_ptr(0x55, ptr_at(0xB348, t));
        if (!(C5 & 0x02)) {
            if (ram.camera_y[0] < 0x30) {
                x = 0;
                goto fill_rest;
            }
        } else {
            set_zp_ptr(0x55, ptr_at(0xB362, t));
        }
        x = 0;
        y = (uint8_t)(CameraColumn() - 4);
        if (y & 0x80) {
            do {
                STRIP(x) = 0x20;
                x++;
                y++;
            } while (y != 0);
        }
        do {
            STRIP(x) = *ind_y(0x55, y);
            y++;
            x++;
            if (x >= 0x10)
                return;
        } while (y < 0x28);
    fill_rest:
        do
            STRIP(x++) = 0x20;
        while (x < 0x10);
        return;
    }
    /* horizontal step: a column of 15 cells */
    set_zp_ptr(0x55, ptr_at(0xB374, (uint8_t)((ram.camera_y[0] >> 4) << 1)));
    R(0x4D) = CameraColumn();
    bool beyond;
    if (!(C5 & 0x08)) {
        R(0x4D) = (uint8_t)(R(0x4D) - 1 - 4);
        beyond = R(0x4D) & 0x80;
    } else {
        R(0x4D) = (uint8_t)(R(0x4D) + 0x0C);
        beyond = R(0x4D) >= 0x28;
    }
    if (beyond) {
        for (x = 0; x < 0x0F; x++)
            STRIP(x) = 0x20;
        return;
    }
    set_zp_ptr(0x55, (uint16_t)(zp_ptr(0x55) + R(0x4D)));
    R(0x4D) = (uint8_t)(ram.camera_y[0] >> 4);
    for (x = 0; x < 0x0F; x++) {
        if (R(0x4D) < 0x02) {
            STRIP(x) = 0x20;
        } else {
            STRIP(x) = *ind_y(0x55, 0);
            set_zp_ptr(0x55, (uint16_t)(zp_ptr(0x55) + 0x28));
        }
        R(0x4D)++;
    }
}

/* $B580: point $67/68 at the tiles of a cell type (per world, exit, magic wall) */
void Camera_MetatileVariant(uint8_t type)
{
    if (type == 0x80 || type == 0x90 || type == 0xA0 || type == 0xD0 || type == 0xE0 || type == 0xF0)
        return;
    set_zp_ptr(0x67, (uint16_t)(zp_ptr(0x67) + (uint8_t)((ram.cave_index >> 2) << 2)));
    if (type == 0x40) {
        if (ram.exit_open)
            set_zp_ptr(0x67, 0xF3E0);
        return;
    }
    if (type == 0x60 && (ram.magic_wall_state & 0x01))
        set_zp_ptr(0x67, 0xF3D8);
}

/* $B537: 4 strip cells -> tiles of metatile_queue entries 0-3 */
static void QueueStripMetatiles(void)
{
    R(0x4D) = 0;
    R(0x4E) = (uint8_t)(C6 << 2);
    for (;;) {
        uint8_t cell = STRIP(R(0x4E));
        R(0x69) = cell;
        uint8_t type = cell & 0xF0;
        set_zp_ptr(0x67, ptr_at(0xF370, (uint8_t)(type >> 3)));
        Camera_MetatileVariant(type);
        uint8_t x = R(0x4D);
        for (uint8_t y = 0; y < 4; y++, x++)
            R(0xC9 + x) = rd((uint16_t)(zp_ptr(0x67) + y));
        R(0x4F) = 0;
        R(0x4E)++;
        R(0x4D) = (uint8_t)(R(0x4D) + 6);
        if (R(0x4D) >= 0x18)
            return;
    }
}

/* $B61A: merge palette $50 into the attribute shadow for nametable address
 * (hi X, lo Y); leaves the attribute address in $55/56 and the byte in $50 */
void Camera_AttributeMerge(uint8_t lo, uint8_t hi)
{
    R(0x67) = lo;
    R(0x68) = hi;
    R(0x69) = (lo >> 2) & 0x07;
    R(0x6A) = hi & 0x03;
    uint8_t a = lo & 0x80;
    for (int i = 0; i < 4; i++) { /* lsr $6A / ror a */
        uint8_t c = R(0x6A) & 1;
        R(0x6A) >>= 1;
        a = (uint8_t)((a >> 1) | (c << 7));
    }
    a = (uint8_t)(a + R(0x69) + 0xC0);
    R(0x55) = a;
    R(0x56) = 0x23;
    uint8_t y = a & 0x3F;
    R(0x6B) = R(0x136 + y);
    if (!(R(0x67) & 0x40)) {
        if (!(R(0x67) & 0x02)) {
            a = R(0x6B) & 0xFC;
        } else {
            R(0x50) = (uint8_t)(R(0x50) << 2);
            a = R(0x6B) & 0xF3;
        }
    } else if (!(R(0x67) & 0x02)) {
        R(0x50) = (uint8_t)(R(0x50) << 4);
        a = R(0x6B) & 0xCF;
    } else {
        R(0x50) = (uint8_t)(R(0x50) << 6);
        a = R(0x6B) & 0x3F;
    }
    R(0x50) = a | R(0x50);
    R(0x136 + y) = R(0x50);
}

/* $B5CF: palettes of the same 4 cells -> attr_queue entries 0-3 */
static void QueueStripAttributes(void)
{
    R(0x4D) = 0;
    R(0x4E) = 0;
    R(0x4F) = (uint8_t)(C6 << 2);
    do {
        R(0x50) = prg((uint16_t)(0xF440 + (STRIP(R(0x4F)) >> 4)));
        R(0x51) = (uint8_t)(R(0x4D) << 1);
        uint8_t x = (uint8_t)((R(0x4D) << 2) + R(0x51));
        Camera_AttributeMerge(R(0xC7 + x), R(0xC8 + x));
        x = R(0x4E);
        R(0xEB + x) = R(0x55);
        R(0xEC + x) = R(0x56);
        R(0xED + x) = R(0x50);
        R(0x4D)++;
        R(0x4F)++;
        R(0x4E) = (uint8_t)(R(0x4E) + 3);
    } while (R(0x4E) < 0x0C);
}

static void StepDone(void) /* common tail: frame count, halfway, end */
{
    C6++;
    if (C6 >= 8) {
        C5 = 0;
        C6 = 0;
    } else if (C6 >= 4) {
        C5 &= 0x0F;
    }
}

/* $B38C: one frame of a step */
void Camera_Scroll(void)
{
    if (C5 & 0x03) { /* vertical */
        if (C6 < 4) {
            R(0x4D) = 0;
            R(0x4E) = 0;
            uint8_t y = (uint8_t)((C2 >> 4) << 1);
            set_zp_ptr(0x55, ptr_at(0xB4CD, y));
            if (!(C5 & 0x01))
                set_zp_ptr(0x55, ptr_at(0xB4E5, y));
            do {
                R(0x4F) = (uint8_t)((ram.camera_x[0] & 0xF0) >> 3);
                R(0x4F) = (uint8_t)((C6 << 3) + R(0x4F));
                uint8_t a = (uint8_t)((((R(0x4E) << 1) + R(0x4F)) & 0x1F) + R(0x55));
                R(0xC7 + R(0x4D)) = a;
                R(0xC8 + R(0x4D)) = R(0x56);
                R(0x4E)++;
                R(0x4D) = (uint8_t)(R(0x4D) + 6);
            } while (R(0x4D) < 0x18);
            QueueStripMetatiles();
            QueueStripAttributes();
            ram.nmi_requests = 0x10;
        }
        /* 2 px per frame, on the low byte only, as the original */
        ram.camera_y[0] = (uint8_t)(ram.camera_y[0] - 2);
        if (!(C5 & 0x01))
            ram.camera_y[0] = (uint8_t)(ram.camera_y[0] + 4);
        StepDone();
        return;
    }
    /* horizontal */
    if (C6 < 4) {
        R(0x4D) = 0;
        R(0x4E) = 0;
        do {
            R(0x4F) = (uint8_t)(ram.camera_y[0] >> 4);
            uint8_t x = (uint8_t)((((C6 << 2) + R(0x4E) + R(0x4F)) & 0xFF) << 1);
            set_zp_ptr(0x55, ptr_at(0xB4FD, x));
            uint16_t addr = (uint16_t)(zp_ptr(0x55) + (uint8_t)((C3 >> 4) << 1));
            R(0xC7 + R(0x4D)) = (uint8_t)addr;
            R(0xC8 + R(0x4D)) = (uint8_t)(addr >> 8);
            R(0x4E)++;
            R(0x4D) = (uint8_t)(R(0x4D) + 6);
        } while (R(0x4D) < 0x18);
        if (C6 >= 3) {
            R(0xD9) = 0xFF;
            R(0xDA) = 0xFF;
        }
        QueueStripMetatiles();
        QueueStripAttributes();
        ram.nmi_requests = 0x10;
    }
    uint16_t cx = (uint16_t)((ram.camera_x[0] | ram.camera_x[1] << 8) - 2);
    if (!(C5 & 0x04))
        cx = (uint16_t)(cx + 4);
    ram.camera_x[0] = (uint8_t)cx;
    ram.camera_x[1] = (uint8_t)(cx >> 8);
    StepDone();
}

void UpdateCamera(void)
{
    if (C5 || (CameraStart() && (Camera_FillStrip(), true)))
        Camera_Scroll();
    ram.scroll_x = ram.camera_x[0];
    ram.scroll_y = ram.camera_y[0];
}

/* ---- status digits ($D2AD) ---- */

/* $D368: 24-bit $4D-$4F /= $50, remainder in $67 */
static void Divide24(void)
{
    uint32_t v = (uint32_t)(R(0x4D) | R(0x4E) << 8 | R(0x4F) << 16);
    R(0x67) = (uint8_t)(v % R(0x50));
    v /= R(0x50);
    R(0x4D) = (uint8_t)v;
    R(0x4E) = (uint8_t)(v >> 8);
    R(0x4F) = (uint8_t)(v >> 16);
}

/* $D337: $AB-$AD -> decimal digits at $0100 (ones first), at least 6 */
static void ScoreToDigits(void)
{
    R(0x4D) = R(0xAB);
    R(0x4E) = R(0xAC);
    R(0x4F) = R(0xAD);
    R(0x50) = 0x0A;
    uint8_t x = 0;
    do {
        Divide24();
        R(0x100 + x) = R(0x67);
        x++;
    } while (R(0x4D) | R(0x4E) | R(0x4F));
    for (; x < 6; x++)
        R(0x100 + x) = 0;
}

void UpdateStatusSprites(void)
{
    if (!(ram.frame_phase & 0x07))
        return;
    uint8_t x = PlayerOffset();
    R(0xAB) = R(0x78 + x);
    R(0xAC) = R(0x79 + x);
    R(0xAD) = R(0x7A + x);
    ScoreToDigits();
    /* digit tiles from $D38E: score in sprites 1-6, diamonds 9-11, time 16-18 */
    x = 0;
    for (int8_t d = 5; d >= 0; d--, x = (uint8_t)(x + 4)) {
        R(0x4D) = (uint8_t)d;
        R(0x205 + x) = prg((uint16_t)(0xD38E + R(0x100 + d)));
    }
    R(0x4D) = 0xFF;
    x = 0x20;
    for (int8_t d = 2; d >= 0; d--, x = (uint8_t)(x + 4)) {
        R(0x4D) = (uint8_t)d;
        R(0x209 + x) = prg((uint16_t)(0xD38E + ram.diamonds_needed[d]));
    }
    R(0x4D) = 0xFF;
    if (ram.exit_open) { /* the diamond row flashes */
        R(0x67) = (uint8_t)((ram.frame_phase & 0x18) >> 3);
        x = 0x20;
        for (int i = 0; i < 6; i++, x = (uint8_t)(x + 4))
            R(0x202 + x) = (R(0x202 + x) & 0xF0) | R(0x67);
    }
    x = 0x40;
    for (int8_t d = 2; d >= 0; d--, x = (uint8_t)(x + 4)) {
        R(0x4D) = (uint8_t)d;
        R(0x201 + x) = prg((uint16_t)(0xD38E + ram.time_left[d]));
    }
    R(0x4D) = 0xFF;
}

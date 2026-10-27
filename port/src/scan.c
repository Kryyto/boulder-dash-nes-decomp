/* Cave simulation: ScanCave ($C413) and everything it calls.
 *
 * Faithful to the original, including its RAM-held loop state: the row ($4D),
 * column ($4E) and row pointer ($55/56) live in RAM, and a routine like
 * KillPlayer overwrites them in the middle of a scan, so the C does too.
 */
#include "bd.h"

#define ROW   R(0x4D)
#define COL   R(0x4E)
#define CELL(off) (*ind_y(0x55, (uint8_t)(off)))  /* cell in the current row ($55) */

enum probe { PROBE_EMPTY, PROBE_AMOEBA, PROBE_OTHER };

static void ExplodeToSpace(void);
static void ExplodeToDiamonds(void);

/* $57/58 = row above the current one */
static void PointAbove(void) { set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x55) - 0x28)); }

/* $C60F: is a boulder, diamond or diamond-explosion resting above? */
static bool FallerAbove(void)
{
    PointAbove();
    uint8_t t = *ind_y(0x57, COL) & 0xF0;
    return t == 0x70 || t == 0x80 || t == 0x90;
}

/* $C82B: probes used by fireflies and butterflies */
static enum probe ProbeValue(uint8_t t)
{
    if (t == 0x00)
        return PROBE_EMPTY;
    return t == 0xD0 ? PROBE_AMOEBA : PROBE_OTHER;
}
static enum probe ProbeUp(void)    { PointAbove(); return ProbeValue(*ind_y(0x57, COL)); }
static enum probe ProbeDown(void)  { return ProbeValue(CELL(COL + 0x28)); }
static enum probe ProbeLeft(void)  { return ProbeValue(CELL(COL - 1)); }
static enum probe ProbeRight(void) { return ProbeValue(CELL(COL + 1)); }

/* 24-bit little-endian compare: a >= b */
static bool ge24(const uint8_t *a, const uint8_t *b)
{
    for (int i = 2; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i];
    return true;
}

void AddScore(uint8_t points)
{
    if (ram.play_mode & 0x80) /* no score in the attract demo */
        return;
    uint8_t x = PlayerOffset();
    uint8_t *score = &R(0x78 + x);      /* 24-bit */
    uint8_t *next_life = &R(0x7B + x);  /* score of the next extra life */
    unsigned sum = score[0] + points;
    score[0] = (uint8_t)sum;
    sum = score[1] + (sum >> 8);
    score[1] = (uint8_t)sum;
    score[2] = (uint8_t)(score[2] + (sum >> 8));

    if (ge24(score, next_life)) { /* extra life every 2000 points */
        unsigned v = score[0] + 0xD0u;
        next_life[0] = (uint8_t)v;
        v = score[1] + 0x07u + (v >> 8);
        next_life[1] = (uint8_t)v;
        next_life[2] = (uint8_t)(score[2] + (v >> 8));
        uint8_t *lives = &R(0x77 + x);
        if (++*lives >= 10)
            *lives = 9;
        R(0xA0) = 0x20;
        PlayerScreenPos(&R(0xA1), &R(0xA2));
    }
    if (ge24(score, &R(0x8A))) { /* high score */
        R(0x8A) = score[0];
        R(0x8B) = score[1];
        R(0x8C) = score[2];
    }
}

/* $C93E */
static void AwardEnemyScore(void)
{
    uint8_t x = (uint8_t)((ram.cave_index >> 2) << 1);
    uint8_t first = prg((uint16_t)(0xC994 + x));
    AddScore(prg((uint16_t)(0xC995 + x)));
    AddScore(first);
    /* score popup at the enemy, in pixels */
    R(0xA6) = 0x20;
    uint8_t row = ROW >= 0x0C ? (uint8_t)(ROW + 1) : ROW;
    uint16_t py = (uint16_t)((uint8_t)(row + 3) << 4);
    uint16_t px = (uint16_t)((uint8_t)(COL + 4) << 4);
    R(0xA7) = (uint8_t)py;
    R(0xA8) = (uint8_t)(py >> 8);
    R(0xA9) = (uint8_t)px;
    R(0xAA) = (uint8_t)(px >> 8);
}

/* $C92C: can an explosion overwrite this tile? Counts a hit player in $4F. */
static bool ExplosionBlocked(uint8_t t)
{
    if (t == 0x30 || t == 0x40)
        return true;
    if (t == 0xE0)
        R(0x4F)++;
    return false;
}

/* $C83B: fill the 3x3 block around (ROW, COL) with tile $50 */
static void Explode3x3(void)
{
    uint8_t v = R(0x50);
    CELL(COL) = v;
    QueueCellRedraw(v, ROW, COL);
    static const int8_t dx[3] = {+1, -1, 0};
    for (int i = 0; i < 2; i++) { /* right, left */
        uint8_t c = (uint8_t)(COL + dx[i]);
        if (!ExplosionBlocked(CELL(c))) {
            CELL(c) = v;
            QueueCellRedraw(v, ROW, c);
        }
    }
    PointAbove();
    for (int dy = -1; dy <= 1; dy += 2) { /* row above, then (after +$50) row below */
        static const int8_t order[3] = {0, -1, +1};
        for (int i = 0; i < 3; i++) {
            uint8_t c = (uint8_t)(COL + order[i]);
            if (!ExplosionBlocked(*ind_y(0x57, c))) {
                *ind_y(0x57, c) = v;
                QueueCellRedraw(v, (uint8_t)(ROW + dy), c);
            }
        }
        if (dy < 0)
            set_zp_ptr(0x57, (uint16_t)(zp_ptr(0x57) + 0x50));
    }
    PlaySoundEffect(2);
}

static void ExplodeToSpace(void) /* $C7CD: fireflies (and the player) */
{
    R(0xB6) = 0x10;
    R(0x4F) = 0;
    R(0x50) = 0xA0;
    Explode3x3();
    if (R(0x4F))
        KillPlayer();
}

static void ExplodeToDiamonds(void) /* $C7E4: butterflies */
{
    R(0x4F) = 0;
    uint8_t slot = R(0xFD) & 0x07;
    R(0x50) = (uint8_t)(slot | 0x90);
    ram.explosion_timers[slot] = 0x10;
    R(0xFD)++;
    Explode3x3();
    if (R(0x4F))
        KillPlayer();
}

void KillPlayer(void)
{
    ram.player_phase = (uint8_t)((ram.player_phase & 0xF0) | 0x03);
    ram.player_phase_timer = 0x50;
    R(0x97) = 0x00; /* death jump velocity */
    R(0x98) = 0xFC;
    R(0x99) = 0xFF;
    R(0xC4) = 0x01;
    uint8_t row, col;
    GetPlayerCell(&row, &col);
    ROW = row;
    COL = col;
    set_zp_ptr(0x55, CaveRowPointer(row));
    ExplodeToSpace();
    Sound_Play(1);
}

/* ---- enemies ---- */

/* $C7B6 and friends: move an enemy to a neighbour cell as tile `v` */
enum dir { MOVE_RIGHT, MOVE_UP, MOVE_LEFT, MOVE_DOWN };

static void MoveEnemy(enum dir d, uint8_t v)
{
    uint8_t row = ROW, col = COL;
    R(0x50) = v;
    switch (d) {
    case MOVE_RIGHT: CELL(COL + 1) = v; col = (uint8_t)(COL + 1); break;
    case MOVE_UP:    PointAbove(); *ind_y(0x57, COL) = v; row = (uint8_t)(ROW - 1); break;
    case MOVE_LEFT:  CELL(COL - 1) = v; col = (uint8_t)(COL - 1); break;
    case MOVE_DOWN:  CELL(COL + 0x28) = v; row = (uint8_t)(ROW + 1); break;
    }
    QueueCellRedraw(v & 0xF0, row, col);
    QueueCellRedraw(0x00, ROW, COL);
    CELL(COL) = 0x00;
}

/* Tile values written when moving. Moving left/up sets bit 3 (the scan will
 * reach that cell again this pass); the low bits are the new heading. */
static void Cell_Firefly(void) /* $C632: turns left first */
{
    if (CELL(COL) & 0x08)
        return;
    PointAbove();
    uint8_t above = *ind_y(0x57, COL) & 0xF0;
    if (above == 0x70 || above == 0x80) {
        AwardEnemyScore();
        ExplodeToSpace();
        return;
    }
    switch (CELL(COL) & 0x0F) {
    case 1:
        if (ProbeUp() == PROBE_EMPTY) { MoveEnemy(MOVE_UP, 0xB8); return; }
        if (ProbeRight() == PROBE_EMPTY) { MoveEnemy(MOVE_RIGHT, 0xB1); return; }
        CELL(COL) = 0xB2;
        return;
    case 2:
        if (ProbeRight() == PROBE_EMPTY) { MoveEnemy(MOVE_RIGHT, 0xB1); return; }
        if (ProbeDown() == PROBE_EMPTY) { MoveEnemy(MOVE_DOWN, 0xB2); return; }
        CELL(COL) = 0xB3;
        return;
    case 3:
        if (ProbeDown() == PROBE_EMPTY) { MoveEnemy(MOVE_DOWN, 0xB2); return; }
        if (ProbeLeft() == PROBE_EMPTY) { MoveEnemy(MOVE_LEFT, 0xBB); return; }
        CELL(COL) = 0xB0;
        return;
    default:
        if (ProbeLeft() == PROBE_EMPTY) { MoveEnemy(MOVE_LEFT, 0xBB); return; }
        if (ProbeUp() == PROBE_EMPTY) { MoveEnemy(MOVE_UP, 0xB8); return; }
        CELL(COL) = 0xB1;
        return;
    }
}

static void Cell_Butterfly(void) /* $C6CD: turns right first, dies on amoeba */
{
    if (CELL(COL) & 0x08)
        return;
    PointAbove();
    uint8_t above = *ind_y(0x57, COL) & 0xF0;
    if (above == 0x70 || above == 0x80) {
        ExplodeToDiamonds();
        return;
    }
    static const struct { enum probe (*probe)(void); enum dir d; uint8_t v; } step[4][2] = {
        {{ProbeRight, MOVE_RIGHT, 0xC1}, {ProbeUp, MOVE_UP, 0xC8}},     /* heading 0 */
        {{ProbeDown, MOVE_DOWN, 0xC2}, {ProbeRight, MOVE_RIGHT, 0xC1}}, /* heading 1 */
        {{ProbeLeft, MOVE_LEFT, 0xCB}, {ProbeDown, MOVE_DOWN, 0xC2}},   /* heading 2 */
        {{ProbeUp, MOVE_UP, 0xC8}, {ProbeLeft, MOVE_LEFT, 0xCB}},       /* heading 3 */
    };
    static const uint8_t turn[4] = {0xC3, 0xC0, 0xC1, 0xC2};
    uint8_t h = CELL(COL) & 0x03;
    for (int i = 0; i < 2; i++) {
        enum probe p = step[h][i].probe();
        if (p == PROBE_AMOEBA) { ExplodeToDiamonds(); return; }
        if (p == PROBE_EMPTY) { MoveEnemy(step[h][i].d, step[h][i].v); return; }
    }
    CELL(COL) = turn[h];
}

/* ---- MagicWallActivate ($CDC6): redraw the cave's magic wall cells ---- */
void MagicWallActivate(void)
{
    uint16_t list;
    switch (ram.cave_index) {
    case 0x07: list = 0xCE19; break;
    case 0x0E: list = 0xCE2A; break;
    case 0x0F: list = 0xCE31; break;
    default:   list = 0xCE4A; break;
    }
    set_zp_ptr(0x6A, list);
    for (uint8_t y = 0; !(rd((uint16_t)(zp_ptr(0x6A) + y)) & 0x80); y = (uint8_t)(y + 2)) {
        uint8_t row = rd((uint16_t)(zp_ptr(0x6A) + y));
        uint8_t col = rd((uint16_t)(zp_ptr(0x6A) + (uint8_t)(y + 1)));
        if (GetCell(row, col) == 0x60)
            QueueCellRedraw(0x60, row, col);
    }
}

/* ---- gravity ---- */

static void RedrawVacatedUnlessRefilled(void)
{
    if (!FallerAbove())
        QueueCellRedraw(0x00, ROW, COL);
}

/* $C468 */
static void ProcessCell(void)
{
    uint8_t type = CELL(COL) & 0xF0;
    if (type == 0xB0) { Cell_Firefly(); return; }
    if (type == 0xC0) { Cell_Butterfly(); return; }
    if (type != 0x70 && type != 0x80)
        return;
    if (CELL(COL) & 0x08) /* already moved this pass */
        return;

    R(0x4F) = (uint8_t)(COL + 0x28);
    uint8_t below = CELL(R(0x4F));
    if (below == 0x00) { /* fall */
        if ((CELL(COL) & 0x0F) == 0)
            PlaySoundEffect(3);
        RedrawVacatedUnlessRefilled();
        QueueCellRedraw(CELL(COL) & 0xF0, (uint8_t)(ROW + 1), COL);
        CELL(R(0x4F)) = (uint8_t)((CELL(COL) & 0xF0) | 0x01);
        CELL(COL) = 0x10;
        return;
    }
    if (below == 0x60) { /* magic wall: a falling boulder passes through as a diamond */
        if (CELL(COL) == 0x71 && !(ram.magic_wall_state & 0x02)) {
            ram.magic_wall_state = 0x01;
            MagicWallActivate();
            R(0x50) = (uint8_t)(R(0x4F) + 0x28);
            if (CELL(R(0x50)) == 0x00) {
                CELL(COL) = 0x10;
                QueueCellRedraw(0x00, ROW, COL);
                CELL(R(0x50)) = 0x81;
                QueueCellRedraw(0x80, (uint8_t)(ROW + 2), COL);
                return;
            }
        }
    } else if (below == 0xE0) {
        if (CELL(COL) & 0x01) { /* falling onto the player */
            KillPlayer();
            return;
        }
    }

    /* landed: stop falling (boulders make a sound) */
    if (CELL(COL) & 0x0F) {
        uint8_t t = CELL(COL) & 0xF0;
        CELL(COL) = t;
        if (t != 0x80)
            PlaySoundEffect(4);
    }

    /* roll off round things (exact tiles: resting boulder/diamond, brick, magic wall) */
    below = CELL(COL + 0x28);
    if (below != 0x70 && below != 0x80 && below != 0x50 && below != 0x60)
        return;
    if (CELL(COL - 1) == 0x00 && CELL(COL - 1 + 0x28) == 0x00) {
        PointAbove();
        uint8_t t = *ind_y(0x57, COL - 1) & 0xF0;
        if (t != 0x70 && t != 0x80) { /* roll left */
            uint8_t v = CELL(COL);
            CELL(COL - 1) = v | 0x09;
            CELL(COL) = 0x10;
            QueueCellRedraw(v & 0xF0, ROW, (uint8_t)(COL - 1));
            RedrawVacatedUnlessRefilled();
            return;
        }
    }
    if (CELL(COL + 1) != 0x00 || CELL(COL + 1 + 0x28) != 0x00)
        return;
    PointAbove();
    uint8_t t = *ind_y(0x57, COL + 1) & 0xF0;
    if (t == 0x70 || t == 0x80)
        return;
    uint8_t v = CELL(COL); /* roll right */
    CELL(COL + 1) = v | 0x01;
    CELL(COL) = 0x10;
    QueueCellRedraw(v & 0xF0, ROW, (uint8_t)(COL + 1));
    RedrawVacatedUnlessRefilled();
}

/* $C9D3: second pass. Returns the Y register: after a redraw the original
 * leaves Y = row, and ScanCave2 keeps scanning from that column. */
static uint8_t ProcessCell2(uint8_t y)
{
    uint8_t *c = &CELL(y);
    switch (*c & 0xF0) {
    case 0x70: case 0x80: case 0xB0: case 0xC0:
        *c &= 0xF7; /* clear "processed" */
        return y;
    case 0x10:
        *c = 0x00; /* vacated -> space */
        return y;
    case 0x90: /* explosion -> diamond, when its timer has run out */
        if (ram.explosion_timers[*c & 0x07] == 0) {
            *c = 0x80;
            QueueCellRedraw(0x80, ROW, y);
            return ROW;
        }
        return y;
    case 0xA0: /* explosion -> space */
        if (R(0xB6) == 0) {
            *c = 0x10;
            QueueCellRedraw(0x00, ROW, y);
            return ROW;
        }
        return y;
    case 0xD0: { /* amoeba: diamonds once enclosed */
        uint8_t v;
        if (ram.amoeba_state == 0)
            v = 0x80;
        else if (ram.amoeba_state == 0xFF)
            v = 0xF0;
        else
            return y;
        *c = v;
        QueueCellRedraw(v, ROW, y);
        return ROW;
    }
    default:
        return y;
    }
}

static void ScanCave1(uint8_t first_row) /* $C444: 5 rows bottom-up, columns 38..1 */
{
    ROW = first_row;
    R(0x54) = 5;
    do {
        COL = 0x26;
        set_zp_ptr(0x55, CaveRowPointer(ROW));
        do
            ProcessCell();
        while (--COL != 0);
        ROW--;
    } while (--R(0x54) != 0);
}

static void ScanCave2(uint8_t first_row, uint8_t rows) /* $C9B2: top-down */
{
    ROW = first_row;
    R(0x54) = rows;
    do {
        set_zp_ptr(0x55, CaveRowPointer(ROW));
        uint8_t y = 0x26;
        do
            y = ProcessCell2(y);
        while (--y != 0);
        ROW++;
    } while (--R(0x54) != 0);
}

void ScanCave(void)
{
    uint8_t phase = ram.frame_phase & 0x07;
    JumpTableInline(0xC41A, phase);
    switch (phase) {
    case 0: break;
    case 1: ScanCave1(0x14); break;
    case 2: ScanCave1(0x0F); break;
    case 3: ScanCave1(0x0A); break;
    case 4: ScanCave1(0x05); break;
    case 5: ScanCave2(0x01, 7); break;
    case 6: ScanCave2(0x08, 7); break;
    case 7: ScanCave2(0x0F, 6); break;
    }
}

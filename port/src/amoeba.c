/* Amoeba ($CE51), magic wall timer ($CDAE) and amoeba hum ($C044).
 *
 * The NES amoeba grows deterministically: every 8 frames a walker follows
 * its edge for 25 steps. Its heading is in $4E ($00 left-first, $40 up,
 * $80 right, $C0 down), its cell in $4F (row) / $50 (col), saved between
 * runs in amoeba_walker / $BC. Each empty or dirt cell it probes counts down
 * amoeba_grow_countdown; at zero that cell becomes amoeba. amoeba_state
 * counts walker runs in bits 0-1 and sets bit 7 when there was room to grow;
 * after 4 runs without room it becomes 0 and ScanCave turns the amoeba
 * into diamonds.
 */
#include "bd.h"

enum probe { P_OTHER, P_ROOM, P_AMOEBA };

static enum probe Probe(void) /* $CF8B on the cell at ($4F, $50) */
{
    uint8_t t = GetCell(R(0x4F), R(0x50));
    if (t == 0xD0)
        return P_AMOEBA;
    return (t == 0x00 || t == 0x20) ? P_ROOM : P_OTHER;
}

/* $CF5E-$CF82: move the probe one cell and look at it */
static enum probe StepLeft(void)  { R(0x50)--; return Probe(); }
static enum probe StepUp(void)    { R(0x4F)--; return Probe(); }
static enum probe StepRight(void) { R(0x50)++; return Probe(); }
static enum probe StepDown(void)  { R(0x4F)++; return Probe(); }

/* $CF99: true once the amoeba grew into the probed cell */
static bool TryGrow(void)
{
    ram.amoeba_state |= 0x80;
    if (--ram.amoeba_grow_countdown != 0)
        return false;
    SetCell(0xD0, R(0x4F), R(0x50));
    QueueCellRedraw(0xD0, R(0x4F), R(0x50));
    ram.amoeba_grow_interval = (uint8_t)(ram.amoeba_grow_interval - 4);
    ram.amoeba_grow_countdown = ram.amoeba_grow_interval;
    ram.amoeba_state = 0x80;
    return true;
}

/* One probe of the walk. Returns 1 to stop the whole update (it grew),
 * 0 to go on; *turned says the walker stepped onto amoeba and turned. */
static int Try(enum probe (*step)(void), uint8_t heading_if_amoeba, int *turned)
{
    enum probe p = step();
    if (p == P_AMOEBA) {
        R(0x4E) = heading_if_amoeba;
        *turned = 1;
        return 0;
    }
    if (p == P_ROOM && TryGrow())
        return 1;
    return 0;
}

void AmoebaUpdate(void)
{
    if (ram.amoeba_state == 0x00 || ram.amoeba_state == 0xFF)
        return;
    if (ram.frame_phase & 0x07)
        return;
    if ((ram.amoeba_state & 0x03) == 0) { /* every 4th run: restart from the origin */
        ram.amoeba_state = 0;
        ram.amoeba_walker[0] = ram.amoeba_origin[0];
        ram.amoeba_walker[1] = ram.amoeba_origin[1];
    }
    R(0x4D) = 0x19;
    R(0x4E) = ram.amoeba_walker[0] & 0xC0;
    R(0x4F) = ram.amoeba_walker[0] & 0x3F;
    R(0x50) = ram.amoeba_walker[1];

    do {
        int turned = 0;
        /* Enter the probe sequence at the current heading, as the original's
         * dispatch does; each failed probe is undone before the next. */
        switch (R(0x4E)) {
        default: /* $00 */
            if (Try(StepLeft, 0xC0, &turned)) return;
            if (turned) break;
            R(0x50)++;
            /* fall through */
        case 0x40:
            if (Try(StepUp, 0x00, &turned)) return;
            if (turned) break;
            R(0x4F)++;
            /* fall through */
        case 0x80:
            if (Try(StepRight, 0x40, &turned)) return;
            if (turned) break;
            R(0x50)--;
            /* fall through */
        case 0xC0:
            if (Try(StepDown, 0x80, &turned)) return;
            if (turned) break;
            R(0x4F)--;
            /* then once more round: left, up, right */
            if (Try(StepLeft, 0xC0, &turned)) return;
            if (turned) break;
            R(0x50)++;
            if (Try(StepUp, 0x00, &turned)) return;
            if (turned) break;
            R(0x4F)++;
            if (Try(StepRight, 0x40, &turned)) return;
            if (turned) break;
            R(0x50)--;
            break;
        }
    } while (--R(0x4D) != 0);

    ram.amoeba_walker[0] = R(0x4F) | R(0x4E);
    ram.amoeba_walker[1] = R(0x50);
    ram.amoeba_state = (uint8_t)((ram.amoeba_state + 1) & 0x83);
}

void MagicWallTimer(void)
{
    if (ram.frame_phase & 0x0F)
        return;
    if (!(ram.magic_wall_state & 0x01))
        return;
    if (--ram.magic_wall_timer != 0)
        return;
    ram.magic_wall_state = 0x02; /* expired: boulders no longer pass */
    MagicWallActivate();
}

void AmoebaSound(void)
{
    if (ram.cave_index != 0x06 && ram.cave_index != 0x0C && ram.cave_index != 0x16)
        return;
    if (ram.sfx_request & 0x80)
        return;
    if (ram.amoeba_state == 0x00 || ram.amoeba_state == 0xFF)
        return;
    if (ram.frame_phase & 0x07)
        return;
    PlaySoundEffect(6);
}

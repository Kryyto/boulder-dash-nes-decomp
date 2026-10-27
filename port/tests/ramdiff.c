/* RAM-diff test: replay fixtures recorded by tools/record.py against the C port.
 *
 * For each recorded call: load the RAM the original had at entry, run the C
 * routine, and compare all 2 KB with the RAM the original had at its RTS.
 * Only the stack bytes below the entry stack pointer are ignored (the
 * original's JSRs write return addresses there; the C port has no 6502 stack).
 *
 * Hardware accesses are compared too. Version 3 fixtures log every access
 * (PPU, APU, joypads, mapper), reads with the value read: the port's writes
 * must be the same sequence, and its reads get the values the original read
 * (the hardware state at entry isn't in the fixture). Version 2 fixtures log
 * APU writes only, so only those are compared.
 *
 * Usage: ramdiff ROM FIXTURES [-v] [-n MAXFAIL] [-r ROUTINE]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bd.h"

typedef struct {
    uint16_t addr;
    const char *name;
    void (*fn)(void);
    unsigned run, pass;
} Routine;

static Routine routines[] = {
    {0xB789, "LoadCave", LoadCave, 0, 0},
    {0xB8AE, "ApplyCavePatches", ApplyCavePatches, 0, 0},
    {0xC413, "ScanCave", ScanCave, 0, 0},
    {0xC06B, "PlayerInput", PlayerInput, 0, 0},
    {0xC373, "PlayerHazards", PlayerHazards, 0, 0},
    {0xCFC6, "TickTime", TickTime, 0, 0},
    {0xD060, "CopyPlayerInput", CopyPlayerInput, 0, 0},
    {0xCE51, "AmoebaUpdate", AmoebaUpdate, 0, 0},
    {0xCDAE, "MagicWallTimer", MagicWallTimer, 0, 0},
    {0xC044, "AmoebaSound", AmoebaSound, 0, 0},
    {0xD398, "PlayerSprite", PlayerSprite, 0, 0},
    {0xB197, "UpdateCamera", UpdateCamera, 0, 0},
    {0xD2AD, "UpdateStatusSprites", UpdateStatusSprites, 0, 0},
    {0x8006, "Sound_Update", Sound_Update, 0, 0},
    {0x8000, "Sound_Init", Sound_Init, 0, 0},
    {0xAB5E, "State0A_CaveIntro", State0A_CaveIntro, 0, 0},
    {0xAB76, "State0B_CavePlay", State0B_CavePlay, 0, 0},
    {0xACA1, "State0C_CaveCleared", State0C_CaveCleared, 0, 0},
    {0xADF4, "State0D_LostLife", State0D_LostLife, 0, 0},
    {0xA2C7, "NmiHandler", NmiHandler, 0, 0},
    {0xA022, "InitGame", InitGame, 0, 0},
    {0xA12B, "ReadJoypads", ReadJoypads, 0, 0},
    {0xA618, "State00_BootScreen", State00_BootScreen, 0, 0},
    {0xA67E, "State01_Title", State01_Title, 0, 0},
    {0xA686, "State02_TitleMenu", State02_TitleMenu, 0, 0},
    {0xA82D, "State03_Password", State03_Password, 0, 0},
    {0xA9E1, "State04_NextStage", State04_NextStage, 0, 0},
    {0xA9E7, "State05_WorldSelectInit", State05_WorldSelectInit, 0, 0},
    {0xA9F2, "State06_WorldSelect", State06_WorldSelect, 0, 0},
    {0xAA2B, "State07_StageIntroInit", State07_StageIntroInit, 0, 0},
    {0xAA36, "State08_StageIntro", State08_StageIntro, 0, 0},
    {0xAB2E, "State09_CaveSetup", State09_CaveSetup, 0, 0},
    {0xAEA3, "State0E_GameOver", State0E_GameOver, 0, 0},
    {0xEABB, "State0F_Ending", State0F_Ending, 0, 0},
    {0xEC82, "State10_EndingScene", State10_EndingScene, 0, 0},
    {0xED09, "State11_Credits", State11_Credits, 0, 0},
    {0xEFF5, "State12_NextLevel", State12_NextLevel, 0, 0},
};
#define NROUTINES (sizeof routines / sizeof routines[0])

enum { REGS = 5, RAMSZ = 0x800, RECORD = 2 + 2 * (REGS + RAMSZ), MAXLOG = 1 << 16 };

typedef struct { uint16_t addr; uint8_t value, read; } Access;
static Access port_log[MAXLOG], orig_log[MAXLOG];
static unsigned port_log_n, orig_n, version;
static unsigned replay_pos[8 + 0x18 + 1]; /* next recorded read to hand out, per register */

static bool is_apu_audio(uint16_t a) { return (a >= 0x4000 && a <= 0x4013) || a == 0x4015 || a == 0x4017; }

static void capture(uint16_t addr, uint8_t value, bool read)
{
    if (version == 2 && (read || !is_apu_audio(addr)))
        return; /* version 2 fixtures have the APU writes only */
    if (read)
        return; /* reads come from the fixture: only writes are compared */
    if (port_log_n < MAXLOG)
        port_log[port_log_n] = (Access){addr, value, 0};
    port_log_n++;
}

static int reg_slot(uint16_t a) { return a < 0x4000 ? (a & 7) : a <= 0x4018 ? 8 + (a - 0x4000) : -1; }

/* the port reads a register: the next value the original read from it */
static bool replay(uint16_t addr, uint8_t *value)
{
    int s = reg_slot(addr);
    if (version != 3 || s < 0)
        return false;
    for (unsigned k = replay_pos[s]; k < orig_n; k++)
        if (orig_log[k].read && reg_slot(orig_log[k].addr) == s) {
            *value = orig_log[k].value;
            replay_pos[s] = k + 1;
            return true;
        }
    replay_pos[s] = orig_n;
    return false;
}

static unsigned orig_writes(Access *out) /* the original's writes, in order */
{
    unsigned n = 0;
    for (unsigned k = 0; k < orig_n; k++)
        if (!orig_log[k].read)
            out[n++] = orig_log[k];
    return n;
}

static const char *ram_name(unsigned a)
{
    /* nearest named field at or below a (from the generated layout) */
    static char buf[64];
    static const struct { unsigned a; const char *n; } names[] = {
#define F(field) {offsetof(BdRam, field), #field}
        F(cave_index), F(player_phase), F(player_phase_timer), F(player_y), F(player_x),
        F(diamonds_needed), F(time_left), F(magic_wall_state), F(amoeba_state),
        F(metatile_queue), F(attr_queue), F(frame_phase), F(snd_ram), F(music_request),
        F(sfx_request), F(cave_grid), F(redraw_count), F(redraw_index), F(redraw_list),
        F(explosion_timers), F(score), F(lives),
#undef F
    };
    const char *best = NULL;
    unsigned ba = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].a <= a && names[i].a >= ba && a - names[i].a < 0x400) {
            ba = names[i].a;
            best = names[i].n;
        }
    if (a >= 0x3E0 && a < 0x3E0 + 880) {
        snprintf(buf, sizeof buf, "cave_grid[row %u col %u]", (a - 0x3E0) / 40, (a - 0x3E0) % 40);
        return buf;
    }
    if (best && a - ba < 0x40) {
        snprintf(buf, sizeof buf, "%s+%u", best, a - ba);
        return buf;
    }
    return "";
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s ROM FIXTURES [-v] [-n MAXFAIL] [-r ROUTINE]\n", argv[0]);
        return 2;
    }
    int verbose = 0, maxfail = 5;
    const char *only = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            maxfail = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            only = argv[++i];
    }
    char err[256];
    if (!bd_rom_load(argv[1], err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    char magic[4];
    uint32_t hdr[2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "BDFX", 4) || fread(hdr, 4, 2, f) != 2 ||
        hdr[0] < 1 || hdr[0] > 3) {
        fprintf(stderr, "%s: not a version 1, 2 or 3 fixture file\n", argv[2]);
        return 2;
    }
    version = hdr[0];
    bd_io_trace = capture;
    bd_io_replay = replay;
    static Access writes[MAXLOG];
    static uint8_t rec[RECORD], raw[MAXLOG * 4];
    int shown = 0;
    for (uint32_t n = 0; n < hdr[1]; n++) {
        if (fread(rec, 1, RECORD, f) != RECORD) {
            fprintf(stderr, "truncated fixture file at record %u\n", n);
            return 2;
        }
        orig_n = 0;
        if (version == 2) { /* u16 count, (u16 addr, u8 value) APU writes */
            uint8_t nb[2];
            if (fread(nb, 1, 2, f) != 2)
                return 2;
            orig_n = nb[0] | nb[1] << 8;
            if (orig_n > MAXLOG || fread(raw, 3, orig_n, f) != orig_n) {
                fprintf(stderr, "bad APU log at record %u\n", n);
                return 2;
            }
            for (unsigned k = 0; k < orig_n; k++)
                orig_log[k] = (Access){(uint16_t)(raw[3 * k] | raw[3 * k + 1] << 8), raw[3 * k + 2], 0};
        } else if (version == 3) { /* u32 count, (u16 addr, u8 value, u8 read) accesses */
            uint32_t cnt;
            if (fread(&cnt, 4, 1, f) != 1 || cnt > MAXLOG || fread(raw, 4, cnt, f) != cnt) {
                fprintf(stderr, "bad I/O log at record %u\n", n);
                return 2;
            }
            orig_n = cnt;
            for (unsigned k = 0; k < orig_n; k++)
                orig_log[k] = (Access){(uint16_t)(raw[4 * k] | raw[4 * k + 1] << 8), raw[4 * k + 2], raw[4 * k + 3]};
        }
        uint16_t addr = (uint16_t)(rec[0] | rec[1] << 8);
        const uint8_t *regs0 = rec + 2, *ram0 = regs0 + REGS;
        const uint8_t *ram1 = ram0 + RAMSZ + REGS;
        Routine *r = NULL;
        for (size_t i = 0; i < NROUTINES; i++)
            if (routines[i].addr == addr)
                r = &routines[i];
        if (!r || (only && strcmp(only, r->name)))
            continue;

        memcpy(ram.raw, ram0, RAMSZ);
        /* hardware as the game set it up, as far as RAM tells (for version 2
         * fixtures, whose reads aren't recorded: LoadCave reads CHR ROM) */
        hw_reset();
        hw.mmc1_ctrl = ram.mmc1_ctrl_shadow;
        hw.ctrl = ram.ppuctrl_shadow & 0xFB;
        memset(replay_pos, 0, sizeof replay_pos);
        port_log_n = 0;
        r->fn();
        r->run++;

        /* hardware writes: the same sequence */
        unsigned nw = orig_writes(writes);
        int apu_ok = 1;
        unsigned apu_bad = nw < port_log_n ? nw : port_log_n; /* where they part */
        if (version >= 2) {
            for (unsigned k = 0; k < nw && k < port_log_n; k++)
                if (port_log[k].addr != writes[k].addr || port_log[k].value != writes[k].value) {
                    apu_bad = k;
                    break;
                }
            apu_ok = port_log_n == nw && apu_bad == nw;
        }

        uint8_t s = regs0[4];
        unsigned diffs = 0, first = 0;
        for (unsigned a = 0; a < RAMSZ; a++) {
            if (a >= 0x100 && a < 0x200 && a <= 0x100u + s && a + 0x40 > 0x100u + s)
                continue; /* stack area written by the original's JSRs */
            if (ram.raw[a] != ram1[a] && diffs++ == 0)
                first = a;
        }
        if (!diffs && apu_ok) {
            r->pass++;
            continue;
        }
        if (shown++ < maxfail) {
            if (!apu_ok) {
                printf("FAIL %s record %u: hardware writes differ from write %u (original %u writes, port %u)\n",
                       r->name, n, apu_bad, nw, port_log_n);
                if (verbose)
                    for (unsigned k = apu_bad; k < apu_bad + 6 && (k < nw || k < port_log_n); k++)
                        printf("    write %u: original $%04X=%02X  port $%04X=%02X\n", k,
                               k < nw ? writes[k].addr : 0, k < nw ? writes[k].value : 0,
                               k < port_log_n ? port_log[k].addr : 0, k < port_log_n ? port_log[k].value : 0);
            }
            if (diffs)
                printf("FAIL %s record %u (frame_phase %u, cave %u): %u bytes differ, first $%04X %s\n",
                       r->name, n, ram0[0xFE] & 7, ram0[0x8D], diffs, first, ram_name(first));
            if (verbose) {
                unsigned k = 0;
                for (unsigned a = 0; a < RAMSZ && k < 24; a++)
                    if (ram.raw[a] != ram1[a] &&
                        !(a >= 0x100 && a < 0x200 && a <= 0x100u + s && a + 0x40 > 0x100u + s)) {
                        printf("    $%04X %-28s before %02X  original %02X  port %02X\n",
                               a, ram_name(a), ram0[a], ram1[a], ram.raw[a]);
                        k++;
                    }
            }
        }
    }
    fclose(f);
    int ok = 1;
    for (size_t i = 0; i < NROUTINES; i++) {
        Routine *r = &routines[i];
        if (!r->run)
            continue;
        printf("%-18s %5u / %5u match\n", r->name, r->pass, r->run);
        ok &= r->pass == r->run;
    }
    return ok ? 0 : 1;
}

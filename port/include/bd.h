/* Boulder Dash (NES) C port: shared state and helpers.
 *
 * The game state is the NES RAM itself (BdRam, generated from tools/hints.txt),
 * so ported routines can be checked byte-for-byte against the original with
 * the RAM fixtures recorded by tools/record.py. Game data (cave headers,
 * layouts, tables) is read from the user's ROM at run time, never embedded.
 */
#ifndef BD_H
#define BD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bd_ram.h"
#include "bd_syms.h"

extern BdRam ram;

/* RAM byte at a NES address (mirrored like the hardware). */
#define R(a) (ram.raw[(a) & 0x7FF])

/* ---- ROM (rom.c) ---- */
bool bd_rom_load(const char *path, char *err, size_t errlen);
uint8_t prg(uint16_t addr);               /* CPU read of $8000-$FFFF */
uint8_t chr(uint32_t offset);             /* CHR ROM byte */
/* CPU read through the bus: RAM below $0800, PRG ROM from $8000. */
uint8_t rd(uint16_t addr);

/* 16-bit pointer stored in zero page, and the byte it points to plus y,
 * like the 6502's (zp),y addressing. The game sometimes computes pointers
 * outside RAM (e.g. a row index past the cave), so this follows the bus:
 * RAM below $2000 (mirrored), PRG ROM from $8000 (writes dropped), and
 * nothing in between (reads 0, writes dropped), as in tools/nesemu.py. */
extern uint8_t bd_bus_scratch;
static inline uint16_t zp_ptr(uint8_t zp) { return (uint16_t)(R(zp) | R((uint8_t)(zp + 1)) << 8); }
static inline uint8_t *bus_ref(uint16_t a)
{
    if (a < 0x2000)
        return &ram.raw[a & 0x7FF];
    bd_bus_scratch = a >= 0x8000 ? prg(a) : 0;
    return &bd_bus_scratch;
}
static inline uint8_t *ind_y(uint8_t zp, uint8_t y) { return bus_ref((uint16_t)(zp_ptr(zp) + y)); }
static inline void set_zp_ptr(uint8_t zp, uint16_t v) { R(zp) = (uint8_t)v; R((uint8_t)(zp + 1)) = (uint8_t)(v >> 8); }

/* ---- hardware: PPU, MMC1, joypads (io.c) ---- */
typedef struct {
    uint8_t mmc1_shift, mmc1_ctrl, mmc1_chr0, mmc1_chr1, mmc1_prg;
    uint8_t ctrl, mask, status, oam_addr, x, w, buf;
    uint16_t v, t;
    uint8_t oam[256], ciram[0x800], pal[32];
    /* the scroll as the frame starts rendering (end of the NMI), and the
     * line from which the sprite-0 split's scroll applies (-1: none) */
    uint16_t frame_t;
    uint8_t frame_x, frame_ctrl, frame_mask;
    int split_line;
    uint8_t pad[2], pad_shift[2], strobe; /* joypads: buttons held, shift registers */
} BdHw;
extern BdHw hw;
void hw_reset(void);
void io_write(uint16_t addr, uint8_t value);        /* STA to $2000-$4017 or $8000+ */
uint8_t io_read(uint16_t addr);                     /* LDA from $2000-$4017 */
/* Every access (after it happens), and an optional source for read values. */
extern void (*bd_io_trace)(uint16_t addr, uint8_t value, bool read);
extern bool (*bd_io_replay)(uint16_t addr, uint8_t *value);
/* Where APU register writes go (default: nowhere). */
extern void (*bd_apu_write)(uint16_t addr, uint8_t value);
uint8_t ppu_peek(uint16_t addr);                    /* PPU address space, no side effects */
void ppu_frame_start(void);                         /* the NMI is done: the frame renders from here */
void ppu_sprite0_split(void);                       /* the game waited for sprite 0 (WaitSprite0Hit) */
void ppu_render(uint8_t out[240][256]);             /* NES colour indices of the frame */

/* ---- shared helpers (core.c) ---- */
uint8_t PlayerOffset(void);                         /* $A822: 0 or $0A */
void JumpTableInline(uint16_t table, uint8_t index); /* $A4CB: RAM side effects only */
uint16_t CaveRowPointer(uint8_t row);               /* entry of CaveRowPointers ($D08B) */
uint8_t GetCell(uint8_t row, uint8_t col);          /* $CC7D */
void GetPlayerCell(uint8_t *row, uint8_t *col);     /* $CC56 */
void SetCell(uint8_t tile, uint8_t row, uint8_t col);       /* $D077 */
bool QueueCellRedraw(uint8_t tile, uint8_t row, uint8_t col); /* $D0B7, true = list full */
void SetGameState(uint8_t state);                   /* $A258 */
void CopyPlayerInput(void);                         /* $D060: joy_held/pressed of the current player */
void PlayerScreenPos(uint8_t *y, uint8_t *x);       /* $CD9B: player position on screen */
void Sound_Play(uint8_t id);                        /* $811C: request a sound (sets music/sfx_request) */
void PlaySoundEffect(uint8_t effect);               /* $BFCA */

/* ---- sound driver (sound.c) ---- */
void Sound_Init(void);                              /* $8011 */
void Sound_Update(void);                            /* $81B4, called once per frame (NMI) */

/* ---- cave loading (cave.c) ---- */
uint8_t WorldStageToCave(uint8_t world_stage);      /* $B69A */
void BinToDec3(uint8_t value);                      /* $B843 */
void LoadCave(void);                                /* $B789 */
void ApplyCavePatches(void);                        /* $B8AE */

/* ---- cave simulation (scan.c) ---- */
void ScanCave(void);                                /* $C413 */
void MagicWallActivate(void);                       /* $CDC6: redraw the magic wall cells */
void KillPlayer(void);                              /* $C3C9 */
void AddScore(uint8_t points);                      /* $CCE2 */

/* ---- amoeba and magic wall (amoeba.c) ---- */
void AmoebaUpdate(void);                            /* $CE51 */
void MagicWallTimer(void);                          /* $CDAE */
void AmoebaSound(void);                             /* $C044 */

/* ---- graphics (gfx.c) ---- */
void DrawMetaspriteWorld(uint8_t attr_or, uint8_t ptr_hi, uint8_t ptr_lo); /* $A505 */
void PlayerSprite(void);                            /* $D398: player into oam_buf from $50 */
void HideUnusedSprites(void);                       /* $A5E0 */

/* ---- camera and status (camera.c) ---- */
void UpdateCamera(void);                            /* $B197: scroll + queue the new row/column */
void UpdateStatusSprites(void);                     /* $D2AD: score, diamonds, time digit sprites */
void Camera_FillStrip(void);                        /* $B245: row/column coming into view -> $0106 */
void Camera_Scroll(void);                           /* $B38C: one frame of a camera step */
void Camera_AttributeMerge(uint8_t lo, uint8_t hi); /* $B61A */
void Camera_MetatileVariant(uint8_t type);          /* $B580: $67/68 -> tiles of a cell type */

/* ---- NMI, frame waits, joypads, fades (nmi.c) ---- */
void NmiHandler(void);                              /* $A2C7: PPU updates, then Sound_Update */
void Nmi_FlushMetatiles4(void);                     /* $B0C1 (also called by the cave setup) */
/* What happens when the game waits for the next frame (WaitForNMI). Default:
 * NmiHandler. bdview replaces it to also draw and play the frame. */
extern void (*bd_frame_hook)(void);
void WaitForNMI(void);                              /* $A0C9 */
void WaitFrames(uint8_t n);                         /* $A4C0 */
void FadeOut(void);                                 /* $A48A: 12 frames */
void BlackoutNow(void);                             /* $A4F5 */

/* ---- PPU, mapper and joypad helpers (screen.c) ---- */
void SetPPUAddr(uint8_t hi, uint8_t lo);            /* $A1A6 */
void RestorePPUCtrlScroll(void);                    /* $A1AD */
void UploadPalette(void);                           /* $A0A2 */
void HideAllSprites(void);                          /* $A0DD */
void ClearNametables(void);                         /* $A0F9 */
void ClearScreen(void);                             /* $A0D6 */
void DisableRendering(void);                        /* $A193 */
void DrawPPUString(uint8_t hi, uint8_t lo);         /* $A1BD */
void MMC1_SetCtrl(uint8_t a);                       /* $A263 */
void MMC1_SetCHR0(uint8_t a);                       /* $A277 */
void MMC1_SetCHR1(uint8_t a);                       /* $A28B */
void ResetScroll(void);                             /* $A2F4 */
void ReadVramTo600(uint8_t bank, uint8_t hi, uint8_t lo); /* $A313 */
void LoadNametable(uint8_t a, uint8_t hi, uint8_t lo);    /* $A334 */
void LoadBGPalette(uint8_t hi, uint8_t lo);         /* $A40E */
void LoadSpritePalette(uint8_t hi, uint8_t lo);     /* $A419 */
void BlackPaletteBuf(void);                         /* $A43D */
void EnableNMI(void);                               /* $A44C */
void FadeIn(void);                                  /* $A456: 16 frames */
void WaitSprite0Hit(void);                          /* $A4E1 */
void RequestRenderingOn(void);                      /* $A4EE */
void FillVram1K(uint8_t a, uint8_t hi, uint8_t lo); /* $DA64 */
void FillVram64(uint8_t a, uint8_t hi, uint8_t lo); /* $DA79 */
void ReadJoypads(void);                             /* $A12B */

/* ---- boot and title (title.c) ---- */
void Reset(void);                                   /* $FF8C .. MainInit: up to the main loop */
void InitGame(void);                                /* $A022: a warm boot keeps the scores */
void MainLoopFrame(void);                           /* $A00B: wait, pads, one frame of the state */
void RunState(void);                                /* the GameStateTable ($A5F2) dispatch */
void State00_BootScreen(void);                      /* $A618 */
void State01_Title(void);                           /* $A67E */
void ResetPlayerScore(uint8_t x);                   /* $AFF3 */

/* ---- menus, world map, stage screens (menu.c) ---- */
void State02_TitleMenu(void);                       /* $A686 */
void State03_Password(void);                        /* $A82D */
void State05_WorldSelectInit(void);                 /* $A9E7 */
void State06_WorldSelect(void);                     /* $A9F2 */
void State07_StageIntroInit(void);                  /* $AA2B */
void State08_StageIntro(void);                      /* $AA36 */
void State09_CaveSetup(void);                       /* $AB2E */
void State0E_GameOver(void);                        /* $AEA3 */
void MapScreenSetup(void);                          /* $A78E */
/* $A802: row $21 of the text box table at hi:lo (50 bytes) into vram_buffer */
void TextBoxRow(uint8_t hi, uint8_t lo);
void SetScreenPos(uint8_t y, uint8_t x);            /* $E996 */

/* ---- world cleared, endings (ending.c) ---- */
void State04_NextStage(void);                       /* $A9E1 */
void State0F_Ending(void);                          /* $EABB */
void State10_EndingScene(void);                     /* $EC82 */
void State11_Credits(void);                         /* $ED09 */
void State12_NextLevel(void);                       /* $EFF5 */

/* ---- cave intro and cave play states (state.c) ---- */
void State0A_CaveIntro(void);                       /* $AB5E, one frame */
void State0B_CavePlay(void);                        /* $AB76, one frame */
void State0C_CaveCleared(void);                     /* $ACA1, one frame (the last one fades out) */
void State0D_LostLife(void);                        /* $ADF4, one frame (may fade out) */
void BuildMetatileQueue(void);                      /* $CA4D: redraw_list -> NMI queues */
void DrawMetaspriteScreen(uint8_t ptr_hi, uint8_t ptr_lo); /* $A5AA */

/* ---- player (player.c) ---- */
void PlayerInput(void);                             /* $C06B */
void PlayerHazards(void);                           /* $C373 */
void CollectDiamond(void);                          /* $CC8F */
void TickTime(void);                                /* $CFC6 */
void DecrementTime(void);                           /* $D039 */
bool PlayerOnBoxRow(void);                          /* $D04A: player at the screen row of the time-up box */

#endif

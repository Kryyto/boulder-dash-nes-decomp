/* bdview: play the ported game in an SDL2 window.
 *
 * The game runs as on the NES: Reset, then the main loop (wait for the
 * frame, read the pads, run the state's handler), all of it the ported
 * code: boot screen, title and attract demo, menus, password, world map,
 * caves, game over, the scenes after each world and the endings.
 *
 * The ported code talks to a model of the hardware (io.c). At each frame
 * (WaitForNMI), the viewer draws the picture the PPU model shows, runs the
 * NMI if the game enabled it (PPU updates, the sound driver), feeds the APU
 * writes to apu.c and its samples to SDL audio (44.1 kHz mono), and reads
 * the keyboard into both joypads (the game reads the current player's).
 *
 *   arrows       d-pad                        Z / X  A / B
 *   Enter        Start                        Right Shift / Tab  Select
 *   PgUp/PgDn    jump to the previous / next cave (a test aid)
 *   F5           reset                        Space  freeze, . steps one frame
 *   +/-          speed                        Esc    quit
 *
 * Usage: bdview ROM [cave] [--scale N] [--mute] [--shot FILE.bmp FRAMES] [--keys MOVES]
 *                [--wav FILE.wav]
 *   cave   starts a 1-player game in that cave (0-23) instead of booting.
 *   --shot runs FRAMES frames without a window and saves the 256x240 result.
 *   --wav  (with --shot) also saves the audio of those frames.
 *   --keys feeds pad 1 for --shot, one character per 8 frames:
 *          U D L R d-pad, u d l r A + direction (snap), A B, S Start, T Select,
 *          G A+B (give up), E open the exit right of the player (a test aid),
 *          . nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "apu.h"
#include "bd.h"

enum { W = 40, H = 22, FB_W = 256, FB_H = 240 };
enum { BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
       BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80 };

enum { RATE = 44100 };
static SDL_AudioDeviceID audio_dev;
static int16_t *wav_buf;       /* --wav: all samples */
static size_t wav_len, wav_cap;
static int collect_wav;
static uint32_t frame_counter; /* NTSC frames alternate 29780 / 29781 CPU cycles */

/* the window, and the viewer's own controls */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int headless, running = 1, frozen, step, speed = 1, jump_to = -1, reset;
static uint32_t frames_shown;  /* every WaitForNMI is one NES frame */
static const char *shot_keys;
static uint32_t fb[FB_W * FB_H];

/* 2C02 colours (the common FCEUX palette): emulator data, not from the game */
static const uint32_t nes_rgb[64] = {
    0x7C7C7C, 0x0000FC, 0x0000BC, 0x4428BC, 0x940084, 0xA80020, 0xA81000, 0x881400,
    0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0x000000, 0x000000,
    0xBCBCBC, 0x0078F8, 0x0058F8, 0x6844FC, 0xD800CC, 0xE40058, 0xF83800, 0xE45C10,
    0xAC7C00, 0x00B800, 0x00A800, 0x00A844, 0x008888, 0x000000, 0x000000, 0x000000,
    0xF8F8F8, 0x3CBCFC, 0x6888FC, 0x9878F8, 0xF878F8, 0xF85898, 0xF87858, 0xFCA044,
    0xF8B800, 0xB8F818, 0x58D854, 0x58F898, 0x00E8D8, 0x787878, 0x000000, 0x000000,
    0xFCFCFC, 0xA4E4FC, 0xB8B8F8, 0xD8B8F8, 0xF8B8F8, 0xF8A4C0, 0xF0D0B0, 0xFCE0A8,
    0xF8D878, 0xD8F878, 0xB8F8B8, 0xB8F8D8, 0x00FCFC, 0xF8D8F8, 0x000000, 0x000000,
};

/* ---- audio ---- */

static void sound_init(void)
{
    bd_apu_write = apu_write;
    apu_reset(RATE);
}

static void audio_frame(void)
{
    int16_t buf[1024];
    int n = apu_run((frame_counter++ & 1) ? 29781 : 29780, buf, 1024);
    if (audio_dev)
        SDL_QueueAudio(audio_dev, buf, (Uint32)(n * sizeof(int16_t)));
    if (collect_wav) {
        if (wav_len + (size_t)n > wav_cap) {
            wav_cap = (wav_len + (size_t)n) * 2;
            wav_buf = realloc(wav_buf, wav_cap * sizeof(int16_t));
        }
        memcpy(wav_buf + wav_len, buf, (size_t)n * sizeof(int16_t));
        wav_len += (size_t)n;
    }
}

static int write_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    uint32_t data = (uint32_t)(wav_len * 2), riff = 36 + data, rate = RATE, byte_rate = RATE * 2, fmt_len = 16;
    uint16_t pcm = 1, chans = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&chans, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(wav_buf, 2, wav_len, f);
    fclose(f);
    return 1;
}

/* ---- starting points ---- */

/* A 1-player game in `cave`, as the title's Start and the map's stage
 * choice would set it up, then the cave setup state (test aid). */
static void start_in_cave(int cave)
{
    ram.play_mode = 0x10;
    for (int x = 9; x >= 0; x--) {
        R(0x74 + x) = prg((uint16_t)(0xDB3D + x));
        R(0x7E + x) = prg((uint16_t)(0xDB3D + x));
    }
    ram.loop_count |= 0x80; /* the map has been seen */
    ram.world_stage = (uint8_t)((cave / 4) << 4 | cave % 4);
    Sound_Play(0x13); /* the tune the map starts, which plays on through the cave intro */
    BlackoutNow();
    SetGameState(0x09);
}

static void jump_to_cave(int cave) /* from wherever the game is */
{
    if (ram.game_state < 0x09 || ram.game_state > 0x0D) {
        start_in_cave(cave);
        return;
    }
    R(0x75 + PlayerOffset()) = (uint8_t)((cave / 4) << 4 | cave % 4);
    BlackoutNow();
    SetGameState(0x09);
}

static int current_cave(void) { return (ram.world_stage >> 4) * 4 + (ram.world_stage & 3); }

/* ---- input ---- */

static uint8_t key_buttons(char c)
{
    switch (c) {
    case 'U': return BTN_UP;
    case 'D': return BTN_DOWN;
    case 'L': return BTN_LEFT;
    case 'R': return BTN_RIGHT;
    case 'u': return BTN_A | BTN_UP;
    case 'd': return BTN_A | BTN_DOWN;
    case 'l': return BTN_A | BTN_LEFT;
    case 'r': return BTN_A | BTN_RIGHT;
    case 'A': return BTN_A;
    case 'B': return BTN_B;
    case 'S': return BTN_START;
    case 'T': return BTN_SELECT;
    case 'G': return BTN_A | BTN_B; /* held: the player gives up */
    default: return 0;
    }
}

/* Test aid, not the game: open the exit and put it right of the player. */
static void cheat_exit(void)
{
    if (ram.game_state != 0x0B)
        return;
    uint8_t row = (uint8_t)((ram.player_y[0] | ram.player_y[1] << 8) >> 4);
    uint8_t col = (uint8_t)((ram.player_x[0] | ram.player_x[1] << 8) >> 4);
    if (col + 1 < W - 1) {
        ram.exit_open = 1;
        ram.cave_grid[row * W + col + 1] = 0x40;
    }
}

static void poll_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            running = 0;
        } else if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: running = 0; break;
            case SDLK_PAGEDOWN: jump_to = (current_cave() + 1) % 24; break;
            case SDLK_PAGEUP: jump_to = (current_cave() + 23) % 24; break;
            case SDLK_F5: reset = 1; break;
            case SDLK_SPACE: frozen = !frozen; break;
            case SDLK_PERIOD: step = 1; break;
            case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS: if (speed < 16) speed *= 2; break;
            case SDLK_MINUS: case SDLK_KP_MINUS: if (speed > 1) speed /= 2; break;
            default: break;
            }
        }
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    hw.pad[0] = (uint8_t)((k[SDL_SCANCODE_Z] ? BTN_A : 0) | (k[SDL_SCANCODE_X] ? BTN_B : 0) |
                          (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_TAB] ? BTN_SELECT : 0) |
                          (k[SDL_SCANCODE_RETURN] ? BTN_START : 0) |
                          (k[SDL_SCANCODE_UP] ? BTN_UP : 0) | (k[SDL_SCANCODE_DOWN] ? BTN_DOWN : 0) |
                          (k[SDL_SCANCODE_LEFT] ? BTN_LEFT : 0) | (k[SDL_SCANCODE_RIGHT] ? BTN_RIGHT : 0));
    hw.pad[1] = hw.pad[0]; /* player 2's turns read pad 2: one keyboard for both */
}

/* ---- the picture ---- */

static void render(void)
{
    static uint8_t idx[FB_H][FB_W];
    ppu_render(idx);
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            fb[y * FB_W + x] = 0xFF000000u | nes_rgb[idx[y][x] & 0x3F];
}

static const char *state_name(uint8_t s)
{
    static const char *const names[] = {
        "boot", "title", "title menu", "password", "world cleared", "world map", "world map",
        "stage intro", "stage intro", "cave setup", "cave intro", "cave", "cave cleared",
        "life lost", "game over", "ending", "ending", "credits", "next level",
    };
    return s < sizeof names / sizeof names[0] ? names[s] : "?";
}

static void present(void)
{
    SDL_UpdateTexture(tex, NULL, fb, FB_W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    char title[160];
    int c = current_cave();
    if (ram.game_state >= 0x09 && ram.game_state <= 0x0D)
        snprintf(title, sizeof title, "Boulder Dash - %s, world %d-%d (cave %d) loop %u%s",
                 state_name(ram.game_state), c / 4 + 1, c % 4 + 1, c, ram.loop_count & 0x07,
                 frozen ? " | frozen" : "");
    else
        snprintf(title, sizeof title, "Boulder Dash - %s%s", state_name(ram.game_state),
                 frozen ? " | frozen" : "");
    SDL_SetWindowTitle(win, title);
    SDL_RenderPresent(ren);
}

/* Keep to the NES's frame rate (60.0988 Hz), whatever the display's: with
 * sound, by the audio clock (wait while more than 3 frames of samples are
 * queued); muted, by the performance counter. */
static void pace(void)
{
    if (audio_dev) {
        const Uint32 ahead = RATE / 60 * 3 * sizeof(int16_t);
        while (SDL_GetQueuedAudioSize(audio_dev) > ahead)
            SDL_Delay(1);
        return;
    }
    static Uint64 next;
    Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
    Uint64 frame = (Uint64)(freq * 655171.0 / 39375000.0); /* 1 / 60.0988 s */
    if (!next || now > next + 8 * frame)
        next = now;
    next += frame;
    while (SDL_GetPerformanceCounter() < next)
        SDL_Delay(1);
}

/* ---- the frame hook: what happens at every WaitForNMI ---- */

static void viewer_frame(void)
{
    render(); /* the frame the PPU showed since the last NMI */
    if (hw.ctrl & 0x80)
        NmiHandler();
    ppu_frame_start();
    audio_frame();
    frames_shown++;
    if (headless) {
        size_t i = frames_shown / 8, n = shot_keys ? strlen(shot_keys) : 0;
        hw.pad[0] = hw.pad[1] = i < n ? key_buttons(shot_keys[i]) : 0;
        if (i < n && shot_keys[i] == 'E' && frames_shown % 8 == 0)
            cheat_exit();
        return;
    }
    if (frames_shown % (uint32_t)speed == 0) {
        present();
        pace();
    } else if (audio_dev) {
        SDL_ClearQueuedAudio(audio_dev); /* fast forward: don't let the sound fall behind */
    }
    poll_events();
    while (frozen && running && !step && !reset && jump_to < 0) { /* the viewer's own pause */
        SDL_Delay(16);
        poll_events();
        present();
    }
    step = 0;
}

static void boot(int cave)
{
    memset(ram.raw, 0, sizeof ram.raw); /* power on */
    hw_reset();
    apu_reset(RATE);
    Reset();
    if (cave >= 0)
        start_in_cave(cave);
}

static void run_frame(void)
{
    if (reset) {
        reset = 0;
        boot(-1);
    }
    if (jump_to >= 0) {
        jump_to_cave(jump_to);
        jump_to = -1;
    }
    MainLoopFrame();
}

static int shot_mode(const char *path, int frames, const char *wav, int cave)
{
    collect_wav = wav != NULL;
    headless = 1;
    sound_init();
    bd_frame_hook = viewer_frame;
    boot(cave);
    while (frames_shown < (uint32_t)frames)
        run_frame();
    render();
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(fb, FB_W, FB_H, 32, FB_W * 4, SDL_PIXELFORMAT_ARGB8888);
    int ok = s && SDL_SaveBMP(s, path) == 0;
    SDL_FreeSurface(s);
    if (wav)
        ok &= write_wav(wav);
    printf("state $%02X/%u (%s), world_stage $%02X, loop %u, lives %u, score %u, player phase $%02X at "
           "pixel (%u, %u), camera (%u, %u), diamonds left %u%u%u, time %u%u%u, exit %s\n",
           ram.game_state, ram.game_substate, state_name(ram.game_state), ram.world_stage, ram.loop_count,
           ram.lives, (unsigned)(ram.score[0] | ram.score[1] << 8 | ram.score[2] << 16),
           ram.player_phase, ram.player_x[0] | ram.player_x[1] << 8, ram.player_y[0] | ram.player_y[1] << 8,
           ram.camera_x[0] | ram.camera_x[1] << 8, ram.camera_y[0] | ram.camera_y[1] << 8,
           ram.diamonds_needed[2], ram.diamonds_needed[1], ram.diamonds_needed[0],
           ram.time_left[2], ram.time_left[1], ram.time_left[0], ram.exit_open ? "open" : "closed");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM [cave 0-23] [--scale N] [--mute] [--shot FILE.bmp FRAMES] "
                        "[--keys MOVES] [--wav FILE.wav]\n", argv[0]);
        return 2;
    }
    char err[256];
    if (!bd_rom_load(argv[1], err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    const char *shot = NULL, *keys = NULL, *wav = NULL;
    int shot_frames = 0, scale = 3, mute = 0, cave = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
            shot = argv[++i];
            shot_frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            keys = argv[++i];
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = argv[++i];
        } else if (!strcmp(argv[i], "--mute")) {
            mute = 1;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1 || scale > 6)
                scale = 3;
        } else {
            cave = atoi(argv[i]) % 24;
        }
    }
    shot_keys = keys;
    if (shot)
        return shot_mode(shot, shot_frames, wav, cave);

    if (SDL_Init(SDL_INIT_VIDEO | (mute ? 0 : SDL_INIT_AUDIO)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!mute) {
        SDL_AudioSpec want = {0}, have;
        want.freq = RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio_dev)
            fprintf(stderr, "no audio: %s\n", SDL_GetError());
        else
            SDL_PauseAudioDevice(audio_dev, 0);
    }
    sound_init();
    win = SDL_CreateWindow("Boulder Dash", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           FB_W * scale, FB_H * scale, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    SDL_RenderSetLogicalSize(ren, FB_W, FB_H);
    bd_frame_hook = viewer_frame;
    boot(cave);
    while (running)
        run_frame();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

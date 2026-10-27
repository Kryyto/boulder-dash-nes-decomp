/* Sound driver: Sound_Init ($8011) and Sound_Update ($81B4), faithful ports.
 *
 * The driver's output is APU register writes, which go through io_write
 * (the RAM-diff test records them, bdview feeds them to apu.c).
 *
 * The 6502 code keeps the channel index in X and does not always restore it:
 * a few paths return with X = channel * 4 or channel + 4, and the caller
 * then indexes its arrays with that. So routines here take and return X
 * where the original lets it leak. SFX code also uses X = channel + 4 to
 * reach the second half of the per-channel arrays ($0328 = $0324 + 4, ...).
 *
 * Stream commands ($90+) can abort through the stack: `pla pla` ends the
 * parse for this tick, and `pla x6` skips the rest of the effect update.
 * They return a Flow code for that.
 */
#include "bd.h"

#define CH          ram.snd_channel
#define A_(base, x) R((uint16_t)((base) + (x)))  /* base,x indexing */
#define FLAGS(x)    A_(0x362, x)   /* chan_flags */
#define RELEASE(x)  A_(0x366, x)   /* release_mode */
#define MASK(x)     prg((uint16_t)(0x81B0 + (x)))

enum flow { CONT, STOP, ABORT_SAVE, ABORT_SILENCE };

static uint8_t stream(uint8_t y) { return rd((uint16_t)(zp_ptr(0x07) + y)); }

/* $846B: $07/08 += y */
static void AdvanceStream(uint8_t y) { set_zp_ptr(0x07, (uint16_t)(zp_ptr(0x07) + y)); }

/* $8567: write the channel's pitch (and sweep). Returns X. For the tone
 * channels it also leaves Y as an index into the period table (last_y),
 * which leaks into the stream parser in one case (see EndMusicChannel). */
static uint8_t last_y; /* Y left by SetPitch, when it sets one */

static uint8_t SetPitch(uint8_t x)
{
    if (x == 3) { /* noise */
        io_write(0x400E, A_(0x356, x) & 0x0F);
        io_write(0x400F, 0x08);
        return x;
    }
    R(0x0F) = A_(0x378, x);
    R(0x09) = A_(0x342, x);
    R(0x0A) = A_(0x346, x);
    uint8_t y = (uint8_t)((uint8_t)(A_(0x352, x) + A_(0x356, x)) << 1);
    R(0x30E) = A_(0x35A, x); /* detune */
    uint8_t x4 = (uint8_t)(x << 2);
    if (x4 != 0x02)
        io_write((uint16_t)(0x4001 + x4), R(0x0F));
    unsigned lo = rd((uint16_t)(zp_ptr(0x09) + y)) + R(0x30E);
    io_write((uint16_t)(0x4002 + x4), (uint8_t)lo);
    uint8_t carry = lo > 0xFF;
    y++;
    last_y = y;
    uint8_t hi = rd((uint16_t)(zp_ptr(0x09) + y));
    if (R(0x30E) & 0x80) {
        io_write((uint16_t)(0x4003 + x4), (uint8_t)((uint8_t)(hi - 1 + carry) | 0x08));
        return x4; /* the original does lda snd_channel here, so X stays x*4 */
    }
    io_write((uint16_t)(0x4003 + x4), (uint8_t)((uint8_t)(hi + carry) | 0x08));
    return CH;
}

/* $8502: write the channel's volume/duty register. Returns X. */
static uint8_t WriteChannelVolume(uint8_t x)
{
    uint8_t a, kind = x & 0x03;
    if (kind == 2) { /* triangle */
        if (A_(0x32C, x))
            return x;
        a = 0x80;
        goto write;
    }
    if (kind == 3) { /* noise */
        a = 0x30;
    } else { /* pulse: duty from the sequence or the duty byte */
        if (FLAGS(x) & 0x80) {
            a = prg((uint16_t)(0x8563 + A_(0x334, x)));
        } else {
            uint8_t v = A_(0x334, x), c = v & 1; /* lsr, ror, ror */
            v >>= 1;
            uint8_t c2 = v & 1;
            v = (uint8_t)((v >> 1) | (c << 7));
            v = (uint8_t)((v >> 1) | (c2 << 7));
            a = v;
        }
        a = (a & 0xC0) | 0x30;
        if (x >= 4) {
            R(0x0F) = a;
            if (A_(0x324, x) & 0x80)
                a = 0xEF & R(0x0F);
            else
                a = R(0x0F);
        }
    }
    a |= A_(0x32C, x);
write:
    R(0x0F) = a;
    io_write((uint16_t)(0x4000 + (uint8_t)((x & 0x03) << 2)), a);
    return CH;
}

/* $887A: mute a channel. Returns X. */
static uint8_t SilenceChannel(uint8_t x)
{
    if (x == 2) {
        io_write(0x4008, 0x80);
    } else {
        uint8_t x4 = (uint8_t)(x << 2);
        io_write((uint16_t)(0x4000 + x4), 0x30);
        io_write((uint16_t)(0x4001 + x4), 0x08);
    }
    SetPitch(CH);
    return CH;
}

/* $8452: note length / 4 times the release factor */
static uint8_t ReleaseLength(uint8_t x)
{
    R(0x0F) = (uint8_t)(A_(0x34E, x) >> 2);
    uint8_t n = RELEASE(x) & 0x0F, a = 0;
    do
        a = (uint8_t)(a + R(0x0F));
    while (--n != 0);
    return a;
}

/* $8631: stream command (a = byte - $90) for channel CH; *y indexes the stream */
static enum flow SoundCommand(uint8_t cmd, uint8_t *y)
{
    /* the original jumps through $0B/0C to the handler from SoundCommandTable */
    uint8_t t = (uint8_t)(cmd << 1);
    R(0x0B) = prg((uint16_t)(0x8643 + t));
    R(0x0C) = prg((uint16_t)(0x8644 + t));
    uint8_t x = CH;
    uint8_t v;
    switch (cmd) {
    case 0x00: FLAGS(x) |= 0x20; return STOP;                               /* rest / end of row */
    case 0x01: A_(0x352, x) = (uint8_t)(A_(0x352, x) + 12); break;           /* octave up */
    case 0x02: A_(0x352, x) = (uint8_t)(A_(0x352, x) - 12); break;           /* octave down */
    case 0x03: A_(0x352, x) = stream((*y)++); break;                         /* transpose */
    case 0x04: A_(0x35E, x) = stream((*y)++); break;                         /* volume */
    case 0x05: FLAGS(x) |= 0x30; break;                                      /* hold */
    case 0x06: A_(0x334, x) = stream((*y)++); break;                         /* duty */
    case 0x07:                                                              /* duty sequence on */
        FLAGS(x) |= 0x80;
        v = stream((*y)++);
        A_(0x374, x) = v;
        A_(0x376, x) = v;
        break;
    case 0x08: FLAGS(x) &= 0x7F; A_(0x334, x) = 2; break;                    /* duty sequence off */
    case 0x09: RELEASE(x) = (uint8_t)((RELEASE(x) | 0x40) & 0xCF); A_(0x36A, x) = stream((*y)++); break;
    case 0x0A: RELEASE(x) = (uint8_t)((RELEASE(x) | 0x60) & 0xEF); A_(0x36A, x) = stream((*y)++); break;
    case 0x0B: RELEASE(x) = (uint8_t)(((RELEASE(x) | 0x50) & 0xDF) | stream((*y)++)); break;
    case 0x0C: RELEASE(x) = (uint8_t)((RELEASE(x) | 0x70) | stream((*y)++)); break;
    case 0x0D: RELEASE(x) &= 0x8F; break;                                    /* release off */
    case 0x0E: A_(0x378, x) = stream((*y)++); break;                         /* sweep value */
    case 0x11: case 0x15: {                                                 /* jump */
        uint8_t lo = stream((*y)++);
        R(0x0F) = lo;
        R(0x08) = stream(*y);
        R(0x07) = R(0x0F);
        *y = 0;
        break;
    }
    case 0x12:                                                              /* end music channel */
        ram.music_channels &= (uint8_t)~MASK(x);
        if (!(ram.sfx_channels & MASK(x))) {
            last_y = *y;
            SilenceChannel(x);
            *y = last_y; /* SetPitch's Y leaks: the stream pointer advances by it */
        }
        return STOP;
    case 0x13: {                                                            /* loop start */
        R(0x0F) = stream((*y)++);
        AdvanceStream(*y);
        uint8_t sp = (uint8_t)(A_(0x320, x) + 3);
        A_(0x320, x) = sp;
        *ind_y(0x0D, (uint8_t)(sp - 1)) = R(0x0F);
        *ind_y(0x0D, (uint8_t)(sp - 2)) = R(0x07);
        *ind_y(0x0D, (uint8_t)(sp - 3)) = R(0x08);
        *y = 0;
        break;
    }
    case 0x14: {                                                            /* loop end */
        R(0x0F) = *y;
        uint8_t sp = (uint8_t)(A_(0x320, x) - 1);
        uint8_t count = (uint8_t)(*ind_y(0x0D, sp) - 1);
        if (count == 0) {
            A_(0x320, x) = (uint8_t)(A_(0x320, x) - 3);
            *y = R(0x0F);
            break;
        }
        *ind_y(0x0D, sp) = count;
        R(0x07) = *ind_y(0x0D, (uint8_t)(sp - 1));
        R(0x08) = *ind_y(0x0D, (uint8_t)(sp - 2));
        *y = 0;
        break;
    }
    case 0x16: A_(0x324, x) = stream((*y)++); return STOP;                   /* sfx: wait */
    case 0x17: A_(0x330, x) = stream((*y)++); break;                         /* sfx: volume */
    case 0x18:                                                              /* sfx: period */
        A_(0x318, x) = stream((*y)++);
        A_(0x31C, x) = stream((*y)++);
        A_(0x328, x) |= 0x40;
        break;
    case 0x19: A_(0x338, x) = stream((*y)++); break;                         /* sfx: duty */
    case 0x1A: io_write((uint16_t)(0x4001 + (uint8_t)(x << 2)), stream((*y)++)); break; /* sweep */
    case 0x1B: A_(0x330, x) = (uint8_t)(A_(0x330, x) + stream((*y)++)); break;
    case 0x1C: {                                                            /* sfx: period += n */
        unsigned s = stream((*y)++) + A_(0x318, x);
        A_(0x318, x) = (uint8_t)s;
        if (s > 0xFF) {
            A_(0x31C, x) = (uint8_t)(A_(0x31C, x) + 1);
            A_(0x328, x) |= 0x40;
        }
        break;
    }
    case 0x1D: A_(0x330, x)++; break;
    case 0x1F: A_(0x338, x)++; break;
    case 0x20: A_(0x330, x) = (uint8_t)(A_(0x330, x) - stream((*y)++)); break;
    case 0x21: {                                                            /* sfx: period -= n */
        uint8_t n = stream((*y)++);
        uint8_t old = A_(0x318, x);
        A_(0x318, x) = (uint8_t)(old - n);
        if (old < n) {
            A_(0x31C, x) = (uint8_t)(A_(0x31C, x) - 1);
            A_(0x328, x) |= 0x40;
        }
        break;
    }
    case 0x22: A_(0x330, x)--; break;
    case 0x26:                                                              /* end sfx channel */
        ram.sfx_channels &= (uint8_t)~MASK(x);
        SilenceChannel(x);
        return ABORT_SILENCE;
    case 0x29: {                                                            /* sfx: raw registers + wait */
        uint8_t x4 = (uint8_t)(x << 2);
        io_write((uint16_t)(0x4000 + x4), stream((*y)++));
        v = stream((*y)++);
        if (x4 != 0x08)
            io_write((uint16_t)(0x4001 + x4), v);
        io_write((uint16_t)(0x4002 + x4), stream((*y)++));
        io_write((uint16_t)(0x4003 + x4), stream((*y)++));
        A_(0x324, CH) = stream((*y)++);
        return ABORT_SAVE;
    }
    case 0x2B: ram.tempo[0] = stream((*y)++); ram.tempo[1] = stream((*y)++); break;
    case 0x2C:                                                              /* repeat start */
        A_(0x38A, x) = stream((*y)++);
        AdvanceStream(*y);
        A_(0x382, x) = R(0x07);
        A_(0x386, x) = R(0x08);
        *y = 0;
        break;
    case 0x2D:                                                              /* repeat end */
        if (--A_(0x38A, x) != 0) {
            R(0x07) = A_(0x382, x);
            R(0x08) = A_(0x386, x);
            *y = 0;
        }
        break;
    case 0x2E:                                                              /* call */
        R(0x0F) = stream((*y)++);
        R(0x30F) = stream((*y)++);
        AdvanceStream(*y);
        A_(0x38E, x) = R(0x07);
        A_(0x392, x) = R(0x08);
        R(0x07) = R(0x0F);
        R(0x08) = R(0x30F);
        *y = 0;
        break;
    case 0x2F:                                                              /* return */
        R(0x07) = A_(0x38E, x);
        R(0x08) = A_(0x392, x);
        *y = 0;
        break;
    case 0x31: A_(0x35A, x) = stream((*y)++); break;                         /* detune */
    default: break; /* no-ops ($9F, $A0, $AE, $B3-$B5, $B7, $B8, $BA, $C0, $C2) */
    }
    return CONT;
}

/* $83F8: read music bytes until a note (or a stopping command) */
static uint8_t MusicParse(uint8_t x, uint8_t y)
{
    for (;;) {
        uint8_t a = stream(y);
        y++;
        if (!(a & 0x80)) {
            A_(0x34E, x) = a; /* note length */
            continue;
        }
        if (a >= 0x90) {
            if (SoundCommand((uint8_t)(a - 0x90), &y) != CONT)
                return y;
            x = CH;
            continue;
        }
        A_(0x356, x) = (uint8_t)(a - 0x80); /* note */
        A_(0x32C, x) = A_(0x35E, x);
        uint8_t rm = RELEASE(x);
        if (!(rm & 0x40))
            return y;
        if (!(rm & 0x20)) {
            A_(0x36E, x) = (rm & 0x10) ? ReleaseLength(x) : A_(0x36A, x);
            return y;
        }
        if (x == 2)
            return y;
        if (rm & 0x10)
            A_(0x36A, x) = ReleaseLength(x);
        A_(0x36E, x) = 0;
        return y;
    }
}

static void MusicReadChannel(uint8_t x) /* $83D5 */
{
    R(0x07) = A_(0x33A, x);
    R(0x08) = A_(0x33E, x);
    uint8_t y = MusicParse(x, 0);
    AdvanceStream(y);
    A_(0x33A, x) = R(0x07);
    A_(0x33E, x) = R(0x08);
}

/* $8478/$849F: read effect bytes until a duration. Returns the flow. */
static enum flow SfxReadChannel(uint8_t x)
{
    R(0x07) = A_(0x310, x);
    R(0x08) = A_(0x314, x);
    R(0x0D) = prg((uint16_t)(0x8009 + x)); /* per-channel loop stack at $0396.. */
    R(0x0E) = prg((uint16_t)(0x800D + x));
    uint8_t y = 0;
    for (;;) {
        uint8_t a = stream(y);
        y++;
        if (a < 0x90) {
            A_(0x324, x) = a;
            break;
        }
        enum flow f = SoundCommand((uint8_t)(a - 0x90), &y);
        if (f == ABORT_SILENCE)
            return f; /* the stream pointer is not saved */
        if (f == STOP)
            break;
        if (f == ABORT_SAVE) {
            x = CH;
            AdvanceStream(y);
            A_(0x310, x) = R(0x07);
            A_(0x314, x) = R(0x08);
            return f;
        }
    }
    AdvanceStream(y);
    A_(0x310, x) = R(0x07);
    A_(0x314, x) = R(0x08);
    return CONT;
}

/* $84C2: write the effect's period. Returns X. */
static uint8_t SfxWritePeriod(uint8_t x)
{
    if (x == 3) {
        io_write(0x400E, A_(0x318, x));
        io_write(0x400F, 0x08);
        return x;
    }
    R(0x0F) = A_(0x318, x);
    uint8_t hi = (A_(0x31C, x) | 0x08) & 0x0F;
    R(0x30F) = A_(0x328, x) & 0x40;
    uint8_t x4 = (uint8_t)(x << 2);
    io_write((uint16_t)(0x4002 + x4), R(0x0F));
    if (R(0x30F))
        io_write((uint16_t)(0x4003 + x4), hi);
    return CH;
}

static uint8_t SfxUpdateChannel(uint8_t x) /* $84B4. Returns X. */
{
    if (SfxReadChannel(x) != CONT)
        return CH;
    x = WriteChannelVolume((uint8_t)(x + 4));
    return SfxWritePeriod(x);
}

/* $85CD: per-frame volume release, duty sequence, pitch refresh. Returns X. */
static uint8_t ChannelTick(uint8_t x)
{
    if (FLAGS(x) & 0x20)
        return x;
    if ((RELEASE(x) & 0x40) && A_(0x32C, x)) {
        if (!(RELEASE(x) & 0x20)) {
            if (--A_(0x36E, x) == 0)
                A_(0x32C, x) = 0;
        } else {
            uint8_t a = (uint8_t)(A_(0x36E, x) + A_(0x35E, x));
            while (a >= A_(0x36A, x)) {
                a = (uint8_t)(a - A_(0x36A, x));
                if (--A_(0x32C, x) == 0)
                    break;
            }
            A_(0x36E, x) = a;
        }
    }
    if (x < 2 && (FLAGS(x) & 0x80)) {
        if (--A_(0x376, x) == 0) {
            A_(0x376, x) = A_(0x374, x);
            A_(0x334, x)++;
        }
    }
    if (FLAGS(x) & 0x40)
        x = SetPitch(x);
    return WriteChannelVolume(x);
}

/* $8270: a note starts this tick. Returns X. */
static uint8_t NoteOn(uint8_t x)
{
    if (!(FLAGS(x) & 0x10) && (FLAGS(x) & 0x20))
        return SilenceChannel(x);
    if (R(0x309) == 0)
        x = SetPitch(x);
    if (x == 2) {
        io_write(0x4008, 0xFF);
        return x;
    }
    return WriteChannelVolume(x);
}

static void StartMusic(void) /* $82B8 */
{
    for (int8_t x = 3; x >= 0; x--)
        if ((ram.music_channels & MASK(x)) && !(ram.sfx_channels & MASK(x))) {
            CH = (uint8_t)x;
            SilenceChannel((uint8_t)x);
        }
    uint8_t t = (uint8_t)(ram.music_request << 1);
    ram.music_request |= 0x80;
    ram.music_playing = ram.music_request;
    R(0x07) = prg((uint16_t)(ROM_SoundTable + t));
    R(0x08) = prg((uint16_t)(ROM_SoundTable + t + 1));
    ram.music_channels = rd(zp_ptr(0x07)) & 0x7F;
    R(0x0F) = ram.music_channels;
    R(0x378) = 0x08;
    R(0x379) = 0x08;
    uint8_t y = 3;
    for (int8_t x = 3; x >= 0; x--) {
        uint8_t c = R(0x0F) & 1;
        R(0x0F) >>= 1;
        if (!c)
            continue;
        A_(0x33A, x) = stream(y++);
        A_(0x33E, x) = stream(y++);
        FLAGS(x) = 0;
        RELEASE(x) = 0;
        A_(0x352, x) = 0;
        A_(0x356, x) = 0;
        A_(0x35A, x) = 0;
        A_(0x35E, x) = 0;
        A_(0x34A, x) = 1;
        A_(0x34E, x) = 1;
    }
}

static void SilenceSfxChannels(void) /* $83C1 */
{
    for (int8_t x = 3; x >= 0; x--)
        if (MASK(x) & ram.sfx_channels) {
            CH = (uint8_t)x;
            SilenceChannel((uint8_t)x);
        }
}

static void StartSfx(void) /* $8337 */
{
    if (ram.sfx_request == 0x19)
        ram.music_paused = 0xFF;
    if (ram.sfx_request == 0x1A)
        ram.music_paused = 0x00;
    if (ram.sfx_channels)
        SilenceSfxChannels();
    uint8_t t = (uint8_t)(ram.sfx_request << 1);
    ram.sfx_request |= 0x80;
    ram.sfx_playing = ram.sfx_request;
    R(0x07) = prg((uint16_t)(ROM_SoundTable + t));
    R(0x08) = prg((uint16_t)(ROM_SoundTable + t + 1));
    ram.sfx_channels = rd(zp_ptr(0x07)) & 0x7F;
    R(0x0F) = ram.sfx_channels;
    io_write(0x4001, 0x08);
    io_write(0x4005, 0x08);
    uint8_t y = 3;
    for (int8_t x = 3; x >= 0; x--) {
        uint8_t c = R(0x0F) & 1;
        R(0x0F) >>= 1;
        if (!c)
            continue;
        A_(0x310, x) = stream(y++);
        A_(0x314, x) = stream(y++);
        A_(0x320, x) = 0;
        A_(0x318, x) = 0;
        A_(0x31C, x) = 0;
        A_(0x330, x) = 0;
        A_(0x324, x) = 1;
        if (x < 2)
            A_(0x338, x) = 2;
    }
}

static void SoundStartRequests(void) /* $829D */
{
    if (ram.music_request && ram.music_request != ram.music_playing)
        StartMusic();
    if (ram.sfx_request && ram.sfx_request != ram.sfx_playing)
        StartSfx();
}

static void SoundTempoTick(void) /* $824F: tempo_acc += tempo + 1; carry = a tick */
{
    unsigned s = R(0x05) + ram.tempo[0] + 1u;
    R(0x05) = (uint8_t)s;
    s = R(0x06) + ram.tempo[1] + (s >> 8);
    R(0x06) = (uint8_t)s;
    ram.tempo_tick = 0;
    if (s > 0xFF && !ram.music_paused)
        ram.tempo_tick = 0xFF;
}

void Sound_Update(void)
{
    SoundStartRequests();
    SoundTempoTick();
    uint8_t x = 3;
    do {
        CH = x;
        if (!ram.music_channels) {
            ram.music_request = 0;
            ram.music_playing = 0;
        } else if ((ram.music_channels & MASK(x)) && ram.tempo_tick) {
            if (--A_(0x34A, x) == 0) { /* next note */
                if (FLAGS(x) & 0x10)
                    R(0x309) = 0xFF;
                FLAGS(x) &= 0xCF;
                MusicReadChannel(x);
                A_(0x34A, x) = A_(0x34E, x);
            }
        }
        if (!ram.sfx_channels) {
            ram.sfx_request = 0;
            ram.sfx_playing = 0;
        } else if (ram.sfx_channels & MASK(x)) { /* an effect owns this channel */
            FLAGS(x) |= 0x20;
            if (--A_(0x324, x) == 0) {
                A_(0x328, x) &= 0xBF;
                x = SfxUpdateChannel(x);
            }
            goto next;
        }
        if ((ram.music_channels & MASK(x)) && ram.tempo_tick) {
            if (A_(0x34A, x) == A_(0x34E, x))
                x = NoteOn(x);
            else
                x = ChannelTick(x);
        }
    next:
        R(0x309) = 0;
        x--;
    } while (!(x & 0x80));
}

void Sound_Init(void)
{
    io_write(0x4015, 0x0F);
    io_write(0x4001, 0x08);
    io_write(0x4005, 0x08);
    for (int i = 0; i < 0x100; i++)
        R(0x300 + i) = 0;
    for (int8_t y = 3; y >= 0; y--) {
        A_(0x362, y) = 0x00;
        A_(0x342, y) = 0x5C; /* period table at $805C */
        A_(0x346, y) = 0x80;
        io_write((uint16_t)(0x4000 + (uint8_t)(y << 2)), 0x80);
    }
    R(0x05) = 0;
    R(0x06) = 0;
    ram.music_paused = 0;
    R(0x300) = 0;
    ram.tempo[0] = 0xFF;
    ram.tempo[1] = 0xFF;
}

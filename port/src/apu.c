/* NES APU (2A03) emulation: two pulse channels, triangle and noise (the
 * game doesn't use the DMC). Standard behaviour as documented on the
 * NESdev wiki; register writes come from the ported sound driver.
 */
#include <string.h>

#include "apu.h"

static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1},
};
static const uint8_t triangle_table[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
static const uint16_t noise_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

typedef struct {
    uint8_t start, loop, constant, volume, divider, decay;
} Envelope;

typedef struct {
    int enabled;
    uint8_t duty, step;
    uint16_t period, timer;
    uint8_t length, halt;
    Envelope env;
    uint8_t sweep_enabled, sweep_period, sweep_negate, sweep_shift, sweep_reload, sweep_divider;
    int ones_complement; /* pulse 1 negates with ones' complement */
} Pulse;

typedef struct {
    int enabled;
    uint16_t period, timer;
    uint8_t step, length, control, linear_reload, linear, reload_flag;
} Triangle;

typedef struct {
    int enabled;
    uint8_t mode, length, halt;
    uint16_t period, timer, lfsr;
    Envelope env;
} Noise;

static struct {
    Pulse p[2];
    Triangle t;
    Noise n;
    int five_step;
    uint32_t frame_cycle, cycle;
    double sample_acc, sample_count, cycles_per_sample, sample_phase;
    double hp_prev_in, hp_prev_out, lp_out;
} apu;

void apu_reset(int sample_rate)
{
    memset(&apu, 0, sizeof apu);
    apu.p[0].ones_complement = 1;
    apu.n.lfsr = 1;
    apu.cycles_per_sample = 1789773.0 / sample_rate;
}

static void env_clock(Envelope *e)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = e->volume;
    } else if (e->divider == 0) {
        e->divider = e->volume;
        if (e->decay)
            e->decay--;
        else if (e->loop)
            e->decay = 15;
    } else {
        e->divider--;
    }
}

static uint8_t env_out(const Envelope *e) { return e->constant ? e->volume : e->decay; }

static uint16_t sweep_target(const Pulse *p)
{
    uint16_t change = p->period >> p->sweep_shift;
    if (p->sweep_negate)
        return (uint16_t)(p->period - change - (p->ones_complement ? 1 : 0));
    return (uint16_t)(p->period + change);
}

static int pulse_muted(const Pulse *p)
{
    return p->period < 8 || (!p->sweep_negate && sweep_target(p) > 0x7FF);
}

static void sweep_clock(Pulse *p)
{
    if (p->sweep_divider == 0 && p->sweep_enabled && p->sweep_shift && !pulse_muted(p))
        p->period = sweep_target(p);
    if (p->sweep_divider == 0 || p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = 0;
    } else {
        p->sweep_divider--;
    }
}

static void quarter_frame(void)
{
    env_clock(&apu.p[0].env);
    env_clock(&apu.p[1].env);
    env_clock(&apu.n.env);
    if (apu.t.reload_flag)
        apu.t.linear = apu.t.linear_reload;
    else if (apu.t.linear)
        apu.t.linear--;
    if (!apu.t.control)
        apu.t.reload_flag = 0;
}

static void half_frame(void)
{
    for (int i = 0; i < 2; i++) {
        if (apu.p[i].length && !apu.p[i].halt)
            apu.p[i].length--;
        sweep_clock(&apu.p[i]);
    }
    if (apu.t.length && !apu.t.control)
        apu.t.length--;
    if (apu.n.length && !apu.n.halt)
        apu.n.length--;
}

void apu_write(uint16_t addr, uint8_t v)
{
    if (addr < 0x4008) {
        Pulse *p = &apu.p[(addr >> 2) & 1];
        switch (addr & 3) {
        case 0:
            p->duty = v >> 6;
            p->halt = p->env.loop = (v >> 5) & 1;
            p->env.constant = (v >> 4) & 1;
            p->env.volume = v & 0x0F;
            break;
        case 1:
            p->sweep_enabled = v >> 7;
            p->sweep_period = (v >> 4) & 7;
            p->sweep_negate = (v >> 3) & 1;
            p->sweep_shift = v & 7;
            p->sweep_reload = 1;
            break;
        case 2:
            p->period = (uint16_t)((p->period & 0x700) | v);
            break;
        case 3:
            p->period = (uint16_t)((p->period & 0xFF) | (v & 7) << 8);
            if (p->enabled)
                p->length = length_table[v >> 3];
            p->step = 0;
            p->env.start = 1;
            break;
        }
    } else if (addr == 0x4008) {
        apu.t.control = v >> 7;
        apu.t.linear_reload = v & 0x7F;
    } else if (addr == 0x400A) {
        apu.t.period = (uint16_t)((apu.t.period & 0x700) | v);
    } else if (addr == 0x400B) {
        apu.t.period = (uint16_t)((apu.t.period & 0xFF) | (v & 7) << 8);
        if (apu.t.enabled)
            apu.t.length = length_table[v >> 3];
        apu.t.reload_flag = 1;
    } else if (addr == 0x400C) {
        apu.n.halt = apu.n.env.loop = (v >> 5) & 1;
        apu.n.env.constant = (v >> 4) & 1;
        apu.n.env.volume = v & 0x0F;
    } else if (addr == 0x400E) {
        apu.n.mode = v >> 7;
        apu.n.period = noise_periods[v & 0x0F];
    } else if (addr == 0x400F) {
        if (apu.n.enabled)
            apu.n.length = length_table[v >> 3];
        apu.n.env.start = 1;
    } else if (addr == 0x4015) {
        apu.p[0].enabled = v & 1;
        apu.p[1].enabled = (v >> 1) & 1;
        apu.t.enabled = (v >> 2) & 1;
        apu.n.enabled = (v >> 3) & 1;
        if (!apu.p[0].enabled) apu.p[0].length = 0;
        if (!apu.p[1].enabled) apu.p[1].length = 0;
        if (!apu.t.enabled) apu.t.length = 0;
        if (!apu.n.enabled) apu.n.length = 0;
    } else if (addr == 0x4017) {
        apu.five_step = v >> 7;
        apu.frame_cycle = 0;
        if (apu.five_step) {
            quarter_frame();
            half_frame();
        }
    }
    /* $4009, $400D, $4010-$4013 (DMC) and anything else: ignored */
}

static double mix(void)
{
    int p[2];
    for (int i = 0; i < 2; i++) {
        const Pulse *q = &apu.p[i];
        p[i] = (q->length && !pulse_muted(q) && duty_table[q->duty][q->step]) ? env_out(&q->env) : 0;
    }
    int t = triangle_table[apu.t.step];
    int n = (apu.n.length && !(apu.n.lfsr & 1)) ? env_out(&apu.n.env) : 0;
    double pulse = (p[0] + p[1]) ? 95.88 / (8128.0 / (p[0] + p[1]) + 100.0) : 0.0;
    double tnd_in = t / 8227.0 + n / 12241.0;
    double tnd = tnd_in > 0 ? 159.79 / (1.0 / tnd_in + 100.0) : 0.0;
    return pulse + tnd;
}

static void cpu_cycle(void)
{
    /* frame sequencer, in CPU cycles */
    apu.frame_cycle++;
    if (!apu.five_step) {
        switch (apu.frame_cycle) {
        case 7457: quarter_frame(); break;
        case 14913: quarter_frame(); half_frame(); break;
        case 22371: quarter_frame(); break;
        case 29829: quarter_frame(); half_frame(); apu.frame_cycle = 0; break;
        }
    } else {
        switch (apu.frame_cycle) {
        case 7457: quarter_frame(); break;
        case 14913: quarter_frame(); half_frame(); break;
        case 22371: quarter_frame(); break;
        case 37281: quarter_frame(); half_frame(); apu.frame_cycle = 0; break;
        }
    }
    /* triangle: every CPU cycle; silent while the counters are 0 */
    if (apu.t.timer == 0) {
        apu.t.timer = apu.t.period;
        if (apu.t.length && apu.t.linear && apu.t.period >= 2) /* skip ultrasonic periods */
            apu.t.step = (apu.t.step + 1) & 31;
    } else {
        apu.t.timer--;
    }
    /* pulse and noise: every other CPU cycle */
    if (apu.cycle++ & 1) {
        for (int i = 0; i < 2; i++) {
            Pulse *p = &apu.p[i];
            if (p->timer == 0) {
                p->timer = p->period;
                p->step = (p->step + 1) & 7;
            } else {
                p->timer--;
            }
        }
        if (apu.n.timer == 0) {
            apu.n.timer = apu.n.period;
            uint16_t bit = (apu.n.lfsr ^ (apu.n.lfsr >> (apu.n.mode ? 6 : 1))) & 1;
            apu.n.lfsr = (uint16_t)((apu.n.lfsr >> 1) | (bit << 14));
        } else {
            apu.n.timer--;
        }
    }
}

int apu_run(uint32_t cpu_cycles, int16_t *out, int max_samples)
{
    int n = 0;
    for (uint32_t c = 0; c < cpu_cycles; c++) {
        cpu_cycle();
        apu.sample_acc += mix();
        apu.sample_count += 1;
        apu.sample_phase += 1;
        if (apu.sample_phase >= apu.cycles_per_sample && n < max_samples) {
            apu.sample_phase -= apu.cycles_per_sample;
            double s = apu.sample_acc / apu.sample_count; /* box filter over the period */
            apu.sample_acc = apu.sample_count = 0;
            /* the console's output filters, roughly: 90 Hz high-pass, 14 kHz low-pass */
            double hp = 0.996 * (apu.hp_prev_out + s - apu.hp_prev_in);
            apu.hp_prev_in = s;
            apu.hp_prev_out = hp;
            apu.lp_out += 0.815 * (hp - apu.lp_out);
            double v = apu.lp_out * 30000.0;
            out[n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    return n;
}

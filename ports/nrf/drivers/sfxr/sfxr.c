/*
 * sfxr.c - see sfxr.h
 *
 * Strictly single-precision. The only transcendental used is sine, served
 * from a 1024-entry lookup table (built once, lazily) so the hot loop never
 * calls libm. pow() at init is replaced by explicit float multiplies.
 */
#include "sfxr.h"

/* ---- tiny math helpers (float, no double, no libm hot path) ---- */

#define SFXR_PI 3.14159265358979323846f

static float sfxr_sin_lut[1024];
static int   sfxr_lut_ready = 0;

/* Build a sine table once. Uses libm sinf at startup only (not in the
 * hot loop). On a target where even init should avoid libm, this table
 * can be generated offline instead. */
#include <math.h>
static void sfxr_build_lut(void) {
    int i;
    for (i = 0; i < 1024; i++)
        sfxr_sin_lut[i] = sinf((float)i * (2.0f * SFXR_PI / 1024.0f));
    sfxr_lut_ready = 1;
}

/* sine of x radians via LUT with linear interpolation. */
static float sfxr_sinf(float x) {
    float t = x * (1024.0f / (2.0f * SFXR_PI));
    int   i;
    float f;
    /* wrap into [0,1024) */
    t = t - 1024.0f * floorf(t * (1.0f / 1024.0f));
    i = (int)t;
    f = t - (float)i;
    return sfxr_sin_lut[i & 1023] +
           (sfxr_sin_lut[(i + 1) & 1023] - sfxr_sin_lut[i & 1023]) * f;
}

/* x*x and x*x*x, replacing pow(x,2)/pow(x,3). */
static inline float sfxr_sq(float x)  { return x * x; }
static inline float sfxr_cube(float x) { return x * x * x; }

/* deterministic xorshift RNG -> float in [0,range) */
static float sfxr_frnd(sfxr_state *s, float range) {
    unsigned int x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return (float)(x & 0xFFFFFF) * (1.0f / 16777216.0f) * range;
}

/* Internal reset. restart=1 keeps the filter/phaser/noise state (used by
 * the repeat feature); restart=0 is a full initialization. */
static void sfxr_reset_impl(sfxr_state *s, int restart) {
    const sfxr_params *p = &s->p;

    if (!restart)
        s->phase = 0;

    s->fperiod    = 100.0f / (p->p_base_freq * p->p_base_freq + 0.001f);
    s->period     = (int)s->fperiod;
    s->fmaxperiod = 100.0f / (p->p_freq_limit * p->p_freq_limit + 0.001f);
    s->fslide     = 1.0f - sfxr_cube(p->p_freq_ramp) * 0.01f;
    s->fdslide    = -sfxr_cube(p->p_freq_dramp) * 0.000001f;

    s->square_duty  = 0.5f - p->p_duty * 0.5f;
    s->square_slide = -p->p_duty_ramp * 0.00005f;

    if (p->p_arp_mod >= 0.0f)
        s->arp_mod = 1.0f - sfxr_sq(p->p_arp_mod) * 0.9f;
    else
        s->arp_mod = 1.0f + sfxr_sq(p->p_arp_mod) * 10.0f;
    s->arp_time  = 0;
    s->arp_limit = (int)(sfxr_sq(1.0f - p->p_arp_speed) * 20000.0f + 32.0f);
    if (p->p_arp_speed == 1.0f)
        s->arp_limit = 0;

    if (!restart) {
        /* low-pass filter */
        s->fltp  = 0.0f;
        s->fltdp = 0.0f;
        s->fltw  = sfxr_cube(p->p_lpf_freq) * 0.1f;
        s->fltw_d = 1.0f + p->p_lpf_ramp * 0.0001f;
        s->fltdmp = 5.0f / (1.0f + sfxr_sq(p->p_lpf_resonance) * 20.0f) *
                    (0.01f + s->fltw);
        if (s->fltdmp > 0.8f) s->fltdmp = 0.8f;
        s->fltphp = 0.0f;
        s->flthp  = sfxr_sq(p->p_hpf_freq) * 0.1f;
        s->flthp_d = 1.0f + p->p_hpf_ramp * 0.0003f;

        /* vibrato */
        s->vib_phase = 0.0f;
        s->vib_speed = sfxr_sq(p->p_vib_speed) * 0.01f;
        s->vib_amp   = p->p_vib_strength * 0.5f;

        /* envelope */
        s->env_vol   = 0.0f;
        s->env_stage = 0;
        s->env_time  = 0;
        s->env_length[0] = (int)(p->p_env_attack  * p->p_env_attack  * 100000.0f);
        s->env_length[1] = (int)(p->p_env_sustain * p->p_env_sustain * 100000.0f);
        s->env_length[2] = (int)(p->p_env_decay   * p->p_env_decay   * 100000.0f);

        /* phaser */
        s->fphase = sfxr_sq(p->p_pha_offset) * 1020.0f;
        if (p->p_pha_offset < 0.0f) s->fphase = -s->fphase;
        s->fdphase = sfxr_sq(p->p_pha_ramp) * 1.0f;
        if (p->p_pha_ramp < 0.0f) s->fdphase = -s->fdphase;
        s->iphase = (int)(s->fphase < 0 ? -s->fphase : s->fphase);
        s->ipp = 0;
        {
            int i;
            for (i = 0; i < 1024; i++) s->phaser_buffer[i] = 0.0f;
            for (i = 0; i < 32; i++)   s->noise_buffer[i] = sfxr_frnd(s, 2.0f) - 1.0f;
        }

        /* repeat */
        s->rep_time  = 0;
        s->rep_limit = (int)(sfxr_sq(1.0f - p->p_repeat_speed) * 20000.0f + 32.0f);
        if (p->p_repeat_speed == 0.0f)
            s->rep_limit = 0;
    }
}

void sfxr_reset(sfxr_state *s, const sfxr_params *p) {
    if (!sfxr_lut_ready) sfxr_build_lut();
    s->p = *p;
    s->rng = 0x1234567u;           /* deterministic seed */
    s->playing = 1;
    /* jsfxr master gain (expf called once here, never in the hot loop). */
    s->gain = expf(p->sound_vol) - 1.0f;
    sfxr_reset_impl(s, 0);
}

/* Advance the engine by one 44100-Hz tick and return the 8x-oversampled,
 * envelope-applied sample (before master/sound volume). Returns 0.0f and
 * leaves s->playing at 0 once the sound has ended. */
static float sfxr_engine_tick(sfxr_state *s) {
    const sfxr_params *p = &s->p;
    float ssample;
    float rfperiod;
    int   si;

    /* repeat */
    s->rep_time++;
    if (s->rep_limit != 0 && s->rep_time >= s->rep_limit) {
        s->rep_time = 0;
        sfxr_reset_impl(s, 1);
    }

    /* arpeggio / frequency slide */
    s->arp_time++;
    if (s->arp_limit != 0 && s->arp_time >= s->arp_limit) {
        s->arp_limit = 0;
        s->fperiod *= s->arp_mod;
    }
    s->fslide += s->fdslide;
    s->fperiod *= s->fslide;
    if (s->fperiod > s->fmaxperiod) {
        s->fperiod = s->fmaxperiod;
        if (p->p_freq_limit > 0.0f) s->playing = 0;
    }

    /* vibrato */
    rfperiod = s->fperiod;
    if (s->vib_amp > 0.0f) {
        s->vib_phase += s->vib_speed;
        rfperiod = s->fperiod * (1.0f + sfxr_sinf(s->vib_phase) * s->vib_amp);
    }
    s->period = (int)rfperiod;
    if (s->period < 8) s->period = 8;

    /* duty */
    s->square_duty += s->square_slide;
    if (s->square_duty < 0.0f) s->square_duty = 0.0f;
    if (s->square_duty > 0.5f) s->square_duty = 0.5f;

    /* volume envelope */
    s->env_time++;
    if (s->env_time > s->env_length[s->env_stage]) {
        s->env_time = 0;
        s->env_stage++;
        if (s->env_stage == 3) { s->playing = 0; return 0.0f; }
    }
    if (s->env_stage == 0)
        s->env_vol = s->env_length[0] ? (float)s->env_time / s->env_length[0] : 0.0f;
    else if (s->env_stage == 1)
        s->env_vol = 1.0f + (s->env_length[1]
                    ? (1.0f - (float)s->env_time / s->env_length[1])
                    : 0.0f) * 2.0f * p->p_env_punch;
    else if (s->env_stage == 2)
        s->env_vol = s->env_length[2]
                    ? 1.0f - (float)s->env_time / s->env_length[2] : 0.0f;

    /* phaser step */
    s->fphase += s->fdphase;
    s->iphase = (int)(s->fphase < 0 ? -s->fphase : s->fphase);
    if (s->iphase > 1023) s->iphase = 1023;

    /* high-pass filter sweep */
    if (s->flthp_d != 0.0f) {
        s->flthp *= s->flthp_d;
        if (s->flthp < 0.00001f) s->flthp = 0.00001f;
        if (s->flthp > 0.1f)     s->flthp = 0.1f;
    }

    /* 8x supersampling */
    ssample = 0.0f;
    for (si = 0; si < 8; si++) {
        float sample = 0.0f;
        float fp;
        float pp;

        s->phase++;
        if (s->phase >= s->period) {
            s->phase %= s->period;
            if (p->wave_type == 3) {
                int i;
                for (i = 0; i < 32; i++)
                    s->noise_buffer[i] = sfxr_frnd(s, 2.0f) - 1.0f;
            }
        }

        fp = (float)s->phase / (float)s->period;
        switch (p->wave_type) {
        case 0: /* square */
            sample = (fp < s->square_duty) ? 0.5f : -0.5f;
            break;
        case 1: /* sawtooth (jsfxr duty-aware: a triangle at duty=0.5) */
            if (fp < s->square_duty)
                sample = -1.0f + 2.0f * fp / s->square_duty;
            else
                sample = 1.0f - 2.0f * (fp - s->square_duty) / (1.0f - s->square_duty);
            break;
        case 2: /* sine */
            sample = sfxr_sinf(fp * 2.0f * SFXR_PI);
            break;
        case 3: /* noise */
            sample = s->noise_buffer[(s->phase * 32 / s->period) & 31];
            break;
        }

        /* low-pass filter */
        pp = s->fltp;
        s->fltw *= s->fltw_d;
        if (s->fltw < 0.0f) s->fltw = 0.0f;
        if (s->fltw > 0.1f) s->fltw = 0.1f;
        if (p->p_lpf_freq != 1.0f) {
            s->fltdp += (sample - s->fltp) * s->fltw;
            s->fltdp -= s->fltdp * s->fltdmp;
        } else {
            s->fltp = sample;
            s->fltdp = 0.0f;
        }
        s->fltp += s->fltdp;

        /* high-pass filter */
        s->fltphp += s->fltp - pp;
        s->fltphp -= s->fltphp * s->flthp;
        sample = s->fltphp;

        /* phaser */
        s->phaser_buffer[s->ipp & 1023] = sample;
        sample += s->phaser_buffer[(s->ipp - s->iphase + 1024) & 1023];
        s->ipp = (s->ipp + 1) & 1023;

        ssample += sample * s->env_vol;
    }

    return ssample;
}

/* Synthesize one output sample in [-1,1]. Assumes s->playing is set. */
static float sfxr_next_sample(sfxr_state *s) {
    /* master_vol is 0.05 in the original sfxr. */
    const float master_vol = 0.05f;
    float acc = 0.0f;
    int   k;

    /* Run the 44100-Hz engine SFXR_SUMMANDS times and average, to
     * decimate down to the target output rate. */
    for (k = 0; k < SFXR_SUMMANDS; k++)
        acc += sfxr_engine_tick(s);
    acc /= (float)SFXR_SUMMANDS;

    acc = acc / 8.0f * master_vol;
    acc *= s->gain;

    if (acc >  1.0f) acc =  1.0f;
    if (acc < -1.0f) acc = -1.0f;
    return acc;
}

int sfxr_generate(sfxr_state *s, float *out, int count) {
    int produced = 0;
    int n;

    for (n = 0; n < count; n++) {
        if (!s->playing) { out[n] = 0.0f; continue; }
        out[n] = sfxr_next_sample(s);
        produced++;
    }

    return produced;
}

int sfxr_generate_s16_stereo(sfxr_state *s, short *out, int frames) {
    int produced = 0;
    int n;

    for (n = 0; n < frames; n++) {
        short v;
        if (!s->playing) {
            v = 0;
        } else {
            float f = sfxr_next_sample(s);
            int   iv;
            /* -1..1 -> -32767..32767 (symmetric, no -32768 clip artifact) */
            iv = (int)(f * 32767.0f + (f >= 0.0f ? 0.5f : -0.5f));
            if (iv >  32767) iv =  32767;
            if (iv < -32767) iv = -32767;
            v = (short)iv;
            produced++;
        }
        out[n * 2 + 0] = v;
        out[n * 2 + 1] = v;
    }

    return produced;
}

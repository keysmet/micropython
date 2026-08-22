/*
 * ksynth.c - see ksynth.h for the design overview.
 *
 * Fixed-point conventions:
 *   - Oscillator phase: uint32, one full cycle = 2^32.
 *   - Pitch: Q8.8 MIDI note (int32), i.e. note*256 + fraction.
 *   - Envelope / gain levels: Q15. The per-sample ramped gain is kept in
 *     Q23 (Q15 << 8) so ramp steps keep precision over a control period.
 *   - libm (pow/exp) is used in ksyn_init() only, never per-sample or
 *     per-note (precedent: sfxr.c builds its LUT the same way).
 *
 * Concurrency: ksyn_render() may run in an IRQ while the other calls run
 * in the main loop. The rules used here:
 *   - Voice arming: `active` is cleared before a voice is (re)written and
 *     set last, so the IRQ never mixes a half-initialized voice.
 *   - Note queues are SPSC rings: the producer only writes `tail`, the
 *     consumer (tick handler) only writes `head`.
 *   - Release/stop from the main loop are request flags consumed at the
 *     next control tick instead of direct state edits.
 */
#include "ksynth.h"
#include <string.h>
#include <math.h> /* init only */

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

#define CTRL       KSYN_CTRL_SAMPLES
#define CTRL_MASK  (KSYN_CTRL_SAMPLES - 1)
#define ENV_ONE    32767            /* Q15 full level                   */
#define PITCH_MAX  (131 << 8)       /* Q8.8 clamp for the pitch LUT     */

static int      g_sr = 22050;
static uint32_t g_note_inc[133];    /* phase increment per MIDI note    */
static uint32_t g_inc_max;          /* Nyquist cap                      */
static uint16_t g_lpf_k[33];        /* Q15 one-pole coefficient LUT     */
static uint32_t g_tick_sec_q16;     /* control period in seconds, Q16   */
static uint32_t g_vib_unit;         /* LFO phase inc per ctrl tick, 1Hz */
static int32_t  g_master_q15 = 128 << 7;
static int      g_blep = 1;         /* polyBLEP band-limiting on/off    */
static int32_t  g_tone_k;           /* master tone LPF coeff, 0=bypass  */
static int32_t  g_tone_y;           /* master tone LPF state            */
static uint32_t g_seq;              /* voice allocation age counter     */

/* Tempo clock (exact rational samples-per-tick via remainder carry). */
static int      g_tempo_on;
static uint32_t g_tick_base;        /* floor(samples per tick)          */
static uint32_t g_tick_rem;         /* remainder numerator              */
static uint32_t g_tick_den;        /* denominator = bpm * ticks_per_beat */
static uint32_t g_tick_acc;         /* running remainder accumulator    */
static int32_t  g_to_tick;          /* samples until the next tick      */
static volatile uint32_t g_ticks_done;
static uint32_t g_ticks_taken;
static void   (*g_tick_cb)(void *);
static void    *g_tick_user;

static int g_ctrl_pos;              /* sample position in ctrl period   */

/* ------------------------------------------------------------------ */
/* Voice                                                               */
/* ------------------------------------------------------------------ */

enum { ST_ATK, ST_DEC, ST_HOLD, ST_REL };

typedef struct {
    ksyn_sound snd;                 /* copied by value                  */
    int16_t note;                   /* KSYN_REST for a rest             */
    uint8_t vel;
    uint8_t flags;
    uint16_t dur;                   /* tempo ticks                      */
    uint8_t has_snd;
} qentry;

typedef struct {
    /* status */
    volatile uint8_t active;
    volatile uint8_t fresh;         /* needs a control update           */
    volatile uint8_t rel_req;       /* main loop asked for release      */
    uint8_t  stage;
    uint32_t seq;

    ksyn_sound snd;                 /* sound copied at note-on          */

    /* oscillator */
    uint32_t phase, inc;
    uint32_t rng;
    int32_t  noise_val;             /* held LFSR output, Q15            */
    int32_t  duty_q16;              /* duty 0..255 in Q16               */
    int32_t  dduty_q16;             /* per control tick                 */

    /* envelope (levels Q15, times in control ticks) */
    int32_t  env;
    int32_t  sus_q15;
    int32_t  atk_step, dec_step, rel_step;
    int32_t  hold_ctr;              /* -1 = infinite                    */
    int32_t  amp_q15;               /* volume * velocity                */

    /* per-sample gain ramp */
    int32_t  gain_q23, gstep_q23;

    /* pitch */
    int32_t  base_q8;               /* note << 8                        */
    int32_t  acc_q8;                /* slide accumulation               */
    int32_t  slide_q8;              /* semitones/sec, Q8.8 (curved)     */
    int32_t  dslide_q8;             /* curve per control tick           */
    uint32_t vib_phase, vib_inc;
    int32_t  vib_depth_q8;

    /* arpeggio */
    uint8_t  arp_idx;
    uint16_t arp_ctr, arp_ticks;

    /* filter */
    int32_t  lpf_q16;               /* cutoff 0..255 in Q16             */
    int32_t  dlpf_q16;
    int32_t  flt_y;                 /* one-pole state, Q15              */
    int32_t  flt_k;                 /* Q15; 0 = bypass                  */

    /* note queue timeline */
    qentry   q[KSYN_QUEUE_LEN];
    volatile uint8_t q_head, q_tail; /* SPSC ring indices (mod 256)     */
    uint8_t  tl_active, tl_wait;    /* timeline armed / awaiting tick   */
    uint16_t dur_left;              /* ticks left on the current note   */
} ksyn_voice;

static ksyn_voice g_v[KSYN_VOICES];

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void ksyn_sound_init(ksyn_sound *s) {
    memset(s, 0, sizeof(*s));
    s->wave = KSYN_SQUARE;
    s->volume = 200;
    s->duty = 128;
    s->attack_ms = 2;
    s->sustain = 255;
    s->hold_ms = KSYN_HOLD_INF;
    s->release_ms = 30;
    s->lpf = 255;
}

void ksyn_init(int rate) {
    g_sr = rate;
    memset(g_v, 0, sizeof(g_v));
    for (int i = 0; i < KSYN_VOICES; i++)
        g_v[i].rng = 0x9E3779B9u ^ (uint32_t)(i * 0x61C88647u);

    /* Pitch LUT: phase increment for each MIDI note (A4 = 69 = 440 Hz),
     * capped just below Nyquist so runaway slides stall instead of
     * wrapping into fold-back garbage. */
    g_inc_max = 0x7FFF0000u; /* ~Nyquist */
    for (int n = 0; n < 133; n++) {
        double f = 440.0 * pow(2.0, (n - 69) / 12.0);
        double inc = f * 4294967296.0 / (double)rate;
        if (inc > (double)g_inc_max) inc = (double)g_inc_max;
        g_note_inc[n] = (uint32_t)inc;
    }

    /* One-pole low-pass coefficients: cutoff index i (= param/8) maps
     * exponentially 40 Hz .. ~10.2 kHz; k = 1 - exp(-2*pi*fc/sr), Q15. */
    for (int i = 0; i <= 32; i++) {
        double fc = 40.0 * pow(2.0, i / 4.0);
        double k = 1.0 - exp(-2.0 * 3.14159265358979 * fc / (double)rate);
        if (k > 0.9995) k = 0.9995;
        g_lpf_k[i] = (uint16_t)(k * 32768.0);
    }

    g_tick_sec_q16 = (uint32_t)(65536.0 * CTRL / (double)rate);
    g_vib_unit = (uint32_t)(4294967296.0 * CTRL / (double)rate);

    g_tempo_on = 0;
    g_ticks_done = g_ticks_taken = 0;
    g_ctrl_pos = 0;
    g_seq = 0;

    g_blep = 1;
    g_tone_y = 0;
    ksyn_set_tone(232); /* ~6 kHz default roll-off */
}

/* One-pole coefficient for a 0..255 cutoff (255 = bypass -> 0). */
static inline int32_t lpf_coeff(uint32_t c) {
    if (c >= 255) return 0;
    uint32_t i = c >> 3, f = c & 7;
    uint32_t a = g_lpf_k[i];
    return (int32_t)(a + (((g_lpf_k[i + 1] - a) * f) >> 3));
}

void ksyn_set_gain(uint8_t gain) {
    g_master_q15 = (int32_t)gain << 7;
}

void ksyn_set_tone(uint8_t cutoff) {
    g_tone_k = lpf_coeff(cutoff);
}

void ksyn_set_blep(int enable) {
    g_blep = enable;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t ms_to_ticks(uint32_t ms) {
    return (ms * (uint32_t)g_sr) / (CTRL * 1000u);
}

/* Q8.8 MIDI pitch -> phase increment, linear interp between semitones. */
static inline uint32_t pitch_to_inc(int32_t p_q8) {
    if (p_q8 < 0) p_q8 = 0;
    if (p_q8 > PITCH_MAX) p_q8 = PITCH_MAX;
    uint32_t n = (uint32_t)p_q8 >> 8;
    uint32_t f = (uint32_t)p_q8 & 255;
    uint32_t a = g_note_inc[n];
    uint32_t inc = a + ((g_note_inc[n + 1] - a) >> 8) * f;
    return inc > g_inc_max ? g_inc_max : inc;
}

/* Envelope step for a linear segment of `range` over `ticks`. */
static inline int32_t seg_step(int32_t range, uint32_t ticks) {
    if (ticks == 0) return range;              /* one-tick transition */
    return range / (int32_t)ticks + 1;
}

/* ------------------------------------------------------------------ */
/* Note lifecycle (render/IRQ context, or main loop before render)     */
/* ------------------------------------------------------------------ */

static void voice_note_on(ksyn_voice *v, const ksyn_sound *s, int note, int vel) {
    int was_active = v->active;
    int32_t env_keep = was_active ? v->env : 0;
    int32_t flt_keep = was_active ? v->flt_y : 0;

    v->active = 0; /* keep the IRQ off this voice while we write it */

    if (s != &v->snd)
        v->snd = *s;
    s = &v->snd;

    /* envelope: retrigger from the current level so there is no click */
    v->env = env_keep;
    v->stage = ST_ATK;
    v->atk_step = seg_step(ENV_ONE, ms_to_ticks(s->attack_ms));
    v->sus_q15 = (int32_t)s->sustain << 7 | (s->sustain >> 1);
    v->dec_step = seg_step(ENV_ONE - v->sus_q15, ms_to_ticks(s->decay_ms));
    v->rel_step = 0;
    v->hold_ctr = (s->hold_ms == KSYN_HOLD_INF) ? -1
                : (int32_t)ms_to_ticks(s->hold_ms);
    v->amp_q15 = ((int32_t)s->volume * (int32_t)vel) >> 1;
    v->rel_req = 0;

    /* pitch */
    v->base_q8 = note << 8;
    v->acc_q8 = 0;
    v->slide_q8 = (int32_t)s->slide << 8;
    v->dslide_q8 = (int32_t)(((int64_t)s->curve << 8) * g_tick_sec_q16 >> 16);
    v->vib_phase = 0x40000000u; /* start at the LFO zero-crossing */
    v->vib_inc = (uint32_t)s->vib_rate * g_vib_unit;
    v->vib_depth_q8 = (int32_t)s->vib_depth << 5;

    /* arpeggio */
    v->arp_idx = 0;
    v->arp_ctr = 0;
    v->arp_ticks = (s->arp_mode != KSYN_ARP_OFF && s->arp_len > 0)
                 ? (uint16_t)(ms_to_ticks(s->arp_ms) ? ms_to_ticks(s->arp_ms) : 1)
                 : 0;

    /* oscillator: phase and noise state run on for continuity */
    v->duty_q16 = (int32_t)s->duty << 16;
    v->dduty_q16 = (int32_t)s->duty_sweep * 4 * (int32_t)g_tick_sec_q16;

    /* filter */
    v->lpf_q16 = (int32_t)s->lpf << 16;
    v->dlpf_q16 = (int32_t)s->lpf_sweep * (int32_t)g_tick_sec_q16;
    v->flt_y = flt_keep;
    v->flt_k = 0;

    v->seq = ++g_seq;
    v->fresh = 1;
    v->active = 1;
}

static void voice_enter_release(ksyn_voice *v) {
    if (v->stage == ST_REL) return;
    v->stage = ST_REL;
    v->rel_step = seg_step(v->env, ms_to_ticks(v->snd.release_ms));
}

/* One control tick for a voice: envelope, pitch, filter, gain ramp.
 * ramp_len: samples until the next control boundary. */
static void voice_ctrl(ksyn_voice *v, int ramp_len) {
    v->fresh = 0;

    if (v->rel_req) {
        v->rel_req = 0;
        voice_enter_release(v);
    }

    /* envelope */
    switch (v->stage) {
    case ST_ATK:
        v->env += v->atk_step;
        if (v->env >= ENV_ONE) { v->env = ENV_ONE; v->stage = ST_DEC; }
        break;
    case ST_DEC:
        v->env -= v->dec_step;
        if (v->env <= v->sus_q15) { v->env = v->sus_q15; v->stage = ST_HOLD; }
        break;
    case ST_HOLD:
        if (v->sus_q15 == 0) {
            /* one-shot sound (sustain 0): decay already faded to silence */
            v->stage = ST_REL;
            v->env = 0;
        } else if (v->hold_ctr >= 0 && v->hold_ctr-- == 0) {
            voice_enter_release(v);
        }
        break;
    case ST_REL:
        v->env -= v->rel_step;
        if (v->env <= 0) {
            v->env = 0;
            v->gain_q23 = 0;
            v->gstep_q23 = 0;
            v->active = 0;
            return;
        }
        break;
    }

    /* pitch: curve -> slide -> accumulation */
    v->slide_q8 += v->dslide_q8;
    v->acc_q8 += (int32_t)(((int64_t)v->slide_q8 * g_tick_sec_q16) >> 16);

    int32_t p = v->base_q8 + v->acc_q8;

    if (v->vib_depth_q8) {
        v->vib_phase += v->vib_inc;
        int32_t t = (int32_t)v->vib_phase;
        t ^= t >> 31;                          /* |saw| = triangle    */
        int32_t tri_q15 = ((t >> 15) - 32768); /* -32768..32767       */
        p += (tri_q15 * v->vib_depth_q8) >> 15;
    }

    if (v->arp_ticks) {
        if (++v->arp_ctr >= v->arp_ticks) {
            v->arp_ctr = 0;
            uint8_t next = v->arp_idx + 1;
            if (next >= v->snd.arp_len)
                next = (v->snd.arp_mode == KSYN_ARP_LOOP) ? 0 : v->arp_idx;
            v->arp_idx = next;
        }
        p += (int32_t)v->snd.arp[v->arp_idx] << 8;
    }

    v->inc = pitch_to_inc(p);
    if (v->snd.wave == KSYN_NOISE) {
        /* LFSR clock = 8x the note frequency, capped at once per sample
         * (high notes = white noise, low notes = crunchy rumble). */
        v->inc = (v->inc > 0x1FFFFFFFu) ? 0xFFFFFFFFu : v->inc << 3;
    }

    /* duty sweep */
    if (v->dduty_q16) {
        v->duty_q16 += v->dduty_q16;
        if (v->duty_q16 < (8 << 16)) v->duty_q16 = 8 << 16;
        if (v->duty_q16 > (248 << 16)) v->duty_q16 = 248 << 16;
    }

    /* filter sweep + coefficient */
    if (v->dlpf_q16) {
        v->lpf_q16 += v->dlpf_q16;
        if (v->lpf_q16 < 0) v->lpf_q16 = 0;
        if (v->lpf_q16 > (255 << 16)) v->lpf_q16 = 255 << 16;
    }
    v->flt_k = lpf_coeff((uint32_t)v->lpf_q16 >> 16);

    /* gain ramp toward env * amp over the coming ramp_len samples */
    {
        int32_t target_q23 = ((v->env * v->amp_q15) >> 15) << 8;
        v->gstep_q23 = (target_q23 - v->gain_q23) / (ramp_len > 0 ? ramp_len : 1);
    }
}

/* ------------------------------------------------------------------ */
/* Note queues + tempo                                                 */
/* ------------------------------------------------------------------ */

static inline int queue_count(const ksyn_voice *v) {
    return (uint8_t)(v->q_tail - v->q_head);
}

/* Consume queue entries until one produces sound/rest with a duration. */
static void timeline_advance(ksyn_voice *v) {
    for (;;) {
        if (queue_count(v) == 0) {
            voice_enter_release(v);
            v->tl_active = 0;
            return;
        }
        qentry *e = &v->q[v->q_head % KSYN_QUEUE_LEN];
        int16_t note = e->note;
        uint8_t vel = e->vel, flags = e->flags;
        uint16_t dur = e->dur;
        /* copy the sound out before releasing the slot to the producer */
        ksyn_sound tmp;
        const ksyn_sound *s = &v->snd;
        if (e->has_snd) { tmp = e->snd; s = &tmp; }
        v->q_head++;

        if (note == KSYN_REST) {
            voice_enter_release(v);
        } else if ((flags & KSYN_QF_LEGATO) && v->active) {
            v->base_q8 = (int32_t)note << 8;
            v->acc_q8 = 0;
        } else {
            voice_note_on(v, s, note, vel);
        }
        v->dur_left = dur;
        if (dur > 0) return; /* zero-length entries chain immediately */
    }
}

static void do_tick(void) {
    g_ticks_done++;
    for (int i = 0; i < KSYN_VOICES; i++) {
        ksyn_voice *v = &g_v[i];
        if (!v->tl_active) continue;
        if (v->tl_wait) {
            v->tl_wait = 0;
            timeline_advance(v);
        } else if (v->dur_left > 0 && --v->dur_left == 0) {
            timeline_advance(v);
        }
    }
    if (g_tick_cb) g_tick_cb(g_tick_user);
}

static inline int32_t next_tick_len(void) {
    int32_t len = (int32_t)g_tick_base;
    g_tick_acc += g_tick_rem;
    if (g_tick_acc >= g_tick_den) {
        g_tick_acc -= g_tick_den;
        len++;
    }
    return len;
}

void ksyn_set_tempo(int bpm, int ticks_per_beat) {
    if (bpm <= 0 || ticks_per_beat <= 0) {
        g_tempo_on = 0;
        return;
    }
    uint32_t num = (uint32_t)g_sr * 60u;
    uint32_t den = (uint32_t)bpm * (uint32_t)ticks_per_beat;
    g_tick_den = den;
    g_tick_base = num / den;
    g_tick_rem = num % den;
    g_tick_acc = 0;
    g_to_tick = next_tick_len();
    g_tempo_on = 1;
}

void ksyn_set_tick_callback(void (*cb)(void *), void *user) {
    g_tick_user = user;
    g_tick_cb = cb;
}

int ksyn_take_ticks(void) {
    uint32_t done = g_ticks_done;
    int n = (int)(done - g_ticks_taken);
    g_ticks_taken = done;
    return n;
}

uint32_t ksyn_tick_now(void) {
    return g_ticks_done;
}

int ksyn_queue(int voice, const ksyn_sound *sound, int note, int vel,
               int dur_ticks, int flags) {
    if (voice < 0 || voice >= KSYN_VOICES) return 0;
    if (dur_ticks < 0 || dur_ticks > 0xFFFF) return 0;
    ksyn_voice *v = &g_v[voice];
    if (queue_count(v) >= KSYN_QUEUE_LEN) return 0;

    qentry *e = &v->q[v->q_tail % KSYN_QUEUE_LEN];
    e->note = (int16_t)note;
    e->vel = (uint8_t)(vel < 0 ? 0 : vel > 255 ? 255 : vel);
    e->flags = (uint8_t)flags;
    e->dur = (uint16_t)dur_ticks;
    e->has_snd = (sound != 0);
    if (sound) e->snd = *sound;
    v->q_tail++;

    if (!v->tl_active) {
        v->tl_wait = 1;
        v->tl_active = 1;
    }
    return 1;
}

int ksyn_queue_space(int voice) {
    if (voice < 0 || voice >= KSYN_VOICES) return 0;
    return KSYN_QUEUE_LEN - queue_count(&g_v[voice]);
}

void ksyn_queue_clear(int voice) {
    if (voice < 0 || voice >= KSYN_VOICES) return;
    ksyn_voice *v = &g_v[voice];
    v->q_head = v->q_tail;
}

/* ------------------------------------------------------------------ */
/* Immediate mode                                                      */
/* ------------------------------------------------------------------ */

int ksyn_play(const ksyn_sound *s, int note, int vel, int voice) {
    if (!s || note < 0 || note > 127) return -1;
    if (vel < 0) vel = 0;
    if (vel > 255) vel = 255;

    if (voice == KSYN_AUTO) {
        /* free voice > oldest fire-and-forget voice > oldest overall */
        int best = -1;
        for (int pass = 0; pass < 3 && best < 0; pass++) {
            uint32_t best_age = 0;
            for (int i = 0; i < KSYN_VOICES; i++) {
                ksyn_voice *v = &g_v[i];
                if (pass == 0 && (v->active || v->tl_active)) continue;
                if (pass == 1 && (!v->active || v->tl_active)) continue;
                if (pass == 0) { best = i; break; }
                int32_t age = (int32_t)(g_seq - v->seq);
                if (best < 0 || age > (int32_t)best_age) {
                    best = i;
                    best_age = (uint32_t)age;
                }
            }
        }
        voice = best;
    }
    if (voice < 0 || voice >= KSYN_VOICES) return -1;

    voice_note_on(&g_v[voice], s, note, vel);
    return voice;
}

void ksyn_release(int voice) {
    if (voice < 0 || voice >= KSYN_VOICES) return;
    g_v[voice].rel_req = 1;
}

void ksyn_stop(int voice) {
    int lo = voice, hi = voice;
    if (voice == KSYN_AUTO) { lo = 0; hi = KSYN_VOICES - 1; }
    if (lo < 0 || hi >= KSYN_VOICES) return;
    for (int i = lo; i <= hi; i++) {
        ksyn_voice *v = &g_v[i];
        v->active = 0;
        v->tl_active = 0;
        v->tl_wait = 0;
        v->dur_left = 0;
        v->q_head = v->q_tail;
        v->env = 0;
        v->gain_q23 = 0;
        v->gstep_q23 = 0;
    }
}

int ksyn_voice_active(int voice) {
    if (voice < 0 || voice >= KSYN_VOICES) return 0;
    return g_v[voice].active;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static int32_t g_acc[CTRL];   /* mix accumulator, one control period max */
static int32_t g_vbuf[CTRL];  /* per-voice scratch, Q15                  */

/* polyBLEP residual (Q15, +/-32768) for a unit upward step at phase 0.
 * Adds a 2-sample polynomial correction on each side of the edge; away
 * from edges it is two compares and returns 0. The divisions run only on
 * the ~4 corrected samples per waveform period. */
static inline int32_t blep_r(uint32_t ph, uint32_t inc) {
    if (ph < inc) {                       /* just after the edge  */
        int32_t x = (int32_t)(((uint64_t)ph << 15) / inc);
        return (x << 1) - ((x * x) >> 15) - 32768;
    }
    if (ph >= (0u - inc)) {               /* just before the edge */
        int32_t x = -(int32_t)(((uint64_t)(0u - ph) << 15) / inc);
        return ((x * x) >> 15) + (x << 1) + 32768;
    }
    return 0;
}

static void render_voice(ksyn_voice *v, int nn) {
    uint32_t ph = v->phase, inc = v->inc;
    int32_t *b = g_vbuf;

    /* 1) raw waveform, Q15-ish full scale */
    switch (v->snd.wave) {
    case KSYN_SQUARE: {
        /* zero-mean pulse: hi/lo levels are duty-compensated so any duty
         * has no DC component (a naive pulse thumps the line out). At 50%
         * duty this is +/-27000. */
        uint32_t thr = (uint32_t)v->duty_q16 << 8;
        int32_t d8 = v->duty_q16 >> 16; /* 0..255 */
        int32_t hi = (54000 * (256 - d8)) >> 8;
        int32_t lo = -((54000 * d8) >> 8);
        if (g_blep) {
            /* band-limited: rising edge at phase 0, falling at thr;
             * the step height is 54000 regardless of duty, so each
             * correction is scaled by half of that. */
            for (int i = 0; i < nn; i++) {
                ph += inc;
                int32_t s = (ph < thr) ? hi : lo;
                s += (27000 * blep_r(ph, inc)) >> 15;
                s -= (27000 * blep_r(ph - thr, inc)) >> 15;
                b[i] = s;
            }
        } else {
            for (int i = 0; i < nn; i++) {
                ph += inc;
                b[i] = (ph < thr) ? hi : lo;
            }
        }
        break;
    }
    case KSYN_TRI:
        for (int i = 0; i < nn; i++) {
            ph += inc;
            int32_t a = (int32_t)ph;
            a ^= a >> 31;
            b[i] = (a >> 15) - 32768;
        }
        break;
    case KSYN_SAW:
        if (g_blep) {
            /* the signed-phase saw jumps +32767 -> -32768 at phase
             * 0x80000000, so shift that edge onto 0 for the residual */
            for (int i = 0; i < nn; i++) {
                ph += inc;
                b[i] = ((int32_t)ph >> 16) - blep_r(ph + 0x80000000u, inc);
            }
        } else {
            for (int i = 0; i < nn; i++) {
                ph += inc;
                b[i] = (int32_t)ph >> 16;
            }
        }
        break;
    default: { /* KSYN_NOISE: sample & hold LFSR clocked by pitch */
        uint32_t x = v->rng;
        int32_t nv = v->noise_val;
        for (int i = 0; i < nn; i++) {
            ph += inc;
            if (ph < inc) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                nv = (int32_t)(int16_t)(x >> 16);
            }
            b[i] = nv;
        }
        v->rng = x;
        v->noise_val = nv;
        break;
    }
    }
    v->phase = ph;

    /* 2) optional one-pole low-pass */
    if (v->flt_k) {
        int32_t y = v->flt_y, k = v->flt_k;
        for (int i = 0; i < nn; i++) {
            y += (k * (b[i] - y)) >> 15;
            b[i] = y;
        }
        v->flt_y = y;
    }

    /* 3) mix with per-sample gain ramp */
    {
        int32_t g = v->gain_q23, gs = v->gstep_q23;
        for (int i = 0; i < nn; i++) {
            g_acc[i] += (b[i] * (g >> 8)) >> 15;
            g += gs;
        }
        v->gain_q23 = g;
    }
}

void ksyn_render(int16_t *out, int n) {
    while (n > 0) {
        if (g_tempo_on && g_to_tick <= 0) {
            do_tick();
            g_to_tick += next_tick_len();
            continue;
        }

        if (g_ctrl_pos == 0) {
            for (int i = 0; i < KSYN_VOICES; i++)
                if (g_v[i].active) voice_ctrl(&g_v[i], CTRL);
        } else {
            /* voices armed asynchronously since the last boundary */
            for (int i = 0; i < KSYN_VOICES; i++)
                if (g_v[i].active && g_v[i].fresh)
                    voice_ctrl(&g_v[i], CTRL - g_ctrl_pos);
        }

        int chunk = CTRL - g_ctrl_pos;
        if (chunk > n) chunk = n;
        if (g_tempo_on && chunk > g_to_tick) chunk = g_to_tick;

        memset(g_acc, 0, (size_t)chunk * sizeof(int32_t));
        for (int i = 0; i < KSYN_VOICES; i++)
            if (g_v[i].active) render_voice(&g_v[i], chunk);

        if (g_tone_k) {
            /* master tone: one-pole low-pass on the mix. The mix can sum
             * several voices, so the delta multiply needs 64 bits. */
            int32_t y = g_tone_y, k = g_tone_k;
            for (int i = 0; i < chunk; i++) {
                y += (int32_t)(((int64_t)k * (g_acc[i] - y)) >> 15);
                int32_t s = (int32_t)(((int64_t)y * g_master_q15) >> 15);
                if (s > 32767) s = 32767;
                if (s < -32767) s = -32767;
                out[i] = (int16_t)s;
            }
            g_tone_y = y;
        } else {
            for (int i = 0; i < chunk; i++) {
                int32_t s = (int32_t)(((int64_t)g_acc[i] * g_master_q15) >> 15);
                if (s > 32767) s = 32767;
                if (s < -32767) s = -32767;
                out[i] = (int16_t)s;
            }
        }

        out += chunk;
        n -= chunk;
        g_ctrl_pos = (g_ctrl_pos + chunk) & CTRL_MASK;
        if (g_tempo_on) g_to_tick -= chunk;
    }
}

void ksyn_render_stereo(int16_t *out, int n) {
    int16_t tmp[CTRL];
    while (n > 0) {
        int chunk = n > CTRL ? CTRL : n;
        ksyn_render(tmp, chunk);
        for (int i = 0; i < chunk; i++) {
            out[2 * i] = tmp[i];
            out[2 * i + 1] = tmp[i];
        }
        out += 2 * chunk;
        n -= chunk;
    }
}

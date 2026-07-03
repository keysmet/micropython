/*
 * sfxr.h - Minimal SFXR sound effect synthesizer.
 *
 * Single-precision float only (no double), no libm in the hot path,
 * no heap. Suitable for Cortex-M4F (nRF52840) generation from an
 * I2S/DMA callback: fill a block per call with sfxr_generate().
 *
 * Fixed sample rate: 22050 Hz.
 *
 * Reference: DrPetter's sfxr (grimfang4 fork) / jsfxr.
 *
 *   Usage:
 *     sfxr_params p = { ... };          // or memset 0 + set fields
 *     sfxr_state  s;
 *     sfxr_reset(&s, &p);
 *     float buf[256];
 *     while (sfxr_playing(&s))
 *         sfxr_generate(&s, buf, 256);  // call from your DMA callback
 */
#ifndef SFXR_H
#define SFXR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Target output sample rate. This is the one knob to turn for your DAC/I2S
 * clock. It need not be exactly 22050; the engine calibration below adapts.
 * (Your hardware clock will rarely land on 22050 exactly, and that is fine —
 * the pitch error from a few Hz of rate mismatch is inaudible.) */
#ifndef SFXR_SAMPLE_RATE
#define SFXR_SAMPLE_RATE 22050
#endif

/* The sfxr core engine's period/time constants are calibrated to 44100 Hz.
 * To emit at SFXR_SAMPLE_RATE we run the engine SFXR_SUMMANDS ticks per
 * output sample and average them (this is what jsfxr does). The ratio is
 * rounded to the nearest integer: 44100/22050 = 2. */
#define SFXR_ENGINE_RATE 44100
#define SFXR_SUMMANDS    ((SFXR_ENGINE_RATE + SFXR_SAMPLE_RATE / 2) / SFXR_SAMPLE_RATE)

/* Input parameters. All p_* are the standard sfxr values in [-1,1] or [0,1].
 * Layout matches the well-known jsfxr JSON field order. */
typedef struct {
    int   wave_type;        /* 0 square, 1 sawtooth, 2 sine, 3 noise */

    float p_env_attack;     /* [0,1] */
    float p_env_sustain;    /* [0,1] */
    float p_env_punch;      /* [0,1] */
    float p_env_decay;      /* [0,1] */

    float p_base_freq;      /* [0,1] */
    float p_freq_limit;     /* [0,1] */
    float p_freq_ramp;      /* [-1,1] */
    float p_freq_dramp;     /* [-1,1] */

    float p_vib_strength;   /* [0,1] */
    float p_vib_speed;      /* [0,1] */

    float p_arp_mod;        /* [-1,1] */
    float p_arp_speed;      /* [0,1] */

    float p_duty;           /* [0,1] */
    float p_duty_ramp;      /* [-1,1] */

    float p_repeat_speed;   /* [0,1] */

    float p_pha_offset;     /* [-1,1] */
    float p_pha_ramp;       /* [-1,1] */

    float p_lpf_freq;       /* [0,1] (1 = off) */
    float p_lpf_ramp;       /* [-1,1] */
    float p_lpf_resonance;  /* [0,1] */
    float p_hpf_freq;       /* [0,1] */
    float p_hpf_ramp;       /* [-1,1] */

    float sound_vol;        /* master gain, e.g. 0.25 */
} sfxr_params;

/* Runtime synthesis state. Treat as opaque; kept in the header so it can
 * live on the stack / in a static, with no allocation. */
typedef struct {
    sfxr_params p;
    int   playing;

    /* rng */
    unsigned int rng;

    /* frequency */
    float fperiod, fmaxperiod, fslide, fdslide;
    int   period;
    int   phase;

    /* arpeggio */
    float arp_mod;
    int   arp_time, arp_limit;

    /* duty */
    float square_duty, square_slide;

    /* envelope */
    int   env_stage, env_time;
    int   env_length[3];
    float env_vol;

    /* vibrato */
    float vib_phase, vib_speed, vib_amp;

    /* low-pass filter */
    float fltp, fltdp, fltw, fltw_d, fltdmp, fltphp, flthp, flthp_d;

    /* phaser */
    float fphase, fdphase;
    int   iphase, ipp;
    float phaser_buffer[1024];

    /* noise */
    float noise_buffer[32];

    /* repeat */
    int   rep_time, rep_limit;

    /* output gain, precomputed from sound_vol (jsfxr: exp(sound_vol)-1) */
    float gain;
} sfxr_state;

/* (Re)initialize state from params and start playback. */
void sfxr_reset(sfxr_state *s, const sfxr_params *p);

/* Generate `count` mono float samples in [-1,1] into `out`.
 * Once the sound ends, remaining samples are filled with silence.
 * Returns the number of non-silent samples actually synthesized. */
int  sfxr_generate(sfxr_state *s, float *out, int count);

/* Generate `frames` interleaved stereo int16 sample frames into `out`.
 * Both channels carry the same mono signal (out has 2*frames int16s).
 * Once the sound ends, remaining frames are filled with silence.
 * Returns the number of non-silent frames actually synthesized. */
int  sfxr_generate_s16_stereo(sfxr_state *s, short *out, int frames);

/* Non-zero while the sound is still producing output. */
static inline int sfxr_playing(const sfxr_state *s) { return s->playing; }

#ifdef __cplusplus
}
#endif
#endif /* SFXR_H */

/*
 * ksynth.h - Keysmet mini synth.
 *
 * A small chiptune synthesizer for the KSM1 (nRF52840, 64 MHz Cortex-M4F),
 * designed to be driven from MicroPython and rendered from an I2S DMA IRQ.
 *
 * Design constraints (see brief.md):
 *   - All-integer hot path (fixed point), no libm after ksyn_init().
 *   - Budget: well under 5% CPU per voice at 22050 Hz, so 4+ simultaneous
 *     voices are comfortable (SFXR cost ~25%/voice on the same target).
 *   - Flat parameter model: one POD struct per sound, no patching graph.
 *   - Pitch is integer MIDI notes; modulation (slide/vibrato/arpeggio) is
 *     what turns plain waveforms into SNES-style sound effects.
 *
 * Architecture:
 *   - KSYN_VOICES identical voices. Each voice = one oscillator
 *     (square/tri/saw/noise) + linear AHDSR envelope + pitch modulators +
 *     optional one-pole low-pass filter.
 *   - Audio-rate loop is trivial (phase accumulate, wave lookup, gain ramp).
 *     Everything expressive runs at control rate: once per KSYN_CTRL_SAMPLES
 *     samples (64 @ 22050 Hz = ~2.9 ms) each active voice recomputes pitch,
 *     envelope target and filter, and the per-sample gain is linearly ramped
 *     to the new target, so there is no zipper noise.
 *   - A tempo clock (BPM x subdivisions) counts in samples with exact
 *     Bresenham accumulation. Per-voice note queues advance on tempo ticks,
 *     so queued music notes are sample-accurate and seamless. A C callback
 *     plus a pending-tick counter let the host (MicroPython main loop) top
 *     up the queues ahead of time.
 *
 * Threading model (matches modaudio.c): ksyn_render() runs in the DMA IRQ.
 * All other calls run in the main loop. Commands take effect at the next
 * render block boundary; state words are written in an order that keeps a
 * concurrent render harmless (worst case: a voice plays one stale block).
 *
 * Usage:
 *     ksyn_init(22050);
 *     ksyn_sound s; ksyn_sound_init(&s);
 *     s.wave = KSYN_SQUARE; s.slide = 80; s.decay_ms = 300;
 *     ksyn_play(&s, 60, 255, KSYN_AUTO);       // fire and forget
 *     ...
 *     ksyn_render(buf, 128);                   // from the audio callback
 */
#ifndef KSYNTH_H
#define KSYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                          */
/* ------------------------------------------------------------------ */

#ifndef KSYN_VOICES
#define KSYN_VOICES 8          /* voice pool size (RAM + worst-case CPU) */
#endif

#ifndef KSYN_CTRL_SAMPLES
#define KSYN_CTRL_SAMPLES 64   /* samples per control tick (power of two) */
#endif

#define KSYN_ARP_MAX   24      /* max arpeggio steps (SMB-mushroom-style
                                  runs need 21: a 7-note figure x3)      */
#define KSYN_QUEUE_LEN 8       /* per-voice queued notes                 */

/* ------------------------------------------------------------------ */
/* Sound definition                                                    */
/* ------------------------------------------------------------------ */

enum {
    KSYN_SQUARE = 0,           /* pulse wave, variable duty              */
    KSYN_TRI    = 1,           /* triangle                               */
    KSYN_SAW    = 2,           /* sawtooth                               */
    KSYN_NOISE  = 3,           /* pitched LFSR noise (sample & hold)     */
};

enum {
    KSYN_ARP_OFF  = 0,
    KSYN_ARP_LOOP = 1,         /* cycle through arp[] forever            */
    KSYN_ARP_ONCE = 2,         /* play arp[] once, hold the last step    */
};

#define KSYN_HOLD_INF 0xFFFF   /* hold_ms: sustain until ksyn_release()  */

/*
 * One sound = one flat set of parameters. Plain integers throughout so a
 * MicroPython dict/bytes can fill it without float conversions.
 * ksyn_play() copies the struct; it does not need to outlive the call.
 */
typedef struct {
    uint8_t  wave;          /* KSYN_SQUARE/TRI/SAW/NOISE                 */
    uint8_t  volume;        /* 0..255 voice gain                         */
    uint8_t  duty;          /* square only: pulse width 0..255 (128=50%) */
    int8_t   duty_sweep;    /* duty change per second, in duty units x4  */
                            /* (e.g. -32 sweeps 128 duty units in 1 s)   */

    /* Envelope (linear segments, times in ms).
     * attack: 0 -> full, decay: full -> sustain, hold: time at sustain
     * (KSYN_HOLD_INF = until release), release: sustain -> 0. */
    uint16_t attack_ms;
    uint16_t decay_ms;
    uint8_t  sustain;       /* 0..255 sustain level                      */
    uint16_t hold_ms;
    uint16_t release_ms;

    /* Pitch modulation. slide bends the note continuously; curve bends
     * the slide (SFXR-style laser curvature). */
    int16_t  slide;         /* semitones per second                      */
    int16_t  curve;         /* slide change, semitones per second^2      */
    uint8_t  vib_rate;      /* vibrato LFO rate, Hz                      */
    uint8_t  vib_depth;     /* vibrato peak depth, 1/8 semitones         */

    /* Arpeggio: semitone offsets stepped at a fixed rate. This is the
     * classic coin/stinger device (e.g. arp={0,5}, once, 80 ms). */
    uint16_t arp_ms;        /* ms per step (0 disables)                  */
    uint8_t  arp_mode;      /* KSYN_ARP_OFF/LOOP/ONCE                    */
    uint8_t  arp_len;       /* used entries in arp[]                     */
    int8_t   arp[KSYN_ARP_MAX];

    /* One-pole low-pass. cutoff 0..255 maps exponentially ~40 Hz..Nyquist;
     * 255 = bypass (zero cost). */
    uint8_t  lpf;
    int16_t  lpf_sweep;     /* cutoff units per second                   */
} ksyn_sound;

/* Fill s with neutral "plain instrument" defaults: square 50%, volume 200,
 * 2 ms attack, full sustain, infinite hold, 30 ms release, no modulation. */
void ksyn_sound_init(ksyn_sound *s);

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

/* Initialize / reset everything. The only place float/libm math runs.
 * rate: output sample rate in Hz (e.g. 22050). */
void ksyn_init(int rate);

/* Render n mono int16 samples. Call from the audio DMA callback.
 * Advances the tempo clock and note queues sample-accurately. */
void ksyn_render(int16_t *out, int n);

/* Same signal duplicated into interleaved stereo frames (2*n int16s),
 * matching the KSM1 I2S packing. */
void ksyn_render_stereo(int16_t *out, int n);

/* Master gain 0..255 (default 128). Applied after voice mixing, before
 * saturation. */
void ksyn_set_gain(uint8_t gain);

/* Output tone control: one-pole low-pass on the final mix. cutoff 0..255
 * (same exponential mapping as ksyn_sound.lpf), 255 = bypass. Default 232
 * (~6 kHz): rolls off the harsh top end of naive squares, SNES-style. */
void ksyn_set_tone(uint8_t cutoff);

/* Enable/disable polyBLEP band-limiting of square/saw edges (default on).
 * A/B knob for sound-design work; off saves a few cycles per sample and
 * sounds rawer/fizzier. */
void ksyn_set_blep(int enable);

/* ------------------------------------------------------------------ */
/* Immediate mode (sound effects)                                      */
/* ------------------------------------------------------------------ */

#define KSYN_AUTO (-1)

/* Start a sound. note: MIDI note number (69 = A4 = 440 Hz), vel: 0..255.
 * voice: explicit voice index, or KSYN_AUTO to allocate (free voice first,
 * else steals the oldest fire-and-forget voice, else the oldest overall —
 * so SFX spam never kills a music channel while an idle voice exists).
 * Returns the voice index used, or -1 if arguments are invalid. */
int ksyn_play(const ksyn_sound *s, int note, int vel, int voice);

/* Enter the release phase (a no-op if already released/idle). */
void ksyn_release(int voice);

/* Hard-stop a voice (KSYN_AUTO = all voices) and clear its queue. */
void ksyn_stop(int voice);

/* Non-zero while the voice is audible. */
int ksyn_voice_active(int voice);

/* ------------------------------------------------------------------ */
/* Tempo + note queues (music)                                         */
/* ------------------------------------------------------------------ */

/* Set the tempo clock: ticks_per_beat subdivisions at bpm beats/min.
 * bpm = 0 stops the clock (queued notes freeze). Tick boundaries fall on
 * exact sample positions (no drift). */
void ksyn_set_tempo(int bpm, int ticks_per_beat);

/* Called from inside ksyn_render() at every tempo tick, after queue
 * advancement — notes queued from the callback for the following ticks are
 * sample-accurate. Runs in the audio IRQ on target: keep it tiny, and do
 * NOT call MicroPython from it — the MicroPython port should instead poll
 * ksyn_take_ticks() from the main loop and keep the queues topped up. */
void ksyn_set_tick_callback(void (*cb)(void *user), void *user);

/* Number of tempo ticks elapsed since the last call (and resets it).
 * Poll from the main loop to drive a Python-side sequencer. */
int ksyn_take_ticks(void);

/* Current absolute tick number (monotonic while tempo runs). */
uint32_t ksyn_tick_now(void);

#define KSYN_REST (-1)          /* queue a silence                       */
#define KSYN_QF_LEGATO 1        /* don't retrigger envelope: pitch change
                                   only (ties / slides between notes)    */

/* Queue a note on a voice, dur_ticks long. The first note queued on an
 * idle timeline starts at the next tempo tick; subsequent notes start when
 * the previous duration expires. sound may be NULL to reuse the voice's
 * current sound (typical for melodies; non-NULL is copied immediately).
 * When a queued note's duration ends and the queue is empty, the note is
 * released (envelope tail) and the timeline goes idle.
 * Returns 0 if the queue is full or arguments are invalid, 1 otherwise. */
int ksyn_queue(int voice, const ksyn_sound *sound, int note, int vel,
               int dur_ticks, int flags);

/* Free slots in the voice's queue (KSYN_QUEUE_LEN when idle). */
int ksyn_queue_space(int voice);

/* Drop queued notes (the playing note continues). */
void ksyn_queue_clear(int voice);

#ifdef __cplusplus
}
#endif
#endif /* KSYNTH_H */

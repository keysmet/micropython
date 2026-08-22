/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Keysmet
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// MicroPython bindings for the ksynth chiptune synthesizer (see README.md).
// Port-agnostic: all functions map straight onto the ksyn_* C API; the only
// port-specific piece is ksynth_port_start() (audio output pump).
//
//     import synth
//     jump = synth.Sound(decay=280, sustain=0, hold=0, slide=70)
//     synth.play(jump, 64)

#include "py/runtime.h"

#if MICROPY_PY_SYNTH

#include <string.h>
#include "py/obj.h"
#include "ksynth.h"
#include "modsynth.h"

#define SYNTH_SAMPLE_RATE (22050)

static bool synth_ready;

static void ensure_init(void) {
    if (!synth_ready) {
        ksyn_init(SYNTH_SAMPLE_RATE);
        ksynth_port_start(SYNTH_SAMPLE_RATE);
        synth_ready = true;
    }
}

// ---------------------------------------------------------------------------
// synth.Sound(**params) - one flat sound definition (see ksynth.h)
// ---------------------------------------------------------------------------

typedef struct _synth_sound_obj_t {
    mp_obj_base_t base;
    ksyn_sound snd;
} synth_sound_obj_t;

extern const mp_obj_type_t synth_sound_type;

static mp_int_t get_clamped(mp_obj_t val, mp_int_t lo, mp_int_t hi) {
    mp_int_t v = mp_obj_get_int(val);
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return v;
}

static void sound_set_field(ksyn_sound *s, qstr q, mp_obj_t val) {
    switch (q) {
        case MP_QSTR_wave:       s->wave = (uint8_t)get_clamped(val, 0, 3); break;
        case MP_QSTR_volume:     s->volume = (uint8_t)get_clamped(val, 0, 255); break;
        case MP_QSTR_duty:       s->duty = (uint8_t)get_clamped(val, 0, 255); break;
        case MP_QSTR_duty_sweep: s->duty_sweep = (int8_t)get_clamped(val, -128, 127); break;

        case MP_QSTR_attack:     s->attack_ms = (uint16_t)get_clamped(val, 0, 0xFFFF); break;
        case MP_QSTR_decay:      s->decay_ms = (uint16_t)get_clamped(val, 0, 0xFFFF); break;
        case MP_QSTR_sustain:    s->sustain = (uint8_t)get_clamped(val, 0, 255); break;
        case MP_QSTR_release:    s->release_ms = (uint16_t)get_clamped(val, 0, 0xFFFF); break;
        case MP_QSTR_hold: {
            // hold < 0 means "sustain until release()" (KSYN_HOLD_INF)
            mp_int_t v = mp_obj_get_int(val);
            s->hold_ms = (v < 0 || v >= KSYN_HOLD_INF) ? KSYN_HOLD_INF : (uint16_t)v;
            break;
        }

        case MP_QSTR_slide:      s->slide = (int16_t)get_clamped(val, -32768, 32767); break;
        case MP_QSTR_curve:      s->curve = (int16_t)get_clamped(val, -32768, 32767); break;
        case MP_QSTR_vib_rate:   s->vib_rate = (uint8_t)get_clamped(val, 0, 255); break;
        case MP_QSTR_vib_depth:  s->vib_depth = (uint8_t)get_clamped(val, 0, 255); break;

        case MP_QSTR_arp: {
            // tuple/list of semitone offsets; implies ARP_ONCE unless
            // arp_mode is also given
            size_t len;
            mp_obj_t *items;
            mp_obj_get_array(val, &len, &items);
            if (len > KSYN_ARP_MAX) {
                mp_raise_ValueError(MP_ERROR_TEXT("arp too long"));
            }
            for (size_t i = 0; i < len; i++) {
                s->arp[i] = (int8_t)get_clamped(items[i], -128, 127);
            }
            s->arp_len = (uint8_t)len;
            if (s->arp_mode == KSYN_ARP_OFF) {
                s->arp_mode = KSYN_ARP_ONCE;
            }
            break;
        }
        case MP_QSTR_arp_ms:     s->arp_ms = (uint16_t)get_clamped(val, 0, 0xFFFF); break;
        case MP_QSTR_arp_mode:   s->arp_mode = (uint8_t)get_clamped(val, 0, 2); break;

        case MP_QSTR_lpf:        s->lpf = (uint8_t)get_clamped(val, 0, 255); break;
        case MP_QSTR_lpf_sweep:  s->lpf_sweep = (int16_t)get_clamped(val, -32768, 32767); break;

        default:
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown sound param '%q'"), q);
    }
}

static mp_obj_t sound_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    synth_sound_obj_t *o = mp_obj_malloc(synth_sound_obj_t, type);
    ksyn_sound_init(&o->snd);
    // sensible default arp step so arp=(...) alone audibly works
    o->snd.arp_ms = 80;
    const mp_obj_t *kw = args + n_args;
    for (size_t i = 0; i < n_kw; i++) {
        sound_set_field(&o->snd, mp_obj_str_get_qstr(kw[2 * i]), kw[2 * i + 1]);
    }
    return MP_OBJ_FROM_PTR(o);
}

MP_DEFINE_CONST_OBJ_TYPE(
    synth_sound_type,
    MP_QSTR_Sound,
    MP_TYPE_FLAG_NONE,
    make_new, sound_make_new
    );

// NULL when obj is None, the sound pointer otherwise (TypeError if neither).
static const ksyn_sound *get_sound_or_null(mp_obj_t obj) {
    if (obj == mp_const_none) {
        return NULL;
    }
    if (!mp_obj_is_type(obj, &synth_sound_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected a synth.Sound"));
    }
    return &((synth_sound_obj_t *)MP_OBJ_TO_PTR(obj))->snd;
}

// ---------------------------------------------------------------------------
// Immediate mode
// ---------------------------------------------------------------------------

// synth.play(sound, note, vel=255, voice=-1) -> voice index
static mp_obj_t synth_play(size_t n_args, const mp_obj_t *args) {
    ensure_init();
    const ksyn_sound *s = get_sound_or_null(args[0]);
    if (s == NULL) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected a synth.Sound"));
    }
    int note = (int)mp_obj_get_int(args[1]);
    int vel = n_args > 2 ? (int)mp_obj_get_int(args[2]) : 255;
    int voice = n_args > 3 ? (int)mp_obj_get_int(args[3]) : KSYN_AUTO;
    return MP_OBJ_NEW_SMALL_INT(ksyn_play(s, note, vel, voice));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(synth_play_obj, 2, 4, synth_play);

// synth.release(voice)
static mp_obj_t synth_release(mp_obj_t voice_in) {
    ksyn_release((int)mp_obj_get_int(voice_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_release_obj, synth_release);

// synth.stop(voice=-1) - hard stop one voice or all
static mp_obj_t synth_stop(size_t n_args, const mp_obj_t *args) {
    ksyn_stop(n_args > 0 ? (int)mp_obj_get_int(args[0]) : KSYN_AUTO);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(synth_stop_obj, 0, 1, synth_stop);

// synth.active(voice) -> bool
static mp_obj_t synth_active(mp_obj_t voice_in) {
    return mp_obj_new_bool(ksyn_voice_active((int)mp_obj_get_int(voice_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_active_obj, synth_active);

// ---------------------------------------------------------------------------
// Tempo + note queues
// ---------------------------------------------------------------------------

// synth.tempo(bpm, ticks_per_beat=4) - bpm 0 stops the clock
static mp_obj_t synth_tempo(size_t n_args, const mp_obj_t *args) {
    ensure_init();
    int bpm = (int)mp_obj_get_int(args[0]);
    int tpb = n_args > 1 ? (int)mp_obj_get_int(args[1]) : 4;
    ksyn_set_tempo(bpm, tpb);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(synth_tempo_obj, 1, 2, synth_tempo);

// synth.ticks() -> tempo ticks elapsed since the last call
static mp_obj_t synth_ticks(void) {
    return MP_OBJ_NEW_SMALL_INT(ksyn_take_ticks());
}
static MP_DEFINE_CONST_FUN_OBJ_0(synth_ticks_obj, synth_ticks);

// synth.tick_now() -> absolute tick counter
static mp_obj_t synth_tick_now(void) {
    return mp_obj_new_int_from_uint(ksyn_tick_now());
}
static MP_DEFINE_CONST_FUN_OBJ_0(synth_tick_now_obj, synth_tick_now);

// synth.queue(voice, note, dur, sound=None, vel=255, legato=False) -> bool
// note may be synth.REST; sound=None reuses the voice's current sound.
static mp_obj_t synth_queue(size_t n_args, const mp_obj_t *args) {
    ensure_init();
    int voice = (int)mp_obj_get_int(args[0]);
    int note = (int)mp_obj_get_int(args[1]);
    int dur = (int)mp_obj_get_int(args[2]);
    const ksyn_sound *s = n_args > 3 ? get_sound_or_null(args[3]) : NULL;
    int vel = n_args > 4 ? (int)mp_obj_get_int(args[4]) : 255;
    int flags = (n_args > 5 && mp_obj_is_true(args[5])) ? KSYN_QF_LEGATO : 0;
    return mp_obj_new_bool(ksyn_queue(voice, s, note, vel, dur, flags));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(synth_queue_obj, 3, 6, synth_queue);

// synth.queue_space(voice) -> free queue slots
static mp_obj_t synth_queue_space(mp_obj_t voice_in) {
    return MP_OBJ_NEW_SMALL_INT(ksyn_queue_space((int)mp_obj_get_int(voice_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_queue_space_obj, synth_queue_space);

// synth.queue_clear(voice)
static mp_obj_t synth_queue_clear(mp_obj_t voice_in) {
    ksyn_queue_clear((int)mp_obj_get_int(voice_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_queue_clear_obj, synth_queue_clear);

// ---------------------------------------------------------------------------
// Global controls
// ---------------------------------------------------------------------------

// synth.gain(g) - master gain 0..255
static mp_obj_t synth_gain(mp_obj_t g_in) {
    ksyn_set_gain((uint8_t)get_clamped(g_in, 0, 255));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_gain_obj, synth_gain);

// synth.tone(c) - output low-pass 0..255, 255 = bypass
static mp_obj_t synth_tone(mp_obj_t c_in) {
    ksyn_set_tone((uint8_t)get_clamped(c_in, 0, 255));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synth_tone_obj, synth_tone);

static const mp_rom_map_elem_t synth_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_synth) },
    { MP_ROM_QSTR(MP_QSTR_Sound), MP_ROM_PTR(&synth_sound_type) },

    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&synth_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&synth_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&synth_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&synth_active_obj) },

    { MP_ROM_QSTR(MP_QSTR_tempo), MP_ROM_PTR(&synth_tempo_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks), MP_ROM_PTR(&synth_ticks_obj) },
    { MP_ROM_QSTR(MP_QSTR_tick_now), MP_ROM_PTR(&synth_tick_now_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue), MP_ROM_PTR(&synth_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue_space), MP_ROM_PTR(&synth_queue_space_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue_clear), MP_ROM_PTR(&synth_queue_clear_obj) },

    { MP_ROM_QSTR(MP_QSTR_gain), MP_ROM_PTR(&synth_gain_obj) },
    { MP_ROM_QSTR(MP_QSTR_tone), MP_ROM_PTR(&synth_tone_obj) },

    // waveforms
    { MP_ROM_QSTR(MP_QSTR_SQUARE), MP_ROM_INT(KSYN_SQUARE) },
    { MP_ROM_QSTR(MP_QSTR_TRI), MP_ROM_INT(KSYN_TRI) },
    { MP_ROM_QSTR(MP_QSTR_SAW), MP_ROM_INT(KSYN_SAW) },
    { MP_ROM_QSTR(MP_QSTR_NOISE), MP_ROM_INT(KSYN_NOISE) },
    // arp modes
    { MP_ROM_QSTR(MP_QSTR_ARP_LOOP), MP_ROM_INT(KSYN_ARP_LOOP) },
    { MP_ROM_QSTR(MP_QSTR_ARP_ONCE), MP_ROM_INT(KSYN_ARP_ONCE) },
    // misc
    { MP_ROM_QSTR(MP_QSTR_REST), MP_ROM_INT(KSYN_REST) },
    { MP_ROM_QSTR(MP_QSTR_AUTO), MP_ROM_INT(KSYN_AUTO) },
    { MP_ROM_QSTR(MP_QSTR_VOICES), MP_ROM_INT(KSYN_VOICES) },
};
static MP_DEFINE_CONST_DICT(synth_module_globals, synth_module_globals_table);

const mp_obj_module_t synth_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&synth_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_synth, synth_module);

#endif // MICROPY_PY_SYNTH

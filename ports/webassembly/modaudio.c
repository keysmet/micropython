/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Keysmet
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

// WebAssembly backend for the KSM1 `audio` module. It shares the exact same
// sfxr synthesizer as the firmware (ports/nrf/drivers/sfxr/sfxr.c) so the
// browser preview sounds identical to the board's I2S output.
//
// The firmware streams sfxr voices continuously through an I2S DMA IRQ. In the
// browser there is no DMA: instead audio.play() renders the whole (finite)
// sound to a mono float buffer up front and hands it to JS (mp_js_audio_play),
// which plays it through the Web Audio API. Overlapping sounds are mixed by
// Web Audio itself, so no C-side voice pool is needed here.

#include "py/runtime.h"

#if MICROPY_PY_AUDIO

#include <string.h>
#include "py/obj.h"
#include "sfxr.h"
#include "sfxr_mp.h"
#include "library.h"

// sfxr synthesizes at this rate; JS creates the AudioBuffer at the same rate.
#define AUDIO_SAMPLE_RATE   (SFXR_SAMPLE_RATE)

// Cap render length so a runaway sound (e.g. infinite repeat) can't allocate
// unbounded memory. 5 s at 22050 Hz is ample for any sfxr effect.
#define AUDIO_MAX_SECONDS   (5)
#define AUDIO_MAX_SAMPLES   (AUDIO_SAMPLE_RATE * AUDIO_MAX_SECONDS)

// Render is done in chunks to keep the working buffer small; sfxr_generate can
// fill any block size.
#define AUDIO_CHUNK         (256)

// audio.play(params) -> True. Renders the whole sound and hands it to JS.
static mp_obj_t audio_play(mp_obj_t params_in) {
    if (!mp_obj_is_type(params_in, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("params must be a dict"));
    }

    sfxr_params p;
    sfxr_params_from_mp_dict(params_in, &p);

    sfxr_state state;
    sfxr_reset(&state, &p);

    // Render into a mono float buffer, growing as needed up to the cap.
    float *buf = m_new(float, AUDIO_MAX_SAMPLES);
    int total = 0;
    float chunk[AUDIO_CHUNK];
    while (state.playing && total < AUDIO_MAX_SAMPLES) {
        int want = AUDIO_MAX_SAMPLES - total;
        if (want > AUDIO_CHUNK) {
            want = AUDIO_CHUNK;
        }
        sfxr_generate(&state, chunk, want);
        memcpy(buf + total, chunk, want * sizeof(float));
        total += want;
    }

    // Hand the buffer to JS (Web Audio). JS copies out of the heap synchronously.
    mp_js_audio_play(buf, total, AUDIO_SAMPLE_RATE);

    m_del(float, buf, AUDIO_MAX_SAMPLES);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_play_obj, audio_play);

// audio.stop() / audio.deinit() -> silence everything currently playing in JS.
static mp_obj_t audio_stop(void) {
    mp_js_audio_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audio_stop_obj, audio_stop);

static const mp_rom_map_elem_t audio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audio) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audio_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audio_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audio_stop_obj) },
};
static MP_DEFINE_CONST_DICT(audio_module_globals, audio_module_globals_table);

const mp_obj_module_t audio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audio, audio_module);

#endif // MICROPY_PY_AUDIO

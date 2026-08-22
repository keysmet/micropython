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

// WebAssembly audio pump for the `synth` module. Unlike sfxr (finite sounds
// rendered up front), ksynth streams: the tempo clock and note queues run
// inside ksyn_render(), so the browser must pull blocks continuously.
//
// On first use, ksynth_port_start() tells JS to open an audio stream
// (globalThis._ksmSynthStart via library.js). JS then calls the exported
// ksm_synth_render(n) from its audio callback; the function renders n mono
// samples into a static float buffer and returns its heap address. Rendering
// never touches the MicroPython VM, so it is safe to call at any time,
// including while VM execution is suspended on the JS event loop (JSPI).

#include "py/mpconfig.h"

#if MICROPY_PY_SYNTH

#include <emscripten.h>
#include <stdint.h>
#include "ksynth.h"
#include "modsynth.h"
#include "library.h"

void ksynth_port_start(int sample_rate) {
    mp_js_synth_start(sample_rate);
}

#define SYNTH_MAX_BLOCK (2048)

static int16_t synth_buf_i16[SYNTH_MAX_BLOCK];
static float synth_buf_f32[SYNTH_MAX_BLOCK];

EMSCRIPTEN_KEEPALIVE float *ksm_synth_render(int n) {
    if (n < 0) {
        n = 0;
    }
    if (n > SYNTH_MAX_BLOCK) {
        n = SYNTH_MAX_BLOCK;
    }
    ksyn_render(synth_buf_i16, n);
    for (int i = 0; i < n; i++) {
        synth_buf_f32[i] = (float)synth_buf_i16[i] * (1.0f / 32768.0f);
    }
    return synth_buf_f32;
}

#endif // MICROPY_PY_SYNTH

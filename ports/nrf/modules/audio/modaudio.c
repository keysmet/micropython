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

// High-level SFXR audio module for the KSM1 board.
//
// This module owns the nRF52840 I2S peripheral directly (it does not go
// through machine.I2S). Python only sees the high-level API:
//
//     import audio
//     audio.play({ "wave_type": 0, "p_base_freq": 0.4, ... })
//
// A pool of sfxr voices is mixed together in the I2S DMA IRQ handler and
// streamed to the DAC. The IRQ glue is intentionally private to this module
// and is not exposed to Python.

#include "py/runtime.h"

#if MICROPY_PY_AUDIO

#include <string.h>
#include "py/obj.h"
#include "py/mphal.h"
#include "sfxr.h"
#include "sfxr_mp.h"
#include "nrfx_i2s.h"

// Fixed output format. sfxr synthesizes at 22050 Hz; the I2S rate table in the
// machine.I2S port notes select ~22727 Hz for a request of 22050, which is
// close enough (the pitch error is inaudible, as documented in sfxr.h).
#define AUDIO_SAMPLE_RATE   (22050)

// KSM1 I2S pins (see boards/KSM1/pins.csv):
//   BCK  = P0.05, LRCK = P0.01, DIN = P0.26. No MCK pin. TX only.
#define AUDIO_PIN_BCK       (5)
#define AUDIO_PIN_LRCK      (1)
#define AUDIO_PIN_DIN       (26)

// Number of simultaneous sfxr voices mixed together.
#define AUDIO_MAX_VOICES    (4)

// Number of 32-bit words in each DMA half-buffer.
// In 16-bit LEFT-aligned stereo the peripheral consumes one packed
// [L_lo,L_hi,R_lo,R_hi] frame per 32-bit word (this matches the TX path in
// machine_i2s.c, which copies packed 4-byte frames straight into the DMA
// buffer). So one word == one stereo frame == 4 bytes.
// 128 words = 512 bytes = 128 stereo frames = 256 int16 samples per buffer.
#define AUDIO_DMA_WORDS     (128)
#define AUDIO_DMA_FRAMES    (AUDIO_DMA_WORDS)

typedef struct _audio_voice_t {
    sfxr_state state;
    volatile bool active;
    uint32_t seq;  // play() order, used to steal the oldest voice when full
} audio_voice_t;

typedef struct _audio_obj_t {
    audio_voice_t voices[AUDIO_MAX_VOICES];

    // Double-buffered DMA storage (must be in RAM for EasyDMA).
    uint32_t dma_buf[2][AUDIO_DMA_WORDS];

    // Per-voice scratch used inside the IRQ to render then mix.
    int16_t voice_scratch[AUDIO_DMA_FRAMES * 2];

    bool initialized;
} audio_obj_t;

static audio_obj_t audio_obj;

// Monotonic counter assigned to each voice on play(), so the oldest active
// voice can be identified when the pool is full and needs to be stolen.
static uint32_t audio_seq;

// ---------------------------------------------------------------------------
// IRQ mixing: render each active voice and sum into the DMA buffer.
// ---------------------------------------------------------------------------

static void audio_fill_buffer(audio_obj_t *self, uint32_t *dst_words) {
    int16_t *dst = (int16_t *)dst_words;  // AUDIO_DMA_FRAMES stereo frames

    memset(dst, 0, AUDIO_DMA_FRAMES * 2 * sizeof(int16_t));

    for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
        audio_voice_t *voice = &self->voices[v];
        if (!voice->active) {
            continue;
        }

        sfxr_generate_s16_stereo(&voice->state, self->voice_scratch, AUDIO_DMA_FRAMES);

        // Accumulate this voice into the mix, saturating to int16 range.
        for (int i = 0; i < AUDIO_DMA_FRAMES * 2; i++) {
            int32_t acc = (int32_t)dst[i] + (int32_t)self->voice_scratch[i];
            if (acc > 32767) {
                acc = 32767;
            } else if (acc < -32767) {
                acc = -32767;
            }
            dst[i] = (int16_t)acc;
        }

        if (!sfxr_playing(&voice->state)) {
            voice->active = false;
        }
    }
}

static void audio_data_handler(nrfx_i2s_buffers_t const *p_released, uint32_t status) {
    audio_obj_t *self = &audio_obj;

    if (status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) {
        uint32_t *next_buf;
        if (p_released != NULL && p_released->p_tx_buffer != NULL) {
            next_buf = (uint32_t *)p_released->p_tx_buffer;
        } else {
            // First NEXT_BUFFERS_NEEDED: supply the second buffer.
            next_buf = self->dma_buf[1];
        }
        audio_fill_buffer(self, next_buf);
        nrfx_i2s_buffers_t next = { .p_rx_buffer = NULL, .p_tx_buffer = next_buf };
        nrfx_i2s_next_buffers_set(&next);
    }
}

// ---------------------------------------------------------------------------
// Peripheral lifecycle. The I2S peripheral is started lazily on the first
// play() and left running (streaming silence when no voice is active).
// ---------------------------------------------------------------------------

static void audio_hw_init(audio_obj_t *self) {
    if (self->initialized) {
        return;
    }

    // 32MHz/11 = 2.909 MHz, /128 = 22727 Hz (~22050), matches machine.I2S.
    nrfx_i2s_config_t cfg = {
        .sck_pin      = AUDIO_PIN_BCK,
        .lrck_pin     = AUDIO_PIN_LRCK,
        .mck_pin      = NRFX_I2S_PIN_NOT_USED,
        .sdout_pin    = AUDIO_PIN_DIN,
        .sdin_pin     = NRFX_I2S_PIN_NOT_USED,
        .irq_priority = NRFX_I2S_DEFAULT_CONFIG_IRQ_PRIORITY,
        .mode         = NRF_I2S_MODE_MASTER,
        .format       = NRF_I2S_FORMAT_I2S,
        .alignment    = NRF_I2S_ALIGN_LEFT,
        .sample_width = NRF_I2S_SWIDTH_16BIT,
        .channels     = NRF_I2S_CHANNELS_STEREO,
        .mck_setup    = NRF_I2S_MCK_32MDIV11,
        .ratio        = NRF_I2S_RATIO_128X,
    };

    nrfx_err_t err = nrfx_i2s_init(&cfg, audio_data_handler);
    if (err != NRFX_SUCCESS) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("nrfx_i2s_init failed (%d)"), (int)err);
    }

    memset(self->dma_buf, 0, sizeof(self->dma_buf));
    nrfx_i2s_buffers_t initial = { .p_rx_buffer = NULL, .p_tx_buffer = self->dma_buf[0] };
    err = nrfx_i2s_start(&initial, AUDIO_DMA_WORDS, 0);
    if (err != NRFX_SUCCESS) {
        nrfx_i2s_uninit();
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("nrfx_i2s_start failed (%d)"), (int)err);
    }

    self->initialized = true;
}

// ---------------------------------------------------------------------------
// Python API.
// ---------------------------------------------------------------------------

// audio.play(params) -> always True. Picks a free voice if one exists,
// otherwise steals the oldest active voice so newer sounds replace old ones.
static mp_obj_t audio_play(mp_obj_t params_in) {
    if (!mp_obj_is_type(params_in, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("params must be a dict"));
    }

    audio_obj_t *self = &audio_obj;
    audio_hw_init(self);

    sfxr_params p;
    sfxr_params_from_mp_dict(params_in, &p);

    // Choose a target slot: prefer a free one, else the oldest active voice.
    // Reading `active`/`seq` is racy against the IRQ, but the IRQ only ever
    // clears `active` (never arms a voice), so at worst we steal a voice that
    // just finished on its own — which is harmless.
    audio_voice_t *target = NULL;
    for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
        audio_voice_t *voice = &self->voices[v];
        if (!voice->active) {
            target = voice;
            break;
        }
        // Track the oldest voice as a steal candidate (smallest seq).
        if (target == NULL || voice->seq < target->seq) {
            target = voice;
        }
    }

    // Arm the target. Clear `active` first so the IRQ can't mix a voice whose
    // sfxr_state is only half-initialized while we overwrite it.
    target->active = false;
    sfxr_reset(&target->state, &p);
    target->seq = ++audio_seq;
    target->active = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_play_obj, audio_play);

// audio.stop() -> silence all voices.
static mp_obj_t audio_stop(void) {
    audio_obj_t *self = &audio_obj;
    for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
        self->voices[v].active = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audio_stop_obj, audio_stop);

// audio.deinit() -> stop and release the I2S peripheral.
static mp_obj_t audio_deinit(void) {
    audio_obj_t *self = &audio_obj;
    for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
        self->voices[v].active = false;
    }
    if (self->initialized) {
        nrfx_i2s_uninit();
        self->initialized = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audio_deinit_obj, audio_deinit);

static const mp_rom_map_elem_t audio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audio) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audio_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audio_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audio_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(audio_module_globals, audio_module_globals_table);

const mp_obj_module_t audio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audio, audio_module);

#endif // MICROPY_PY_AUDIO

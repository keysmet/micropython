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

// nRF52840 audio pump for the `synth` module (ksynth). Same I2S handling as
// modules/audio/modaudio.c: the peripheral is claimed lazily on first use and
// left running, with ksyn_render_stereo() filling each DMA half-buffer from
// the IRQ. All other synth calls run in the main loop; the engine is written
// for exactly this split (see drivers/ksynth/ksynth.h).
//
// The `audio` (sfxr) module and this one both drive nrfx_i2s directly, so a
// script can use one or the other, not both: whichever plays first owns the
// peripheral (the loser's init raises OSError; audio.deinit() releases it).
//
// Rendering cost is ~1-2% of the 64 MHz core per active voice at 22727 Hz,
// so even all 8 voices stay far from the ~5.6 ms DMA deadline that capped
// sfxr at 2 voices.

#include "py/runtime.h"

#if MICROPY_PY_SYNTH

#include <string.h>
#include "py/mphal.h"
#include "ksynth.h"
#include "modsynth.h"
#include "nrfx_i2s.h"

// True output rate: MCK 32MHz/11 = 2.909 MHz, /128 = 22727 Hz. Passing the
// real rate to ksyn_init() keeps note pitch and tempo exact (the sfxr module
// tolerated the 22050-vs-22727 mismatch; music makes it worth fixing).
#define SYNTH_TRUE_RATE     (22727)

// KSM1 I2S pins (boards/KSM1/pins.csv): BCK=P0.05, LRCK=P0.01, DIN=P0.26.
#define SYNTH_PIN_BCK       (5)
#define SYNTH_PIN_LRCK      (1)
#define SYNTH_PIN_DIN       (26)

// One 32-bit word per packed stereo frame; 128 frames = ~5.6 ms per buffer.
#define SYNTH_DMA_WORDS     (128)
#define SYNTH_DMA_FRAMES    (SYNTH_DMA_WORDS)

static uint32_t synth_dma_buf[2][SYNTH_DMA_WORDS];
static bool synth_hw_ready;

static void synth_data_handler(nrfx_i2s_buffers_t const *p_released, uint32_t status) {
    if (status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) {
        uint32_t *next_buf;
        if (p_released != NULL && p_released->p_tx_buffer != NULL) {
            next_buf = (uint32_t *)p_released->p_tx_buffer;
        } else {
            // First NEXT_BUFFERS_NEEDED: supply the second buffer.
            next_buf = synth_dma_buf[1];
        }
        ksyn_render_stereo((int16_t *)next_buf, SYNTH_DMA_FRAMES);
        nrfx_i2s_buffers_t next = { .p_rx_buffer = NULL, .p_tx_buffer = next_buf };
        nrfx_i2s_next_buffers_set(&next);
    }
}

void ksynth_port_start(void) {
    if (synth_hw_ready) {
        return;
    }

    ksyn_init(SYNTH_TRUE_RATE);

    nrfx_i2s_config_t cfg = {
        .sck_pin      = SYNTH_PIN_BCK,
        .lrck_pin     = SYNTH_PIN_LRCK,
        .mck_pin      = NRFX_I2S_PIN_NOT_USED,
        .sdout_pin    = SYNTH_PIN_DIN,
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

    nrfx_err_t err = nrfx_i2s_init(&cfg, synth_data_handler);
    if (err != NRFX_SUCCESS) {
        // Usually: the sfxr `audio` module already owns I2S (audio.deinit()
        // releases it).
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("I2S unavailable for synth (%d)"), (int)err);
    }

    memset(synth_dma_buf, 0, sizeof(synth_dma_buf));
    nrfx_i2s_buffers_t initial = { .p_rx_buffer = NULL, .p_tx_buffer = synth_dma_buf[0] };
    err = nrfx_i2s_start(&initial, SYNTH_DMA_WORDS, 0);
    if (err != NRFX_SUCCESS) {
        nrfx_i2s_uninit();
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("nrfx_i2s_start failed (%d)"), (int)err);
    }

    synth_hw_ready = true;
}

#endif // MICROPY_PY_SYNTH

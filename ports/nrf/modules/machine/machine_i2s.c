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

// This file is never compiled standalone, it's included directly from
// extmod/machine_i2s.c via MICROPY_PY_MACHINE_I2S_INCLUDEFILE.

#include <stdlib.h>
#include <string.h>
#include "py/mperrno.h"
#include "py/mphal.h"
#include "pin.h"
#include "genhdr/pins.h"

#include "nrfx_i2s.h"

// nRF52840 has a single I2S peripheral.
#define MACHINE_I2S_NUM_INSTANCES (1)

// Number of 32-bit words in each DMA half-buffer.
// The nrfx driver uses a double-buffer scheme: while one buffer is being
// played by the hardware, we fill the other.
// 128 words = 512 bytes = 256 stereo 16-bit samples per buffer.
#define I2S_DMA_WORDS (128)

// For non-blocking mode: how many bytes to copy per IRQ call from the app
// buffer into the ring buffer.  Must be defined here; extmod uses it.
#define SIZEOF_NON_BLOCKING_COPY_IN_BYTES (I2S_DMA_WORDS * 4)

// i2s_frame_map and get_frame_mapping_index are required by extmod/machine_i2s.c
// when MICROPY_PY_MACHINE_I2S_RING_BUF=1.
//
// The nRF I2S peripheral in LEFT_ALIGNED / 16-bit mode places samples in the
// upper 16 bits of each 32-bit word (big-endian within the word):
//   word[n] = { L_hi, L_lo, R_hi, R_lo }  for stereo
//   word[n] = { L_hi, L_lo, 0,    0    }  for mono (LEFT channel only)
//
// Frame map: maps byte positions in the 8-byte DMA frame to app buffer positions.
// -1 means "discard this byte" (padding).
// Indices into the 8-byte DMA frame:  [0..3] = left word, [4..7] = right word.
// Within each 32-bit word, 16-bit samples are in the high half (bytes 2,3 of the word).
// Note: the nRF52840 I2S peripheral supports 8, 16, and 24-bit sample widths
// only — there is no 32-bit mode.  The frame map rows for "32-bit" in the
// extmod enum are repurposed here for 24-bit.
static const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    // Mono,   16-bit: pick bytes 2,3 of left word; discard rest
    { -1, -1,  0,  1, -1, -1, -1, -1 },
    // Mono,   24-bit: pick bytes 1,2,3 of left word; discard right word
    { -1,  0,  1,  2, -1, -1, -1, -1 },
    // Stereo, 16-bit: pick bytes 2,3 of left, bytes 6,7 of right
    { -1, -1,  0,  1, -1, -1,  2,  3 },
    // Stereo, 24-bit: bytes 1-3 of left, bytes 5-7 of right
    { -1,  0,  1,  2, -1,  3,  4,  5 },
};

static int8_t get_frame_mapping_index(int8_t bits, format_t format) {
    // Row 0: mono 16-bit, row 1: mono 24-bit, row 2: stereo 16-bit, row 3: stereo 24-bit
    if (format == MONO) {
        return (bits == 16) ? 0 : 1;
    } else {
        return (bits == 16) ? 2 : 3;
    }
}

// nRF52840 MCK / RATIO combinations for common sample rates.
// LRCK = MCK / RATIO.  MCK = 32MHz / divisor.
typedef struct {
    uint32_t        rate_hz;
    nrf_i2s_mck_t   mck_setup;
    nrf_i2s_ratio_t ratio;
} i2s_rate_config_t;

static const i2s_rate_config_t i2s_rate_table[] = {
    // 32MHz/11 = 2.909 MHz, /128 = 22727 Hz  (~22050)
    { 22050, NRF_I2S_MCK_32MDIV11, NRF_I2S_RATIO_128X },
    // 32MHz/8  = 4.000 MHz, /128 = 31250 Hz  (~32000)
    { 32000, NRF_I2S_MCK_32MDIV8,  NRF_I2S_RATIO_128X },
    // 32MHz/21 = 1.524 MHz, /32  = 47619 Hz  (~48000)
    { 48000, NRF_I2S_MCK_32MDIV21, NRF_I2S_RATIO_32X  },
    // 32MHz/15 = 2.133 MHz, /128 = 16667 Hz  (~16000)
    { 16000, NRF_I2S_MCK_32MDIV15, NRF_I2S_RATIO_128X },
    // 32MHz/63 = 507.9 kHz, /64  =  7937 Hz  (~8000)
    {  8000, NRF_I2S_MCK_32MDIV63, NRF_I2S_RATIO_64X  },
    // 32MHz/11 = 2.909 MHz, /256 = 11364 Hz  (~11025)
    { 11025, NRF_I2S_MCK_32MDIV11, NRF_I2S_RATIO_256X },
    // 32MHz/21 = 1.524 MHz, /128 = 11905 Hz  (~12000)
    { 12000, NRF_I2S_MCK_32MDIV21, NRF_I2S_RATIO_128X },
    // 32MHz/42 = 761.9 kHz, /32  = 23810 Hz  (~24000)
    { 24000, NRF_I2S_MCK_32MDIV42, NRF_I2S_RATIO_32X  },
};

// Per-instance state — must match field names used by extmod/machine_i2s.c.
typedef struct _machine_i2s_obj_t {
    mp_obj_base_t base;
    uint8_t i2s_id;
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t ws;
    mp_hal_pin_obj_t sd;
    int8_t mode;
    int8_t bits;
    format_t format;
    int32_t rate;
    int32_t ibuf;
    mp_obj_t callback_for_non_blocking;
    io_mode_t io_mode;
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;
    non_blocking_descriptor_t non_blocking_descriptor;

    // Double-buffered DMA storage (must be in RAM for EasyDMA).
    // We keep both buffers here and let nrfx tell us which one it just
    // finished with via the p_released pointer in the data handler.
    uint32_t dma_buf[2][I2S_DMA_WORDS];
    bool initialized;
} machine_i2s_obj_t;

static machine_i2s_obj_t machine_i2s_obj[MACHINE_I2S_NUM_INSTANCES];

// ---------------------------------------------------------------------------
// nrfx data handler — called from IRQ when a buffer is released and the next
// one is needed.
// ---------------------------------------------------------------------------

static void i2s_data_handler(nrfx_i2s_buffers_t const *p_released, uint32_t status) {
    machine_i2s_obj_t *self = &machine_i2s_obj[0];

    // Drain incoming RX data from the buffer that was just released.
    if (p_released != NULL && p_released->p_rx_buffer != NULL) {
        const uint8_t *src = (const uint8_t *)p_released->p_rx_buffer;
        for (uint32_t i = 0; i < I2S_DMA_WORDS * 4; i++) {
            ringbuf_push(&self->ring_buffer, src[i]);
        }
        if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
            fill_appbuf_from_ringbuf_non_blocking(self);
        }
    }

    if (status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) {
        // Pick which of our two DMA buffers to hand to nrfx next.
        // If nrfx just released a buffer, reuse that exact pointer so we
        // always know which slot is which.  On the very first call
        // p_released has both pointers NULL (hardware hasn't transferred
        // anything yet), so we fall back to the second slot.
        uint32_t *next_buf;
        if (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) {
            if (p_released != NULL && p_released->p_tx_buffer != NULL) {
                next_buf = (uint32_t *)p_released->p_tx_buffer;
            } else {
                // First NEXT_BUFFERS_NEEDED: supply the second buffer.
                next_buf = self->dma_buf[1];
            }
            // Fill with data from the ring buffer (silence on underrun).
            uint8_t *dst = (uint8_t *)next_buf;
            for (uint32_t i = 0; i < I2S_DMA_WORDS * 4; i++) {
                if (!ringbuf_pop(&self->ring_buffer, dst + i)) {
                    dst[i] = 0;
                }
            }
            if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
                copy_appbuf_to_ringbuf_non_blocking(self);
            }
            nrfx_i2s_buffers_t next = { .p_rx_buffer = NULL, .p_tx_buffer = next_buf };
            nrfx_i2s_next_buffers_set(&next);
        } else {
            // RX: supply whichever buffer we're not currently draining.
            if (p_released != NULL && p_released->p_rx_buffer != NULL) {
                next_buf = (uint32_t *)p_released->p_rx_buffer;
            } else {
                next_buf = self->dma_buf[1];
            }
            nrfx_i2s_buffers_t next = { .p_rx_buffer = next_buf, .p_tx_buffer = NULL };
            nrfx_i2s_next_buffers_set(&next);
        }
    }
}

// ---------------------------------------------------------------------------
// Interface required by extmod/machine_i2s.c
// ---------------------------------------------------------------------------

static machine_i2s_obj_t *mp_machine_i2s_make_new_instance(mp_int_t i2s_id) {
    if (i2s_id != 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2S(%d) does not exist"), i2s_id);
    }
    machine_i2s_obj_t *self = &machine_i2s_obj[0];
    self->base.type = &machine_i2s_type;
    self->i2s_id = 0;
    return self;
}

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    // Uninit first if we're being re-initialized (e.g. second call from REPL).
    if (self->initialized) {
        nrfx_i2s_uninit();
        self->initialized = false;
        if (self->ring_buffer_storage != NULL) {
            m_free(self->ring_buffer_storage);
            self->ring_buffer_storage = NULL;
        }
    }

    self->sck    = mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    self->ws     = mp_hal_get_pin_obj(args[ARG_ws].u_obj);
    self->sd     = mp_hal_get_pin_obj(args[ARG_sd].u_obj);
    self->mode   = args[ARG_mode].u_int;
    self->bits   = args[ARG_bits].u_int;
    self->format = args[ARG_format].u_int;
    self->rate   = args[ARG_rate].u_int;
    self->ibuf   = args[ARG_ibuf].u_int;

    if (self->mode != MICROPY_PY_MACHINE_I2S_CONSTANT_TX &&
        self->mode != MICROPY_PY_MACHINE_I2S_CONSTANT_RX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }
    if (self->bits != 16 && self->bits != 24) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 16 or 24"));
    }
    if (self->ibuf <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("ibuf must be > 0"));
    }

    // Allocate ring buffer.
    self->ring_buffer_storage = m_new(uint8_t, self->ibuf);
    ringbuf_init(&self->ring_buffer, self->ring_buffer_storage, self->ibuf);
    self->io_mode = BLOCKING;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->non_blocking_descriptor.copy_in_progress = false;

    // Find MCK/RATIO for the requested sample rate (closest within 5%).
    nrf_i2s_mck_t   mck_setup = NRF_I2S_MCK_32MDIV11;
    nrf_i2s_ratio_t ratio     = NRF_I2S_RATIO_128X;
    uint32_t best_err = UINT32_MAX;
    for (size_t i = 0; i < MP_ARRAY_SIZE(i2s_rate_table); i++) {
        uint32_t t = i2s_rate_table[i].rate_hz;
        uint32_t r = (uint32_t)self->rate;
        uint32_t err = (t > r) ? (t - r) : (r - t);
        if (err < best_err) {
            best_err = err;
            mck_setup = i2s_rate_table[i].mck_setup;
            ratio     = i2s_rate_table[i].ratio;
        }
    }
    if (best_err * 20 > (uint32_t)self->rate) {
        mp_raise_ValueError(MP_ERROR_TEXT("no supported sample rate"));
    }

    // Configure nrfx — matches Arduino keysmet.cpp exactly for TX.
    nrfx_i2s_config_t cfg = {
        .sck_pin      = self->sck->pin,
        .lrck_pin     = self->ws->pin,
        .mck_pin      = NRFX_I2S_PIN_NOT_USED,
        .sdout_pin    = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX)
                            ? self->sd->pin : NRFX_I2S_PIN_NOT_USED,
        .sdin_pin     = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_RX)
                            ? self->sd->pin : NRFX_I2S_PIN_NOT_USED,
        .irq_priority = NRFX_I2S_DEFAULT_CONFIG_IRQ_PRIORITY,
        .mode         = NRF_I2S_MODE_MASTER,
        .format       = NRF_I2S_FORMAT_I2S,
        .alignment    = NRF_I2S_ALIGN_LEFT,
        .sample_width = (self->bits == 16) ? NRF_I2S_SWIDTH_16BIT : NRF_I2S_SWIDTH_24BIT,
        .channels     = (self->format == STEREO)
                            ? NRF_I2S_CHANNELS_STEREO : NRF_I2S_CHANNELS_LEFT,
        .mck_setup    = mck_setup,
        .ratio        = ratio,
    };

    nrfx_err_t err = nrfx_i2s_init(&cfg, i2s_data_handler);
    if (err != NRFX_SUCCESS) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("nrfx_i2s_init failed (%d)"), (int)err);
    }
    self->initialized = true;

    // Start with the first DMA buffer.  nrfx will immediately fire the
    // handler asking for the second one.
    memset(self->dma_buf, 0, sizeof(self->dma_buf));
    nrfx_i2s_buffers_t initial = { NULL, NULL };
    if (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) {
        initial.p_tx_buffer = self->dma_buf[0];
    } else {
        initial.p_rx_buffer = self->dma_buf[0];
    }
    err = nrfx_i2s_start(&initial, I2S_DMA_WORDS, 0);
    if (err != NRFX_SUCCESS) {
        nrfx_i2s_uninit();
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("nrfx_i2s_start failed (%d)"), (int)err);
    }
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    if (self->initialized) {
        // nrfx_i2s_uninit() internally calls nrfx_i2s_stop().
        nrfx_i2s_uninit();
        self->initialized = false;
    }
    if (self->ring_buffer_storage != NULL) {
        m_free(self->ring_buffer_storage);
        self->ring_buffer_storage = NULL;
    }
}

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    // The nrfx IRQ is permanently enabled once nrfx_i2s_start() is called.
    // Non-blocking callbacks are driven from i2s_data_handler via the
    // copy_appbuf_to_ringbuf_non_blocking / fill_appbuf_from_ringbuf_non_blocking
    // helpers defined in extmod/machine_i2s.c.
    (void)self;
}

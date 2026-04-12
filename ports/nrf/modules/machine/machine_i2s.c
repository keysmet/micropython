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

#include <string.h>
#include "py/mperrno.h"
#include "py/mphal.h"
#include "pin.h"
#include "genhdr/pins.h"

#include "nrfx_i2s.h"

// nRF52840 has a single I2S peripheral.
#define MACHINE_I2S_NUM_INSTANCES (1)

// Number of 32-bit words in each DMA half-buffer.
// Each call to the nrfx data handler covers one half.
// 128 words = 512 bytes = 256 stereo 16-bit samples.
#define I2S_DMA_WORDS (128)

// Mode constants exposed as I2S.RX / I2S.TX
#define MACHINE_I2S_RX (0)
#define MACHINE_I2S_TX (1)

// nRF52840 MCK / RATIO combinations that produce standard sample rates.
// For each entry the actual LRCK = MCK_HZ / (ratio_mult * 32) for mono,
// or MCK_HZ / (ratio_mult * 64) for stereo.  We pick MCK and RATIO to get
// the closest match to the requested rate.
typedef struct {
    uint32_t            rate_hz;
    nrf_i2s_mck_t       mck_setup;
    nrf_i2s_ratio_t     ratio;
} i2s_rate_config_t;

static const i2s_rate_config_t i2s_rate_table[] = {
    // MCK = 32MHz/11 ≈ 2.909 MHz, RATIO=128x → LRCK ≈ 22727 Hz ≈ 22050 Hz
    { 22050, NRF_I2S_MCK_32MDIV11, NRF_I2S_RATIO_128X },
    // MCK = 32MHz/8 = 4 MHz, RATIO=128x → LRCK = 31250 Hz ≈ 32000 Hz
    { 32000, NRF_I2S_MCK_32MDIV8,  NRF_I2S_RATIO_128X },
    // MCK = 32MHz/21 ≈ 1.524 MHz, RATIO=32x → LRCK ≈ 47619 Hz ≈ 48000 Hz
    { 48000, NRF_I2S_MCK_32MDIV21, NRF_I2S_RATIO_32X  },
    // MCK = 32MHz/42 ≈ 762 kHz, RATIO=32x → LRCK ≈ 23810 Hz ≈ 24000 Hz (fallback)
    { 24000, NRF_I2S_MCK_32MDIV42, NRF_I2S_RATIO_32X  },
    // MCK = 32MHz/15 ≈ 2.133 MHz, RATIO=64x → LRCK ≈ 33333 Hz (fallback)
    { 16000, NRF_I2S_MCK_32MDIV15, NRF_I2S_RATIO_128X },
    // MCK = 32MHz/63 ≈ 508 kHz, RATIO=64x → LRCK ≈ 7937 Hz ≈ 8000 Hz
    {  8000, NRF_I2S_MCK_32MDIV63, NRF_I2S_RATIO_64X  },
    // MCK = 32MHz/11 ≈ 2.909 MHz, RATIO=256x → LRCK ≈ 11364 Hz ≈ 11025 Hz
    { 11025, NRF_I2S_MCK_32MDIV11, NRF_I2S_RATIO_256X },
    // MCK = 32MHz/21 ≈ 1.524 MHz, RATIO=128x → LRCK ≈ 11905 Hz ≈ 12000 Hz
    { 12000, NRF_I2S_MCK_32MDIV21, NRF_I2S_RATIO_128X },
    // MCK = 32MHz/42 ≈ 762 kHz, RATIO=128x → LRCK ≈ 5952 Hz ≈ 6000 Hz
    {  6000, NRF_I2S_MCK_32MDIV42, NRF_I2S_RATIO_128X },
    // MCK = 32MHz/63 ≈ 508 kHz, RATIO=128x → LRCK ≈ 3968 Hz ≈ 4000 Hz
    {  4000, NRF_I2S_MCK_32MDIV63, NRF_I2S_RATIO_128X },
};

// Per-instance state
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

    // Ring buffer (managed by extmod/machine_i2s.c)
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;

    // Non-blocking descriptor
    non_blocking_descriptor_t non_blocking_descriptor;

    // Double-buffered DMA buffers (must be in RAM for EasyDMA)
    uint32_t dma_buf[2][I2S_DMA_WORDS];
    uint8_t  active_buf; // which half the hardware is consuming now
} machine_i2s_obj_t;

// Only one instance on nRF52840
static machine_i2s_obj_t machine_i2s_obj[MACHINE_I2S_NUM_INSTANCES];

// Forward declaration
static void i2s_data_handler(nrfx_i2s_buffers_t const *p_released, uint32_t status);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool i2s_find_rate(int32_t rate_hz, nrf_i2s_mck_t *mck_out, nrf_i2s_ratio_t *ratio_out) {
    // Find the closest entry in the rate table (within 5 %).
    uint32_t best_err = UINT32_MAX;
    int best_idx = -1;
    for (size_t i = 0; i < MP_ARRAY_SIZE(i2s_rate_table); i++) {
        uint32_t t = i2s_rate_table[i].rate_hz;
        uint32_t r = (uint32_t)rate_hz;
        uint32_t err = (t > r) ? (t - r) : (r - t);
        if (err < best_err) {
            best_err = err;
            best_idx = (int)i;
        }
    }
    // Accept if within 5 % of the requested rate
    if (best_idx < 0 || best_err * 20 > (uint32_t)rate_hz) {
        return false;
    }
    *mck_out   = i2s_rate_table[best_idx].mck_setup;
    *ratio_out = i2s_rate_table[best_idx].ratio;
    return true;
}

// Feed silence / pull from ring buffer and hand to nrfx.
// Called from IRQ context.
static void i2s_fill_dma_buf(machine_i2s_obj_t *self, uint32_t *buf) {
    uint32_t n_bytes = I2S_DMA_WORDS * 4;
    if (self->mode == MACHINE_I2S_TX) {
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < n_bytes; i++) {
            if (!ringbuf_pop(&self->ring_buffer, dst + i)) {
                dst[i] = 0; // underrun: silence
            }
        }
        // Non-blocking: schedule more data from app buffer if needed
        if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
            copy_appbuf_to_ringbuf_non_blocking(self);
        }
    }
    // RX: data will be written into the buf by hardware; we drain after release.
}

static void i2s_drain_dma_buf(machine_i2s_obj_t *self, const uint32_t *buf) {
    if (self->mode == MACHINE_I2S_RX) {
        const uint8_t *src = (const uint8_t *)buf;
        uint32_t n_bytes = I2S_DMA_WORDS * 4;
        for (uint32_t i = 0; i < n_bytes; i++) {
            ringbuf_push(&self->ring_buffer, src[i]);
        }
        // Non-blocking: copy from ring buffer to app buffer if needed
        if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
            fill_appbuf_from_ringbuf_non_blocking(self);
        }
    }
}

// ---------------------------------------------------------------------------
// nrfx data handler (called from IRQ)
// ---------------------------------------------------------------------------

static void i2s_data_handler(nrfx_i2s_buffers_t const *p_released, uint32_t status) {
    machine_i2s_obj_t *self = &machine_i2s_obj[0];

    // p_released is NULL on the very first call (no buffer has been released yet).
    if (p_released != NULL) {
        // The buffer that was just released has been fully processed.
        if (self->mode == MACHINE_I2S_RX && p_released->p_rx_buffer != NULL) {
            i2s_drain_dma_buf(self, p_released->p_rx_buffer);
        }
        // For TX: the buffer that was just sent is now free; we'll refill it next.
    }

    if (status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) {
        // Determine which buffer index to use next: alternate between 0 and 1.
        uint8_t next = self->active_buf ^ 1;
        self->active_buf = next;

        nrfx_i2s_buffers_t next_bufs = { NULL, NULL };
        if (self->mode == MACHINE_I2S_TX) {
            i2s_fill_dma_buf(self, self->dma_buf[next]);
            next_bufs.p_tx_buffer = self->dma_buf[next];
        } else {
            next_bufs.p_rx_buffer = self->dma_buf[next];
        }
        nrfx_i2s_next_buffers_set(&next_bufs);
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
    // --- parse arguments ---
    self->sck    = mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    self->ws     = mp_hal_get_pin_obj(args[ARG_ws].u_obj);
    self->sd     = mp_hal_get_pin_obj(args[ARG_sd].u_obj);
    self->mode   = args[ARG_mode].u_int;
    self->bits   = args[ARG_bits].u_int;
    self->format = args[ARG_format].u_int;
    self->rate   = args[ARG_rate].u_int;
    self->ibuf   = args[ARG_ibuf].u_int;

    if (self->mode != MACHINE_I2S_TX && self->mode != MACHINE_I2S_RX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }
    if (self->bits != 16 && self->bits != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bits; must be 16 or 32"));
    }
    if (self->ibuf <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("ibuf must be > 0"));
    }

    // --- allocate ring buffer ---
    self->ring_buffer_storage = m_new(uint8_t, self->ibuf);
    ringbuf_init(&self->ring_buffer, self->ring_buffer_storage, self->ibuf);
    self->io_mode = BLOCKING;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->non_blocking_descriptor.copy_in_progress = false;

    // --- find MCK/RATIO ---
    nrf_i2s_mck_t   mck_setup;
    nrf_i2s_ratio_t ratio;
    if (!i2s_find_rate(self->rate, &mck_setup, &ratio)) {
        mp_raise_ValueError(MP_ERROR_TEXT("no supported sample rate found"));
    }

    // --- build nrfx config ---
    nrfx_i2s_config_t cfg = {
        .sck_pin      = self->sck->pin,
        .lrck_pin     = self->ws->pin,
        .mck_pin      = NRFX_I2S_PIN_NOT_USED,
        .sdout_pin    = (self->mode == MACHINE_I2S_TX) ? self->sd->pin : NRFX_I2S_PIN_NOT_USED,
        .sdin_pin     = (self->mode == MACHINE_I2S_RX) ? self->sd->pin : NRFX_I2S_PIN_NOT_USED,
        .irq_priority = NRFX_I2S_DEFAULT_CONFIG_IRQ_PRIORITY,
        .mode         = NRF_I2S_MODE_MASTER,
        .format       = NRF_I2S_FORMAT_I2S,
        .alignment    = NRF_I2S_ALIGN_LEFT,
        .sample_width = (self->bits == 16) ? NRF_I2S_SWIDTH_16BIT : NRF_I2S_SWIDTH_32BIT,
        .channels     = (self->format == STEREO) ? NRF_I2S_CHANNELS_STEREO : NRF_I2S_CHANNELS_LEFT,
        .mck_setup    = mck_setup,
        .ratio        = ratio,
    };

    nrfx_err_t err = nrfx_i2s_init(&cfg, i2s_data_handler);
    if (err != NRFX_SUCCESS) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("nrfx_i2s_init failed: %d"), err);
    }

    // --- pre-fill TX buffer / prepare RX buffer, then start ---
    self->active_buf = 0;
    memset(self->dma_buf, 0, sizeof(self->dma_buf));

    nrfx_i2s_buffers_t initial = { NULL, NULL };
    if (self->mode == MACHINE_I2S_TX) {
        initial.p_tx_buffer = self->dma_buf[0];
    } else {
        initial.p_rx_buffer = self->dma_buf[0];
    }
    err = nrfx_i2s_start(&initial, I2S_DMA_WORDS, 0);
    if (err != NRFX_SUCCESS) {
        nrfx_i2s_uninit();
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("nrfx_i2s_start failed: %d"), err);
    }
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    nrfx_i2s_stop();
    nrfx_i2s_uninit();
    if (self->ring_buffer_storage != NULL) {
        m_free(self->ring_buffer_storage);
        self->ring_buffer_storage = NULL;
    }
}

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    // Nothing to do: the nrfx IRQ is always active once started.
    // Non-blocking callbacks are scheduled from within i2s_data_handler
    // via mp_sched_schedule() (in fill/drain helpers from extmod).
    (void)self;
}

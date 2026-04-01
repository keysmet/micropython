/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Keysmet
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

// nRF52 PWM EasyDMA implementation of machine_bitstream_high_low.
// Uses hardware PWM to generate precise WS2812 timing without any
// CPU cycle counting — the DMA burst is fully hardware-timed.
//
// PWM instance selection:
//   nRF52840: NRF_PWM3 (52840-only peripheral, avoids conflict with machine.PWM)
//   nRF52832: NRF_PWM2 (conflicts with machine.PWM(device=2) if used simultaneously)
//
// Stack usage: (len * 8 + 40) * 2 bytes.  At 150 pixels: ~3.7 KB.
// The nRF52840 has an 8 KB ISR/task stack, so up to ~300 pixels is safe.
// Beyond that the caller risks a stack overflow.

#include "py/mpconfig.h"
#include "py/mphal.h"
#include "modules/machine/pin.h"
#include "nrf_gpio.h"
#include <nrfx.h>

#if MICROPY_PY_MACHINE_BITSTREAM

// 16 MHz clock, COUNTERTOP = 20 → 1.25 µs period (800 kHz)
#define PWM_COUNTERTOP  (20UL)
// Number of PWM reset-code entries appended to guarantee ≥ 50 µs low.
// 40 × 1.25 µs = 50 µs exactly.
#define PWM_RESET_ENTRIES (40U)

// Polarity flag: PWM starts LOW by default; OR with 0x8000 inverts so pin
// starts HIGH and falls at the compare point.
#define PWM_POLARITY_FLAG (0x8000U)

void machine_bitstream_high_low(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
    const uint8_t *buf, size_t len) {

    // Convert T0H and T1H from nanoseconds to 16 MHz ticks.
    // timing_ns[0] = T0H, timing_ns[1] = T0L, timing_ns[2] = T1H, timing_ns[3] = T1L
    // At 16 MHz: ticks = ns * 16 / 1000
    uint16_t t0h = (uint16_t)(timing_ns[0] * 16 / 1000) | PWM_POLARITY_FLAG;
    uint16_t t1h = (uint16_t)(timing_ns[2] * 16 / 1000) | PWM_POLARITY_FLAG;
    // Reset entry: duty = 0 with polarity flag → pin stays LOW the whole period.
    uint16_t rst = 0 | PWM_POLARITY_FLAG;

    // Allocate the pattern buffer on the stack.
    // Each byte expands to 8 PWM entries; reset entries follow.
    size_t n_entries = len * 8 + PWM_RESET_ENTRIES;
    uint16_t pattern[n_entries];

    // Fill bit pattern MSB-first.
    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = buf[i];
        for (int bit = 7; bit >= 0; --bit) {
            pattern[pos++] = (byte & (1 << bit)) ? t1h : t0h;
        }
    }
    // Append reset entries.
    for (size_t i = 0; i < PWM_RESET_ENTRIES; ++i) {
        pattern[pos++] = rst;
    }

    // Configure the GPIO pin as output, drive low before enabling PWM.
    nrf_gpio_cfg_output(pin->pin);
    nrf_gpio_pin_clear(pin->pin);

    // Select the PWM instance.
#if defined(NRF52840_XXAA) || defined(NRF52840)
    NRF_PWM_Type *pwm = NRF_PWM3;
#else
    NRF_PWM_Type *pwm = NRF_PWM2;
#endif

    // Configure PWM registers.
    pwm->MODE      = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);
    pwm->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos);
    pwm->COUNTERTOP = (PWM_COUNTERTOP << PWM_COUNTERTOP_COUNTERTOP_Pos);
    pwm->LOOP      = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);
    pwm->DECODER   = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos)
                   | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);

    // Connect only OUT[0]; disconnect other channels.
    pwm->PSEL.OUT[0] = (pin->pin << PWM_PSEL_OUT_PIN_Pos)
                     | (PWM_PSEL_OUT_CONNECT_Connected << PWM_PSEL_OUT_CONNECT_Pos);
    pwm->PSEL.OUT[1] = 0xFFFFFFFFUL;
    pwm->PSEL.OUT[2] = 0xFFFFFFFFUL;
    pwm->PSEL.OUT[3] = 0xFFFFFFFFUL;

    // Set up DMA sequence.
    pwm->SEQ[0].PTR      = (uint32_t)pattern;
    pwm->SEQ[0].CNT      = n_entries << PWM_SEQ_CNT_CNT_Pos;
    pwm->SEQ[0].REFRESH  = 0;
    pwm->SEQ[0].ENDDELAY = 0;

    // Enable PWM and start sequence.
    pwm->ENABLE = (PWM_ENABLE_ENABLE_Enabled << PWM_ENABLE_ENABLE_Pos);
    pwm->EVENTS_SEQEND[0] = 0;
    pwm->TASKS_SEQSTART[0] = 1;

    // Poll until the sequence is complete.
    while (!pwm->EVENTS_SEQEND[0]) {
    }
    pwm->EVENTS_SEQEND[0] = 0;

    // Stop and disable in the correct order to avoid pin glitches
    // (per nRF52840 PS §6.31.4: disconnect PSEL only after ENABLE=Disabled).
    pwm->TASKS_STOP = 1;
    pwm->ENABLE     = (PWM_ENABLE_ENABLE_Disabled << PWM_ENABLE_ENABLE_Pos);
    pwm->PSEL.OUT[0] = 0xFFFFFFFFUL;

    // Leave pin low (idle state for WS2812 data line).
    nrf_gpio_pin_clear(pin->pin);
}

#endif // MICROPY_PY_MACHINE_BITSTREAM

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

#define MICROPY_HW_BOARD_NAME       "KSM1"
#define MICROPY_HW_MCU_NAME         "NRF52840"
#define MICROPY_PY_SYS_PLATFORM     "nrf52840-KSM1"

#define MICROPY_PY_MACHINE_UART     (0)
#define MICROPY_PY_MACHINE_SPI      (0)
#define MICROPY_PY_MACHINE_HW_PWM   (1)
#define MICROPY_PY_MACHINE_RTCOUNTER (1)
#define MICROPY_PY_MACHINE_I2C      (1)
#define MICROPY_PY_MACHINE_ADC      (1)
#define MICROPY_PY_MACHINE_TEMP     (1)
#define MICROPY_PY_MACHINE_I2S      (1)

#define MICROPY_HW_ENABLE_RNG       (1)

// No 32.768kHz LF crystal on MDBT50Q module - use internal RC oscillator
#define MICROPY_HW_CLK_LFCLKSRC  ((CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos) & CLOCK_LFCLKSRC_SRC_Msk)

// USB CDC REPL via native nRF52840 USB
#define MICROPY_HW_ENABLE_USBDEV    (1)
#define MICROPY_HW_USB_CDC          (1)

// LED: use PWR_LED (P0.07) as the single built-in LED
// LED_PULLUP = 0 means active high (set pin HIGH to turn on)
#define MICROPY_HW_HAS_LED          (1)
#define MICROPY_HW_LED_COUNT        (1)
#define MICROPY_HW_LED_PULLUP       (0)
#define MICROPY_HW_LED1             (7)  // PIN_PWR_LED = P0.07

// I2C config (LSM6DS3 IMU)
// SDA = P0.04, SCL = P0.11
#define MICROPY_HW_I2C0_NAME        "I2C0"
#define MICROPY_HW_I2C0_SCL         (11) // PIN_I2C_SCL = P0.11
#define MICROPY_HW_I2C0_SDA         (4)  // PIN_I2C_SDA = P0.04

// ADC: battery level on P0.30 (AIN6)
// Accessible as machine.ADC(machine.Pin(30))

// PWM
#define MICROPY_HW_PWM0_NAME        "PWM0"
#define MICROPY_HW_PWM1_NAME        "PWM1"
#define MICROPY_HW_PWM2_NAME        "PWM2"

#define HELP_TEXT_BOARD_LED         "1"

// Board early init: sets UICR.REGOUT0 = 3.3V on first boot so GPIO output
// voltage is 3.3V instead of the default 1.8V. Required for SK6812 LEDs on 2.0.5.
extern void KSM1_board_early_init(void);
#define MICROPY_BOARD_EARLY_INIT    KSM1_board_early_init

# NeoPixel / WS2812 Support for nRF52 — Implementation Plan

## Overview

MicroPython's `neopixel` module uses `machine.bitstream()`, defined in
`extmod/machine_bitstream.c`. That file is already compiled into the nRF build
(confirmed by `build-KSM1/extmod/machine_bitstream.c.o`). The only missing
pieces are:

1. The feature flag `MICROPY_PY_MACHINE_BITSTREAM` is not enabled
2. The port-specific function `machine_bitstream_high_low()` does not exist

**Approach:** Direct PWM register writes (same as Adafruit_NeoPixel Arduino
lib). No nrfx driver — simpler, no heap allocation, no interrupt handler
needed. The PWM EasyDMA fires a one-shot DMA burst; the CPU just fills a
buffer and polls for completion.

---

## Timing Encoding

- **Clock:** 16 MHz, PRESCALER = DIV_1 → 62.5 ns per tick
- **COUNTERTOP = 20** → 1.25 µs bit period (800 kHz WS2812)
- **T0H duty = 6 ticks** (375 ns), **T1H duty = 13 ticks** (812 ns)
- **0x8000 polarity flag** on every entry: pin starts HIGH, falls at compare point
- **Reset:** 40 entries of `0 | 0x8000` → 50 µs low
- Buffer size: `numBytes × 8 + 40` uint16_t entries

---

## Files to Create / Modify

### 1. CREATE `ports/nrf/modules/machine/machine_bitstream.c`

New file. Core logic:

1. Compute T0H/T1H duty ticks from `timing_ns[0]` and `timing_ns[2]` (`ns * 16 / 1000`), OR with `0x8000`
2. Allocate VLA on stack: `(len × 8 + 40)` × `uint16_t`
3. Fill buffer MSB-first per byte, append 40 reset entries
4. Configure GPIO pin as output, drive low
5. Write PWM registers:
   - `ENABLE = Enabled`
   - `MODE = Up`
   - `PRESCALER = DIV_1`
   - `COUNTERTOP = 20`
   - `DECODER = Load_Common | Mode_RefreshCount`
   - `LOOP = 0`
   - `PSEL.OUT[0] = pin->pin`, channels 1–3 = `0xFFFFFFFF`
   - `SEQ[0].PTR/CNT/REFRESH/ENDDELAY`
6. Clear `EVENTS_SEQEND[0]`, write `TASKS_SEQSTART[0] = 1`
7. Poll `EVENTS_SEQEND[0]`
8. `TASKS_STOP`, `ENABLE = Disabled`, `PSEL.OUT[0] = 0xFFFFFFFF`, drive pin low

**PWM instance selection:**
| Chip | Instance | Reason |
|---|---|---|
| nRF52840 | `NRF_PWM3` | PWM3 is nRF52840-only; PWM0-2 used by `machine.PWM` |
| nRF52832 | `NRF_PWM2` | No PWM3; conflicts with `machine.PWM(device=2)` — document this |

### 2. MODIFY `ports/nrf/mpconfigport.h`

After the existing `MICROPY_PY_MACHINE_PULSE` define (~line 191), add:

```c
#ifndef MICROPY_PY_MACHINE_BITSTREAM
#if defined(NRF52840_XXAA) || defined(NRF52840)
#define MICROPY_PY_MACHINE_BITSTREAM (1)
#else
#define MICROPY_PY_MACHINE_BITSTREAM (0)
#endif
#endif
```

### 3. MODIFY `ports/nrf/Makefile`

Add to the `SRC_C` block (~line 227):

```makefile
    modules/machine/machine_bitstream.c \
```

---

## Gotchas

- **VLA stack size:** 150 pixels → ~3.7 KB. nRF52840 has 8 KB stack — fine for
  typical strips. >300 pixels approaches the limit; add a comment warning.
- **PSEL disconnect order:** Must do `TASKS_STOP` → `ENABLE=Disabled` →
  `PSEL.OUT[0]=0xFFFF`. Writing PSEL while ENABLE=1 can glitch the pin (per PS §6.31.4).
- **No IRQ disable needed:** DMA runs autonomously; SoftDevice is unaffected
  (PWM peripherals are not SD-restricted on nRF52840).
- **No DWT needed:** Unlike STM32/RP2 implementations, timing is fully
  hardware-enforced. No `mp_hal_quiet_timing_enter/exit`.
- **nrfx conflict on nRF52832:** Direct register access bypasses nrfx state.
  On nRF52840 with PWM3 (never touched by nrfx), non-issue.

---

## Verification

1. `make BOARD=KSM1` succeeds
2. `machine_bitstream_high_low` present in `.map` file
3. Python smoke test:
   ```python
   import machine, neopixel
   np = neopixel.NeoPixel(machine.Pin(X), 1)
   np[0] = (255, 0, 0)
   np.write()
   ```
4. Logic analyser: T0H ≈ 375 ns, T1H ≈ 812 ns, reset ≥ 50 µs

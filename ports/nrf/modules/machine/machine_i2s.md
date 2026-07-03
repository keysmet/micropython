# machine.I2S — nRF52840 port implementation notes

## Context

`machine.I2S` has no upstream nRF implementation. It exists on ESP32, RP2040,
STM32, and iMXRT. This document records the design decisions and findings for
the KSM1 board implementation.

## Hardware (KSM1)

| Signal | nRF52840 GPIO | MicroPython pin |
|--------|---------------|-----------------|
| BCK (SCK) | P0.05 | `I2S_BCK` |
| LRCK (WS) | P0.01 | `I2S_LRCK` |
| DIN (SDOUT) | P0.26 | `I2S_DIN` |

TX-only. No MCK pin exposed on the board.

## Configuration

Matches the working Arduino `keysmet.cpp` exactly:

| Parameter | Value |
|-----------|-------|
| Mode | Master |
| Format | I2S |
| Alignment | Left |
| Sample width | 16-bit |
| Channels | Stereo |
| MCK | 32MHz / 11 ≈ 2.909 MHz |
| RATIO | 128x |
| LRCK | ≈ 22727 Hz (nominally 22050 Hz) |

## Architecture

### Why not replicate the Arduino approach directly

The Arduino implementation uses a FreeRTOS task that polls `EVENTS_TXPTRUPD`
every 1ms and calls a user callback to fill the next buffer. MicroPython on nRF
runs a cooperative scheduler with no FreeRTOS — there are no background tasks.

### Chosen approach: nrfx IRQ + extmod ring buffer

MicroPython provides a common I2S framework in `extmod/machine_i2s.c` used by
all ports. It defines three operating modes (blocking, non-blocking, asyncio)
and manages a ring buffer that bridges Python time and hardware time. The port
provides four functions and a set of symbols; extmod does the rest.

```
Python:  i2s.write(buf)  →  copies samples into ring_buffer (blocks until drained)
IRQ:     hardware done   →  pops ring_buffer into dma_buf, calls nrfx_i2s_next_buffers_set
```

The IRQ replaces the FreeRTOS polling task. In blocking mode, `__WFI()` in
`MICROPY_EVENT_POLL_HOOK` sleeps the CPU until the I2S IRQ fires, making it
efficient.

### nrfx driver

`lib/nrfx/drivers/src/nrfx_i2s.c` is the HAL driver. Key points:

- `nrfx_i2s_init()` configures pins and enables the IRQ.
- `nrfx_i2s_start()` takes an initial buffer and starts the peripheral.
- The data handler is called from IRQ with `p_released` (the buffer just
  finished) and `status`. When `NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED` is set,
  `nrfx_i2s_next_buffers_set()` must be called before the current buffer runs
  out.
- On the **first** handler call, `p_released` points to a struct with both
  pointers NULL (no data has transferred yet). It is not a NULL pointer.
- `nrfx_i2s_uninit()` calls `nrfx_i2s_stop()` internally.

### Double-buffer design

Two DMA buffers (`dma_buf[0]` and `dma_buf[1]`) live as static fields in the
instance struct, ensuring they are always in RAM (required by EasyDMA).

- `nrfx_i2s_start()` is called with `dma_buf[0]`.
- The first IRQ arrives with `p_released = {NULL, NULL}` and
  `NEXT_BUFFERS_NEEDED` set → we supply `dma_buf[1]`.
- All subsequent IRQs: `p_released->p_tx_buffer` is the pointer that was just
  sent and is now free. We refill that exact pointer and hand it back. This
  keeps our state perfectly in sync with nrfx without any independent index.

## Files changed

| File | Change |
|------|--------|
| `ports/nrf/modules/machine/machine_i2s.c` | New file — port implementation |
| `ports/nrf/nrfx_config.h` | Added `NRFX_I2S_ENABLED` and `NRFX_I2S_DEFAULT_CONFIG_IRQ_PRIORITY` for `NRF52_SERIES` |
| `ports/nrf/mpconfigport.h` | Added `MICROPY_PY_MACHINE_I2S` default (0), includefile path, RX/TX constants, `RING_BUF`, `FINALISER` |
| `ports/nrf/Makefile` | Added `nrfx_i2s.c` to `SRC_NRFX` |
| `ports/nrf/boards/KSM1/mpconfigboard.h` | Added `#define MICROPY_PY_MACHINE_I2S (1)` |

## Port-level symbols required by extmod

`extmod/machine_i2s.c` expects the includefile to define these when
`MICROPY_PY_MACHINE_I2S_RING_BUF=1`:

| Symbol | Purpose |
|--------|---------|
| `i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES]` | Maps DMA frame bytes to app buffer positions for `readinto()` |
| `get_frame_mapping_index(bits, format)` | Returns row index into frame map |
| `SIZEOF_NON_BLOCKING_COPY_IN_BYTES` | Bytes copied per IRQ in non-blocking mode |

The frame map reflects the nRF LEFT-aligned I2S layout: 16-bit samples occupy
the high 16 bits of each 32-bit word. The map is only exercised on the RX path
(`readinto()`); the TX `write()` path copies app bytes into the ring buffer
directly without mapping.

## Non-blocking / scheduler

`mp_sched_schedule()` is called from IRQ context in non-blocking mode. This
requires `MICROPY_ENABLE_SCHEDULER=1`. KSM1 has `MICROPY_HW_ENABLE_USBDEV=1`
which unconditionally enables the scheduler in `mpconfigport.h` — confirmed
safe.

## Sample rate table

The nRF52840 I2S peripheral derives LRCK from a fixed 32 MHz source:
`LRCK = 32MHz / MCK_DIV / RATIO`. The table covers the most common rates;
the closest entry within 5% is selected at init time.

| Requested Hz | MCK divisor | RATIO | Actual LRCK |
|-------------|-------------|-------|-------------|
| 22050 | /11 | 128x | 22727 Hz |
| 32000 | /8  | 128x | 31250 Hz |
| 48000 | /21 | 32x  | 47619 Hz |
| 16000 | /15 | 128x | 16667 Hz |
| 8000  | /63 | 64x  |  7937 Hz |
| 11025 | /11 | 256x | 11364 Hz |
| 12000 | /21 | 128x | 11905 Hz |
| 24000 | /42 | 32x  | 23810 Hz |

## Usage

```python
from machine import I2S, Pin

i2s = I2S(0,
    sck=Pin(5),   # I2S_BCK
    ws=Pin(1),    # I2S_LRCK
    sd=Pin(26),   # I2S_DIN
    mode=I2S.TX,
    bits=16,
    format=I2S.STEREO,
    rate=22050,
    ibuf=4096,
)

# blocking write
i2s.write(samples_buf)

# non-blocking write
def cb(i2s):
    pass  # buffer has been sent
i2s.irq(cb)
i2s.write(samples_buf)
```

# KSM1 Boot & Power Notes

How the board boots, runs a user script, and sleeps. This documents the
implementation in `board.c` + `freeze/main.py` (and the scanner in
`freeze/ksm.py`). None of this runs in the WASM simulator — the device is the
only place it can be tested.

## Modes

- **MENU mode** — idle standby (slow orange pulse on LED 0). USB/REPL is live, so
  uploads and recovery always work. Pressing any of K1–K10 launches the user
  script. If there is no `/eeprom/app.py`, a press does nothing.
- **USER mode** — `/eeprom/app.py` is running (`setup()`/`loop()` and/or the
  `on*` callbacks). Triple-press MENU returns to MENU mode.
- **OFF** — nRF in System OFF (~µA). The switched rail (LEDs/audio/IMU via
  PWR_ON) is cut; the EEPROM and nRF core stay on the always-on VDD rail.

## GPREGRET — one-reset intent latch

`NRF_POWER->GPREGRET` (0x4000051C) survives both a reset and a System OFF wake,
and the Adafruit bootloader passes it through unless it holds the UF2 magic. It
carries boot intent across a reset without touching EEPROM:

    0xA1  RUN_USER   — main.py runs /eeprom/app.py once, then clears the flag
    0xA2  POWER_OFF  — main.py enters System OFF, then clears the flag
    0xA3  SLEEPING   — set by _system_off() just before sleeping; the next boot
                       reads it as "this is a wake" and runs the confirm gate
    0x57  BOOTLOADER — reserved Adafruit UF2 magic; NEVER written by our code

`main.py` reads GPREGRET once at boot and **clears it immediately**, before
acting. Clearing first is the anti-brick rule: a crash or reset loop can never
replay a stuck intent. Mode is not stored anywhere — every boot without an
intent flag lands in MENU, so a bad script (crash/watchdog/reset) always drops
back to a recoverable state instead of re-running.

## Power-off has exactly one owner: board.c

The 2s MENU-hold power-off lives in `KSM1_vm_hook()` (C), which the VM runs every
~200 bytecodes. Detecting it in C means it still works when user Python is stuck
in a tight loop that never cooperates with the key scanner. On trigger it:

1. writes `GPREGRET = POWER_OFF`,
2. **spins until MENU is released**, then
3. `NVIC_SystemReset()`.

The release-wait in step 2 is essential. An earlier attempt reset while MENU was
still held; `main.py` then re-read the still-held button as a wake gesture and
the board got stuck in a reset/re-wake loop. Waiting for release means `main.py`
always begins the power-off path with MENU already up — no double-edge, no race.
`ksm.py`'s scanner deliberately does **not** implement power-off, so there is
only one owner.

## Boot flow (main.py)

1. Read `GPREGRET`; clear it to 0 immediately.
2. `POWER_OFF` → `_system_off()` (never returns).
3. **Wake confirm:** only a `SLEEPING` boot runs this (the flag `_system_off` set
   before it slept — the MENU pin is never consulted on any other boot). MENU is
   still held from the press that woke us; require a 1s hold to confirm (silent,
   no LEDs); released early → back to System OFF.
4. `ksm.start()` — start the cooperative key scanner (Timer 2).
5. `RUN_USER` → load and run `/eeprom/app.py`; triple-press MENU → reset (→ MENU).
6. Otherwise → MENU mode; any K1–K10 press → set `GPREGRET = RUN_USER`, reset.

## Recovery / anti-brick

- USER runs only for an explicit one-shot `RUN_USER`. Any crash, watchdog, soft
  reset, or plain power-on has no flag → MENU. A bad script can't trap the board.
- USB/REPL stays live in MENU, so `mpremote` / the web tool can always upload.
- A script that hangs *and* ignores Ctrl+C is still killed by the 2s MENU hold
  (C-owned), which powers off; the next boot is MENU, not the bad script.

## Hardware notes

- **PWR_ON gates VDDH** (LEDs, audio amp, LDO, IMU) via load switch U8 — **not**
  the EEPROM. EEPROM VCC is on the nRF's always-on VDD rail, so cutting PWR_ON in
  `_system_off()` is safe and leaves EEPROM + GPREGRET intact for the next wake.
- Sleep still measures ~2.5mA (hardware 2.0.7) — higher than a clean System OFF
  should be. A power-optimization item, separate from this boot logic.

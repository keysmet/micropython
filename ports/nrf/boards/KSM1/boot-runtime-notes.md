# KSM1 Boot And Runtime Notes

This note captures the intended direction for KSM1 boot modes, live coding, key scanning, and power handling.

## Goals

- Keep live coding simple: uploading a script to EEPROM should still start the user script with the current live-coding protocol.
- Make broken user scripts recoverable: a bad script should not permanently trap the board away from upload/menu access.
- Keep power behavior consistent: the same long MENU hold duration should be used for power-on confirmation and power-off.
- Avoid duplicated long-press logic: power-off should have one owner, not competing Python and C implementations.
- Keep user code beginner-friendly: examples using `wait()` should continue to service keys, callbacks, LEDs, and scheduled work.

## Boot Modes

The practical modes are:

- MENU mode: upload/live-coding mode. User scripts are not automatically running unless the live-coding flow starts them.
- USER mode: the uploaded script from `/eeprom/app.py` is running.
- OFF mode: external power is cut and the nRF is in System OFF.

For now, keep the existing live-coding protocol so the web tool continues to work: upload to `/eeprom/app.py`, then restart/run using the current flow.

Longer term, the mode state does not need to live in EEPROM if reset/crash/watchdog default to MENU and live coding explicitly starts USER. EEPROM should primarily store user files.

## Ownership By File

- `ports/nrf/boards/KSM1/board.c`: owns board-level emergency behavior that must work even when Python user code is bad. This includes long MENU hold detection while Python bytecode is running, reset flags in `GPREGRET`, and entering the bootloader.
- `ports/nrf/boards/KSM1/mpconfigboard.h`: wires `KSM1_vm_hook()` into `MICROPY_VM_HOOK_*` and defines board-specific feature switches.
- `ports/nrf/freeze/main.py`: owns high-level boot flow after MicroPython is alive: power-on confirmation, EEPROM mount assumptions, MENU/USER dispatch, and acting on reset flags.
- `ports/nrf/freeze/ksm.py`: owns the public Python API, cooperative key scanning from `tick()`, callbacks, LED updates, scheduled callbacks, and triple-MENU detection.
- `ports/nrf/modules/scripts/_boot.py`: should only mount `/eeprom` and set up paths. It should not decide whether user code runs.

## Reset Flags

Use `NRF_POWER->GPREGRET` as a one-reset intent latch. It is better suited than EEPROM for transient boot intent because it survives reset but does not persist as user data.

Suggested values:

```c
#define KSM1_GPREGRET_NONE       0x00
#define KSM1_GPREGRET_RUN_USER   0xA1
#define KSM1_GPREGRET_POWER_OFF  0xA2
#define KSM1_GPREGRET_BOOTLOADER 0x57  // existing Adafruit UF2 bootloader magic
```

Expected rules:

- `RUN_USER`: boot into USER once, clear the flag before loading `/eeprom/app.py`.
- `POWER_OFF`: boot into the Python/board power-off path, clear the flag, then enter System OFF before user code can run.
- no flag, watchdog reset, crash reset, Ctrl+C/soft reset, or ordinary reset: boot to MENU.
- `BOOTLOADER`: leave the existing UF2 bootloader behavior as-is.

If a flag is consumed by `main.py`, clear it early. This prevents a broken app or reset loop from repeatedly skipping MENU.

Implementation location:

- `board.c` should provide tiny helper functions/macros for writing GPREGRET from C.
- `main.py` can read and clear `machine.mem32[...]` directly, or use a small board helper module later if one exists.
- Keep EEPROM mode bytes out of this flow unless a future requirement needs a truly persistent user preference.

## Boot Flow

`_boot.py`:

1. Power the external rail if needed.
2. Mount the M24256 EEPROM at `/eeprom`.
3. Append `/eeprom` and `/eeprom/lib` to `sys.path`.
4. Do not load or run `/eeprom/app.py`.

`main.py`:

1. Read `GPREGRET`.
2. Clear any KSM-owned flag immediately.
3. If the flag is `POWER_OFF`, enter System OFF.
4. If this boot is a wake from System OFF with MENU already held, require the full power-hold duration. If released too soon, go back to System OFF.
5. If the flag is `RUN_USER`, load and run `/eeprom/app.py`.
6. Otherwise enter MENU mode.

MENU mode:

- Keep USB/REPL/live-coding available.
- Single MENU press starts USER mode.
- The current web upload protocol can continue to write `/eeprom/app.py` and reset/run as it does today.

USER mode:

- Load `/eeprom/app.py`.
- Run `setup()`/`loop()` if present.
- Also allow top-level scripts with their own `while True` as long as they call `wait()`/`tick()`.
- Triple MENU detected from `ksm.tick()` returns to MENU.

Crash, watchdog, or ordinary reset:

- No `RUN_USER` flag should be present, so the board comes back in MENU.

## Key Scanning

The Python `machine.Timer` scanner is more complicated than needed and risks running user callbacks from timer interrupt context.

Preferred model:

- `ksm.tick()` scans keys.
- `ksm.tick()` dispatches `onPress`, `onRelease`, `onTap`, and `onUpdate`.
- `ksm.wait(ms)` loops through `tick()` often enough to keep the system responsive.
- User code that runs for a long time should call `wait()` or `tick()` regularly.

This supports code like:

```python
from ksm import *

while True:
    setColor(1, (255, 0, 0))
    wait(200)
    setColor(1, (0, 255, 0))
    wait(200)
```

Triple MENU press can stay in this cooperative Python path. It works while user code uses `wait()`/`tick()`, which is the intended coding model.

Implementation details for `ksm.py`:

- Remove `machine.Timer` from the scanner path.
- Remove `_timer_keys`, `_timer_cb()`, and `start()`.
- Call `_scan_keys()` at the start of `tick()`.
- Keep `_scan_keys()` responsible for updating `_state`, `_press`, `_release`, tap state, and triple-MENU state.
- Dispatch user callbacks only from `tick()`, not from C or interrupt context.
- Remove long MENU power-off/reset handling from `_scan_keys()`.

Implementation details for `main.py`:

- Remove `ksm.start()`.
- MENU loops and USER loops should call `ksm.tick()` or `ksm.wait()` instead of `time.sleep_ms()` when they need key handling.
- If USER mode runs a `loop()` function, the firmware loop calls `_loop()` then `ksm.tick()`.
- If USER mode runs top-level user code, that user code is responsible for calling `wait()`/`tick()` to service cooperative events.

## Power Handling

Long MENU hold should be owned by the board/C layer, not by Python key scanning.

Runtime behavior:

- In MENU or USER mode, long MENU hold powers off.
- During power-on from System OFF, MENU must stay held for the same duration.
- If MENU is released too early during wake, the board goes back to System OFF.

Keeping long-press power handling in one place avoids two independent implementations drifting apart. It also preserves an escape path for bad Python loops where cooperative `tick()` is not running.

Implementation detail:

- `KSM1_vm_hook()` in `board.c` should sample the MENU GPIO and maintain a small hold timer.
- Once MENU has been held for the configured duration, `board.c` should set `GPREGRET = KSM1_GPREGRET_POWER_OFF` and reset.
- `main.py` sees `POWER_OFF` before loading user code and enters the same System OFF sequence used by normal firmware power-off.
- If later it is safe and simple to enter System OFF entirely from C, `board.c` may do that directly. Until then, the reset flag keeps I2C/LED/power-rail details in Python.

This means Python does not independently decide that a long MENU hold powers off. Python may show progress or expose helper APIs later, but the authoritative transition is the board-level long-hold path.

Power-on confirmation remains in `main.py` because it runs before user code, can update LEDs, and can return to System OFF if MENU is released before the duration.

## Live Coding And Reloading

For now, keep the current protocol so the live-coding app does not break.

Future simplification to consider:

- Replace passive "new file detected" reload behavior with an explicit command from the live-coding tool.
- The command would write the new script to EEPROM, complete the file update atomically, and then start/restart the user script.
- Before restarting a script, clear app-owned callbacks, scheduled work, loop/setup references, HID/gamepad state, and then run garbage collection.

That explicit protocol would be easier to reason about than polling file modification time, especially when scripts are already running.

Current compatibility target:

- The web app writes `/eeprom/app.py`.
- The web app may interrupt the running script and reset the board.
- Firmware should continue to support that path until the web protocol is changed.

Future explicit protocol:

```python
import ksm
ksm.install_app(source_bytes_or_text)
ksm.run_app()
```

Possible behavior:

- `install_app()` writes `/eeprom/app.py.tmp`, closes it, then renames it to `/eeprom/app.py`.
- `run_app()` sets `GPREGRET = RUN_USER` and resets, or directly starts USER if no hardware reset is needed.
- If a hard reset is needed after upload, `RUN_USER` gives exactly one USER boot and then clears itself.

This would replace the file watcher. A direct command is less surprising than detecting `mtime` changes and trying to reload while a previous app may still be running.

## Memory Management

Restarting a Python script by `exec()` can be acceptable if old app state is deliberately released before loading the new app.

Important cleanup before loading/reloading:

- Clear `_setup` and `_loop`.
- Clear callback references on `ksm`.
- Clear or intentionally preserve scheduled callbacks and tweens.
- Release HID/gamepad state if it may be held.
- Call `gc.collect()` after references are dropped.

The key invariant is that no long-lived firmware object should keep references to functions or objects from the previous app namespace unless that is intentional.

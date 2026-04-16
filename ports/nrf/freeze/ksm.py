# ksm.py — public hardware API for KSM1
#
# Usage in your app:
#   from ksm import *          → all public names available directly
#   import ksm                 → use as ksm.key_press(ksm.KEY_K1), etc.
#
# Call ksm.start() is handled by main.py — do NOT call it from your app.

from machine import Pin, Timer
from neopixel import NeoPixel
import array, time, machine

# ── NeoPixels ──────────────────────────────────────────────────────────────────
# change to 11 if led MENU is present
NB_LEDS = 10
np = NeoPixel(Pin(0), NB_LEDS)

def clear_all():
    """Turn off all LEDs."""
    for i in range(NB_LEDS):
        np[i] = (0, 0, 0)
    np.write()

# ── Key constants ──────────────────────────────────────────────────────────────
KEY_MENU = 0
KEY_K1   = 1
KEY_K10  = 10

# ── Key scanner (internal state — do not use directly) ─────────────────────────
_KEY_PINS = [42, 22, 17, 13, 8, 6, 24, 20, 15, 41, 27]
_KEY_COUNT = len(_KEY_PINS)

_pins    = [Pin(p, Pin.IN, Pin.PULL_UP) for p in _KEY_PINS]
_state   = array.array('b', [0] * _KEY_COUNT)
_prev    = array.array('b', [0] * _KEY_COUNT)
_press   = array.array('b', [0] * _KEY_COUNT)
_hold_ms = array.array('l', [0] * _KEY_COUNT)

# Triple-press MENU detection — pre-allocated arrays (no heap allocation in ISR)
_menu_press_count = array.array('b', [0])
_menu_press_last  = array.array('l', [0])
_menu_triple      = array.array('b', [0])
_TRIPLE_WINDOW_MS = 500

_timer_keys = None

def _scan_keys(timer):
    now = time.ticks_ms()
    for i in range(_KEY_COUNT):
        curr = 1 if _pins[i].value() == 0 else 0
        if curr == 1 and _prev[i] == 0:
            _press[i]   = 1
            _hold_ms[i] = now
            # Triple-press detection on MENU
            if i == KEY_MENU:
                if time.ticks_diff(now, _menu_press_last[0]) < _TRIPLE_WINDOW_MS:
                    _menu_press_count[0] += 1
                else:
                    _menu_press_count[0] = 1
                _menu_press_last[0] = now
                if _menu_press_count[0] >= 3:
                    _menu_triple[0] = 1
                    _menu_press_count[0] = 0
        _prev[i]  = _state[i]
        _state[i] = curr
    # Power off: MENU held 2s → reset (POWER flag pre-written by main.py)
    if _state[KEY_MENU] and time.ticks_diff(now, _hold_ms[KEY_MENU]) >= 2000:
        machine.reset()

def start():
    """Start the hardware key scanner. Called once by main.py after boot checks."""
    global _timer_keys
    _timer_keys = Timer(2, period=10000, mode=Timer.PERIODIC, callback=_scan_keys)
    _timer_keys.start()

# ── Public API ─────────────────────────────────────────────────────────────────

def key_down(k):
    """True while key k is held."""
    return bool(_state[k])

def key_press(k):
    """True once per press (consumes the event)."""
    if _press[k]:
        _press[k] = 0
        return True
    return False

def key_hold(k, ms):
    """True if key k has been held for at least ms milliseconds."""
    if not _state[k]:
        return False
    return time.ticks_diff(time.ticks_ms(), _hold_ms[k]) >= ms

def menu_triple_press():
    """True once if MENU was pressed 3 times quickly (<500ms between presses).
    Used by main.py to switch MODE. Available to user scripts too."""
    if _menu_triple[0]:
        _menu_triple[0] = 0
        return True
    return False

# ── Exports ────────────────────────────────────────────────────────────────────
# Controls what 'from ksm import *' pulls in.
# Private names (_scan_keys, _state, etc.) are excluded automatically.
__all__ = [
    'NB_LEDS', 'np', 'clear_all',
    'KEY_MENU', 'KEY_K1', 'KEY_K10',
    'key_press', 'key_down', 'key_hold', 'menu_triple_press',
]

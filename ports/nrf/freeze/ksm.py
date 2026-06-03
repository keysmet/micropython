# ksm.py — public hardware API for KSM1

from machine import Pin, Timer
from neopixel import NeoPixel
import machine
import time as _time

from pins import *

# ── NeoPixels ──────────────────────────────────────────────────────────────────
NB_LEDS = 10
np      = NeoPixel(Pin(PIN_LED), NB_LEDS)
_dirty  = False

def setColor(key, clr):
    global _dirty
    if key == 0: return
    np[NB_LEDS - key] = clr
    _dirty = True

def clearAll():
    global _dirty
    for i in range(NB_LEDS):
        np[i] = (0, 0, 0)
    np.write()
    _dirty = False

def _flush():
    global _dirty
    if _dirty:
        np.write()
        _dirty = False

def flashColor(key, clr, ms):
    orig = np[NB_LEDS - key] if key != 0 else (0, 0, 0)
    setColor(key, clr)
    delay(ms, lambda: setColor(key, orig))

def fadeColor(key, clr, ms):
    if key == 0: return
    start = np[NB_LEDS - key]
    for i in range(len(_tweens) - 1, -1, -1):
        if _tweens[i][4] == key:
            _tweens.pop(i)
    _tweens.append([_time.ticks_ms(), ms, start, clr, key])

# ── Color helpers ──────────────────────────────────────────────────────────────
class color:
    @staticmethod
    def rgb(r, g, b):
        return (int(r * 255), int(g * 255), int(b * 255))

    @staticmethod
    def hsl(h, s, l):
        if s == 0:
            v = int(l * 255)
            return (v, v, v)
        q = l * (1 + s) if l < 0.5 else l + s - l * s
        p = 2 * l - q
        t = h + 1/3
        if t > 1: t -= 1
        if t < 1/6:   r = p + (q-p)*6*t
        elif t < 0.5: r = q
        elif t < 2/3: r = p + (q-p)*(2/3-t)*6
        else:         r = p
        t = h
        if t < 1/6:   g = p + (q-p)*6*t
        elif t < 0.5: g = q
        elif t < 2/3: g = p + (q-p)*(2/3-t)*6
        else:         g = p
        t = h - 1/3
        if t < 0: t += 1
        if t < 1/6:   b = p + (q-p)*6*t
        elif t < 0.5: b = q
        elif t < 2/3: b = p + (q-p)*(2/3-t)*6
        else:         b = p
        return (int(r*255), int(g*255), int(b*255))

    @staticmethod
    def mul(a, b):
        return (a[0] * b[0] // 255, a[1] * b[1] // 255, a[2] * b[2] // 255)

    @staticmethod
    def add(a, b):
        return (min(255, a[0] + b[0]), min(255, a[1] + b[1]), min(255, a[2] + b[2]))

    @staticmethod
    def mix(a, b, t):
        return (int(a[0] + (b[0] - a[0]) * t),
                int(a[1] + (b[1] - a[1]) * t),
                int(a[2] + (b[2] - a[2]) * t))

def lerp(a, b, t):
    return a + (b - a) * t

# ── Key constants ──────────────────────────────────────────────────────────────
KEY_MENU = 0
KEY_K1   = 1
KEY_K10  = 10

# ── Key scanner ────────────────────────────────────────────────────────────────
_KEY_PINS  = [PIN_MENU, PIN_K1, PIN_K2, PIN_K3, PIN_K4, PIN_K5,
              PIN_K6,   PIN_K7, PIN_K8, PIN_K9, PIN_K10]
_KEY_COUNT = len(_KEY_PINS)

_pins    = [Pin(p, Pin.IN, Pin.PULL_UP) for p in _KEY_PINS]
_state   = [0] * _KEY_COUNT
_press   = [0] * _KEY_COUNT
_release = [0] * _KEY_COUNT
_hold_ms = [0] * _KEY_COUNT
_tap_ms  = [0] * _KEY_COUNT

_TAP_MAX_MS = 200

# Triple-press MENU detection
_menu_press_count = 0
_menu_press_last  = 0
_menu_triple      = False
_TRIPLE_WINDOW_MS = 500

_pre_reset_hooks = []
_timer_keys = None  # keeps the Timer object alive (GC would stop it)

# ── Event callbacks ────────────────────────────────────────────────────────────
# main.py wires these from the app namespace after loading app.py.
# Can also be set directly: ksm.onPress = my_fn
onPress       = None
onRelease     = None
onTap         = None
onUpdate      = None
onMenuPress   = None
onMenuRelease = None
onMenuTap     = None

def _scan_keys():
    global _menu_press_count, _menu_press_last, _menu_triple
    now = _time.ticks_ms()
    for i in range(_KEY_COUNT):
        curr = 1 if _pins[i].value() == 0 else 0
        if curr == 1 and _state[i] == 0:       # rising edge
            _press[i]   = 1
            _hold_ms[i] = now
            _tap_ms[i]  = now
            if i == KEY_MENU:
                if _time.ticks_diff(now, _menu_press_last) < _TRIPLE_WINDOW_MS:
                    _menu_press_count += 1
                else:
                    _menu_press_count = 1
                _menu_press_last = now
                if _menu_press_count >= 3:
                    _menu_triple = True
                    _menu_press_count = 0
                if onMenuPress:
                    try: onMenuPress()
                    except Exception as e: print("onMenuPress:", e)
            else:
                if onPress:
                    try: onPress(i)
                    except Exception as e: print("onPress:", e)
        elif curr == 0 and _state[i] == 1:     # falling edge
            _release[i] = 1
            if i == KEY_MENU:
                if _time.ticks_diff(now, _tap_ms[i]) <= _TAP_MAX_MS:
                    if onMenuTap:
                        try: onMenuTap()
                        except Exception as e: print("onMenuTap:", e)
                if onMenuRelease:
                    try: onMenuRelease()
                    except Exception as e: print("onMenuRelease:", e)
            else:
                if _time.ticks_diff(now, _tap_ms[i]) <= _TAP_MAX_MS:
                    if onTap:
                        try: onTap(i)
                        except Exception as e: print("onTap:", e)
                if onRelease:
                    try: onRelease(i)
                    except Exception as e: print("onRelease:", e)
        _state[i] = curr
    # Power off: MENU held 2s → fire hooks then reset
    if _state[KEY_MENU] and _time.ticks_diff(now, _hold_ms[KEY_MENU]) >= 2000:
        for fn in _pre_reset_hooks:
            try: fn()
            except Exception: pass
        machine.reset()

def _timer_cb(t):
    _scan_keys()

def start():
    global _timer_keys
    _timer_keys = Timer(2, period=10000, mode=Timer.PERIODIC, callback=_timer_cb)
    _timer_keys.start()

# ── Scheduled callbacks and tweens ────────────────────────────────────────────
_scheduled    = []
_tweens       = []  # [start_ms, duration_ms, start_clr, end_clr, key]
_last_tick_ms = _time.ticks_ms()

def delay(ms, fn):
    _scheduled.append([_time.ticks_add(_time.ticks_ms(), ms), fn])

def tick():
    """Fire scheduled callbacks and yield 10ms. Always returns True."""
    global _last_tick_ms
    now = _time.ticks_ms()
    i = 0
    while i < len(_scheduled):
        if _time.ticks_diff(now, _scheduled[i][0]) >= 0:
            fn = _scheduled.pop(i)[1]
            fn()
        else:
            i += 1
    i = 0
    while i < len(_tweens):
        tw = _tweens[i]
        elapsed = _time.ticks_diff(now, tw[0])
        if elapsed >= tw[1]:
            setColor(tw[4], tw[3])
            _tweens.pop(i)
        else:
            t = elapsed / tw[1]
            setColor(tw[4], (int(tw[2][0] + (tw[3][0] - tw[2][0]) * t),
                             int(tw[2][1] + (tw[3][1] - tw[2][1]) * t),
                             int(tw[2][2] + (tw[3][2] - tw[2][2]) * t)))
            i += 1
    _flush()
    if onUpdate:
        try: onUpdate(_time.ticks_diff(now, _last_tick_ms))
        except Exception as e: print("onUpdate:", e)
    _last_tick_ms = now
    _time.sleep_ms(10)
    return True

# ── Time tracking ──────────────────────────────────────────────────────────────
_time_ref = _time.ticks_ms()

def time():
    return _time.ticks_diff(_time.ticks_ms(), _time_ref)

def resetTime():
    global _time_ref
    _time_ref = _time.ticks_ms()

# ── Utility ────────────────────────────────────────────────────────────────────
def restart():
    machine.soft_reset()

# ── Public key API ─────────────────────────────────────────────────────────────
def down(*keys):
    """Returns a pressed key number (True for MENU), or False."""
    if not keys:
        for i in range(1, _KEY_COUNT):
            if _state[i]: return i
        return False
    for k in keys:
        if _state[k]: return k or True  # KEY_MENU=0 serait falsy sans ce or True
    return False

def press(*keys):
    """Returns key number on press (True for MENU, consumes event), or False."""
    if not keys:
        for i in range(1, _KEY_COUNT):
            if _press[i]:
                _press[i] = 0
                return i
        return False
    for k in keys:
        if _press[k]:
            _press[k] = 0
            return k or True  # KEY_MENU=0 serait falsy sans ce or True
    return False

def release(*keys):
    """Returns key number on release (True for MENU, consumes event), or False."""
    if not keys:
        for i in range(1, _KEY_COUNT):
            if _release[i]:
                _release[i] = 0
                return i
        return False
    for k in keys:
        if _release[k]:
            _release[k] = 0
            return k or True  # KEY_MENU=0 serait falsy sans ce or True
    return False

def hold(key, ms):
    if not _state[key]:
        return False
    return _time.ticks_diff(_time.ticks_ms(), _hold_ms[key]) >= ms

def wait(ms):
    deadline = _time.ticks_add(_time.ticks_ms(), ms)
    while _time.ticks_diff(deadline, _time.ticks_ms()) > 0:
        tick()

def waitPress(*keys):
    while True:
        tick()
        k = press(*keys)
        if k is not False:
            return k

def waitRelease(*keys):
    while True:
        tick()
        k = release(*keys)
        if k is not False:
            return k

def waitUntil(fn):
    while not fn():
        tick()

def menu_triple_press():
    """True once if MENU was pressed 3 times quickly. Used internally by main.py."""
    global _menu_triple
    if _menu_triple:
        _menu_triple = False
        return True
    return False

# ── USB HID ────────────────────────────────────────────────────────────────────
# hid_keys([keycode, ...], modifier=0) — send a USB HID keyboard report.
# Keycodes: USB HID usage page 0x07 (e.g. 4=A, 40=Enter, 79=Right, 80=Left).
# Pass [] to release all keys.
try:
    from hid import hid_keys
except ImportError:
    def hid_keys(keycodes, modifier=0):
        pass

# ── Exports ────────────────────────────────────────────────────────────────────
__all__ = [
    'NB_LEDS', 'clearAll', 'setColor', 'flashColor', 'fadeColor', 'color', 'lerp',
    'KEY_MENU', 'KEY_K1', 'KEY_K10',
    'down', 'press', 'release', 'hold',
    'waitPress', 'waitRelease', 'waitUntil',
    'tick', 'delay', 'wait',
    'time', 'resetTime', 'restart',
    'hid_keys',
]

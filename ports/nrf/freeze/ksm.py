# ksm.py — public hardware API for KSM1

from machine import Pin
from neopixel import NeoPixel
import array, machine
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
_state   = array.array('b', [0] * _KEY_COUNT)
_press   = array.array('b', [0] * _KEY_COUNT)
_release = array.array('b', [0] * _KEY_COUNT)
_hold_ms = array.array('l', [0] * _KEY_COUNT)
_tap_ms  = array.array('l', [0] * _KEY_COUNT)

_TAP_MAX_MS = 200

# Triple-press MENU detection
_menu_press_count = array.array('b', [0])
_menu_press_last  = array.array('l', [0])
_menu_triple      = array.array('b', [0])
_TRIPLE_WINDOW_MS = 500

_pre_reset_hooks = []

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
    now = _time.ticks_ms()
    for i in range(_KEY_COUNT):
        curr = 1 if _pins[i].value() == 0 else 0
        if curr == 1 and _state[i] == 0:       # rising edge
            _press[i]   = 1
            _hold_ms[i] = now
            _tap_ms[i]  = now
            if i == KEY_MENU:
                if _time.ticks_diff(now, _menu_press_last[0]) < _TRIPLE_WINDOW_MS:
                    _menu_press_count[0] += 1
                else:
                    _menu_press_count[0] = 1
                _menu_press_last[0] = now
                if _menu_press_count[0] >= 3:
                    _menu_triple[0] = 1
                    _menu_press_count[0] = 0
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
            except: pass
        machine.reset()

def start():
    pass  # no-op — key scanning happens in tick()

# ── Scheduled callbacks ────────────────────────────────────────────────────────
_scheduled    = []
_last_tick_ms = array.array('l', [_time.ticks_ms()])

def delay(ms, fn):
    _scheduled.append([_time.ticks_add(_time.ticks_ms(), ms), fn])

def tick():
    """Scan keys, fire callbacks, and yield 10ms. Always returns True."""
    now = _time.ticks_ms()
    _scan_keys()
    i = 0
    while i < len(_scheduled):
        if _time.ticks_diff(now, _scheduled[i][0]) >= 0:
            fn = _scheduled.pop(i)[1]
            fn()
        else:
            i += 1
    _flush()
    if onUpdate:
        try: onUpdate(_time.ticks_diff(now, _last_tick_ms[0]))
        except Exception as e: print("onUpdate:", e)
    _last_tick_ms[0] = now
    _time.sleep_ms(10)
    return True

# ── Time tracking ──────────────────────────────────────────────────────────────
_time_ref = array.array('l', [_time.ticks_ms()])

def time():
    return _time.ticks_diff(_time.ticks_ms(), _time_ref[0])

def resetTime():
    _time_ref[0] = _time.ticks_ms()

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
    if _menu_triple[0]:
        _menu_triple[0] = 0
        return True
    return False

# ── Exports ────────────────────────────────────────────────────────────────────
__all__ = [
    'NB_LEDS', 'clearAll', 'setColor', 'flashColor', 'color', 'lerp',
    'KEY_MENU', 'KEY_K1', 'KEY_K10',
    'down', 'press', 'release', 'hold',
    'waitPress', 'waitRelease', 'waitUntil',
    'tick', 'delay', 'wait',
    'time', 'resetTime', 'restart',
]

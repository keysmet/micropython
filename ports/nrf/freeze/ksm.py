# ksm.py — public hardware API for KSM1

from machine import Pin
from neopixel import NeoPixel
import machine
import time as _time
import sys as _sys

from pins import *

# ── Hang watchdog (firmware) ───────────────────────────────────────────────────
# tick() feeds the firmware watchdog (board.c) so a non-yielding loop reboots to
# MENU; start()/stop() disarm it around boot and shutdown. On the sim there is no
# `hid` module; its watchdog (mp_js_hook) is fed by sleep_ms instead, so feed and
# disarm are no-ops there.
try:
    from hid import feed as _watchdog_feed, disarm as _watchdog_disarm
except ImportError:
    def _watchdog_feed():
        pass
    def _watchdog_disarm():
        pass

# ── NeoPixels ──────────────────────────────────────────────────────────────────
# Each key is an Led (below), recomposited every tick (no scheduled callbacks):
#   base:  solid | fade (from→to over ms) | blink (from↔to, period ms)
#   flash: a temporary color laid on top that decays back into the base.
# Colors are plain ints (0xRRGGBB), so compositing allocates nothing per frame.
NB_LEDS = 11
np      = NeoPixel(Pin(PIN_LED), NB_LEDS)

# On the real device, LEDs are perceptually corrected (f² gamma) so mid-tones
# match the linear sim. The sim writes colors straight through.
_GAMMA = _sys.platform.startswith("nrf")   # True on device, False in the sim

def _pix(key):
    return 0 if key == 0 else NB_LEDS - key

# A color is a 24-bit int 0xRRGGBB. Accepts "#RRGGBB" and (r,g,b) too.
def _to_int(c):
    if type(c) is int: return c
    if type(c) is str: return int(c[1:] if c[0] == '#' else c, 16)
    return (c[0] << 16) | (c[1] << 8) | c[2]

# One Led per key holds its base (solid/fade/blink) and flash-overlay state,
# and knows how to composite itself. Mutating fields allocates nothing per frame.
class Led:
    __slots__ = ('to', 'frm', 't0', 'ms', 'blink', 'f_from', 'f_t0', 'f_ms', 'shown')

    def __init__(self):
        self.to = self.frm = 0
        self.t0 = self.ms = 0
        self.blink = False
        self.f_from = self.f_t0 = self.f_ms = 0
        self.shown = -1                       # last value written to np

    def set(self, c):
        self.to = self.frm = c
        self.ms = 0
        self.blink = False

    def fade(self, c, ms):
        self.frm = self.compose()             # fade from what's shown now
        self.to, self.t0, self.ms, self.blink = c, _time.ticks_ms(), ms, False

    def do_blink(self, frm, to, period):
        self.frm, self.to = frm, to
        self.t0, self.ms, self.blink = _time.ticks_ms(), period, True

    def flash(self, c, ms):
        self.f_from, self.f_t0, self.f_ms = c, _time.ticks_ms(), ms

    def _base(self, now):
        if self.ms <= 0:
            return self.to
        t = _time.ticks_diff(now, self.t0) / self.ms
        if self.blink:
            t %= 1.0
            t = t * 2 if t < 0.5 else 2 - t * 2      # 0→1→0 triangle
        elif t >= 1.0:
            return self.to
        return color.mix(self.frm, self.to, t)

    # Displayed color: base with the decaying flash mixed on top.
    def compose(self, now=None):
        if now is None: now = _time.ticks_ms()
        c = self._base(now)
        if self.f_ms > 0:
            t = _time.ticks_diff(now, self.f_t0) / self.f_ms
            if t >= 1.0: self.f_ms = 0
            else: c = color.mix(self.f_from, c, t)
        return c

_leds = [Led() for _ in range(NB_LEDS)]

def setColor(key, clr):   _leds[key].set(_to_int(clr))
def fadeColor(key, clr, ms): _leds[key].fade(_to_int(clr), ms)
def blink(key, frm, to, period): _leds[key].do_blink(_to_int(frm), _to_int(to), period)
def flashColor(key, clr, ms): _leds[key].flash(_to_int(clr), ms)

def clearAll():
    for led in _leds:
        led.set(0)
        led.f_ms = 0
    _flush()

def _flush():
    now = _time.ticks_ms()
    for k in range(NB_LEDS):
        led = _leds[k]
        c = led.compose(now)
        if c != led.shown:
            led.shown = c
            if _GAMMA:  # f² per channel: (v*v)//255
                r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
                np[_pix(k)] = (r * r // 255, g * g // 255, b * b // 255)
            else:
                np[_pix(k)] = ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)
    np.write()

# ── Colors ───────────────────────────────────────────────────────────────────
# Every color is a 24-bit integer 0xRRGGBB. Named colors and the color.* helpers
# all return ints, so nothing here allocates. The most common way to pick a color
# is a hex literal (0xFF8800) or a name (ORANGE); color.rgb/color.hsl are for
# building one from numbers.
BLACK   = 0x000000
WHITE   = 0xFFFFFF
RED     = 0xFF0000
GREEN   = 0x00FF00
BLUE    = 0x0000FF
YELLOW  = 0xFFFF00
CYAN    = 0x00FFFF
MAGENTA = 0xFF00FF
ORANGE  = 0xFF8000
PURPLE  = 0x8000FF
PINK    = 0xFF40A0

class color:
    @staticmethod
    def rgb(r, g, b):
        """Build a color from red/green/blue, each 0..255. rgb(255, 0, 0) is red."""
        return (r << 16) | (g << 8) | b

    @staticmethod
    def hsl(h, s, l):
        """Build a color from hue (degrees 0..360), saturation and lightness (0..1).
        hsl(0, 1, 0.5) is red, hsl(120, 1, 0.5) green, hsl(240, 1, 0.5) blue."""
        h = (h % 360) / 360
        if s == 0:
            v = int(l * 255)
            return (v << 16) | (v << 8) | v
        q = l * (1 + s) if l < 0.5 else l + s - l * s
        p = 2 * l - q
        return (int(_hue(p, q, h + 1/3) * 255) << 16
                | int(_hue(p, q, h) * 255) << 8
                | int(_hue(p, q, h - 1/3) * 255))

    @staticmethod
    def mix(a, b, t):
        """Blend between two colors. t=0 gives a, t=1 gives b, 0.5 is halfway."""
        ar, ag, ab = (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF
        br, bg, bb = (b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF
        return (int(ar + (br - ar) * t) << 16
                | int(ag + (bg - ag) * t) << 8
                | int(ab + (bb - ab) * t))

    @staticmethod
    def scale(c, f):
        """Dim or brighten a color. scale(c, 0.5) is half brightness, 0 is black."""
        r = min(255, int(((c >> 16) & 0xFF) * f))
        g = min(255, int(((c >> 8) & 0xFF) * f))
        b = min(255, int((c & 0xFF) * f))
        return (r << 16) | (g << 8) | b

    @staticmethod
    def add(a, b):
        """Add two colors channel by channel, clamped (like mixing light)."""
        return (min(255, ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) << 16
                | min(255, ((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) << 8
                | min(255, (a & 0xFF) + (b & 0xFF)))

def _hue(p, q, t):
    if t < 0: t += 1
    if t > 1: t -= 1
    if t < 1/6:   return p + (q - p) * 6 * t
    if t < 0.5:   return q
    if t < 2/3:   return p + (q - p) * (2/3 - t) * 6
    return p

def lerp(a, b, t):
    return a + (b - a) * t

# ── Key constants ──────────────────────────────────────────────────────────────
KEY_MENU = 0
KEY_K1   = 1
KEY_K10  = 10

# ── Keys ───────────────────────────────────────────────────────────────────────
_KEY_PINS  = [PIN_MENU, PIN_K1, PIN_K2, PIN_K3, PIN_K4, PIN_K5,
              PIN_K6,   PIN_K7, PIN_K8, PIN_K9, PIN_K10]
_KEY_COUNT = len(_KEY_PINS)

_TAP_MAX_MS = 200

# One Key per button: its pin, whether it's down, the press/release edges the
# public API consumes, and the timestamps for hold()/tap. scan() reads the pin
# (active-low) and returns +1 on a press edge, -1 on release, 0 otherwise.
# Runs inside the scan timer ISR, so it must not allocate — fields are mutated
# in place (__slots__, plain ints).
class Key:
    __slots__ = ('pin', 'down', 'pressed', 'released', 'hold_ms', 'tap_ms')

    def __init__(self, gpio):
        self.pin = Pin(gpio, Pin.IN, Pin.PULL_UP)
        self.down = 0
        self.pressed = self.released = 0
        self.hold_ms = self.tap_ms = 0

    def scan(self, now):
        curr = 1 if self.pin.value() == 0 else 0   # active-low: pressed reads 0
        if curr and not self.down:
            self.pressed = 1
            self.hold_ms = self.tap_ms = now
            self.down = 1
            return 1
        if not curr and self.down:
            self.released = 1
            self.down = 0
            return -1
        return 0

    def is_tap(self, now):
        return _time.ticks_diff(now, self.tap_ms) <= _TAP_MAX_MS

_keys = [Key(p) for p in _KEY_PINS]

# Triple-press MENU detection
_menu_press_count = 0
_menu_press_last  = 0
_menu_triple      = False
_TRIPLE_WINDOW_MS = 500

# 2s MENU-hold power-off: the scanner just latches this flag; main.py polls it
# (menu_power_off()) and owns the whole shutdown sequence. Keeps all the boot/
# power logic in one place instead of the scanner reaching back into main.
_menu_power_off = False

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

# Fire a user callback by name (e.g. "onPress"), printing (never raising) if it
# errors — a broken app callback must not kill the scan ISR. MENU callbacks
# (onMenu*) take no key argument; the others receive the key number.
def _fire(name, i):
    fn = globals().get(name)
    if fn:
        try:
            fn() if i == KEY_MENU else fn(i)
        except Exception as e:
            print(name + ":", e)

def _count_menu_triple(now):
    global _menu_press_count, _menu_press_last, _menu_triple
    if _time.ticks_diff(now, _menu_press_last) < _TRIPLE_WINDOW_MS:
        _menu_press_count += 1
    else:
        _menu_press_count = 1
    _menu_press_last = now
    if _menu_press_count >= 3:
        _menu_triple = True
        _menu_press_count = 0

def _scan_keys():
    now = _time.ticks_ms()
    for i in range(_KEY_COUNT):
        edge = _keys[i].scan(now)
        # MENU (key 0) uses the onMenu* callbacks and passes no key argument;
        # every other key uses on* and receives its number.
        if i == KEY_MENU:
            press, tap, release = "onMenuPress", "onMenuTap", "onMenuRelease"
            if edge == 1: _count_menu_triple(now)
        else:
            press, tap, release = "onPress", "onTap", "onRelease"
        if edge == 1:                          # press
            _fire(press, i)
        elif edge == -1:                       # release
            if _keys[i].is_tap(now): _fire(tap, i)
            _fire(release, i)
    # Power off: MENU held 2s → just latch a flag. main.py polls menu_power_off()
    # and runs the actual shutdown (which needs I2C to the EEPROM, unsafe from
    # inside tick()'s fast path).
    global _menu_power_off
    if hold(KEY_MENU, 2000):
        _menu_power_off = True

def start():
    """Called by main.py before running the app. There is no background timer any
    more — keys are scanned in tick() — so the only thing to do here is keep the
    hang watchdog disarmed across boot/import/setup (which can be slow) so it can't
    false-trip. The watchdog arms on the first tick() of the app's steady loop.
    main.py ticks a few times right after this to read keys held at boot."""
    _watchdog_disarm()

def stop():
    """Disarm the hang watchdog. main.py calls this before the power-off / System
    OFF sequence, which stops calling tick() while it cuts power — without this the
    watchdog could reboot the board mid-shutdown."""
    _watchdog_disarm()

# ── Scheduled callbacks ───────────────────────────────────────────────────────
_scheduled    = []
_last_tick_ms = _time.ticks_ms()

def delay(ms, fn):
    _scheduled.append([_time.ticks_add(_time.ticks_ms(), ms), fn])

def tick():
    """Scan keys, recomposite LEDs, fire scheduled callbacks, yield 10ms. True.

    This is the single cooperative yield point: every blocking loop must call it
    (directly or via wait()/waitPress()/...). Scanning lives here — there is no
    background timer — so a loop that never calls tick() stops scanning keys.
    The firmware watchdog (board.c VM hook) catches that case: if tick() isn't
    called for ~1s the script is deemed hung and the device reboots into MENU."""
    global _last_tick_ms
    _watchdog_feed()
    _scan_keys()
    now = _time.ticks_ms()
    i = 0
    while i < len(_scheduled):
        if _time.ticks_diff(now, _scheduled[i][0]) >= 0:
            _scheduled.pop(i)[1]()
        else:
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
# down/press/release share one shape: with no args, scan K1..K10; with args,
# check just those keys. `field` is the Key attribute to test ("down"/"pressed"/
# "released"); `consume` clears it (edge events fire once). The result is the key
# number, or True for MENU (key 0 is falsy, so a bare number wouldn't read true).
def _query(keys, field, consume):
    for k in keys if keys else range(1, _KEY_COUNT):
        if getattr(_keys[k], field):
            if consume: setattr(_keys[k], field, 0)
            return k or True
    return False

def down(*keys):
    """Key number currently held (True for MENU), or False."""
    return _query(keys, "down", False)

def press(*keys):
    """Key number on its press edge (True for MENU), consumed on read, or False."""
    return _query(keys, "pressed", True)

def release(*keys):
    """Key number on its release edge (True for MENU), consumed on read, or False."""
    return _query(keys, "released", True)

def hold(key, ms):
    """True while `key` has been held down for at least `ms` milliseconds."""
    k = _keys[key]
    return bool(k.down) and _time.ticks_diff(_time.ticks_ms(), k.hold_ms) >= ms

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

def menu_power_off():
    """True once MENU has been held 2s. main.py runs the shutdown. Internal."""
    return _menu_power_off

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
    'NB_LEDS', 'clearAll', 'setColor', 'flashColor', 'fadeColor', 'blink', 'color', 'lerp',
    'BLACK', 'WHITE', 'RED', 'GREEN', 'BLUE', 'YELLOW', 'CYAN', 'MAGENTA',
    'ORANGE', 'PURPLE', 'PINK',
    'KEY_MENU', 'KEY_K1', 'KEY_K10',
    'down', 'press', 'release', 'hold',
    'waitPress', 'waitRelease', 'waitUntil',
    'tick', 'delay', 'wait',
    'time', 'resetTime', 'restart',
    'hid_keys',
]

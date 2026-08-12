# Registers a Python object as sys.modules['machine'] before ksm.py is imported.
# This intercepts 'from machine import Pin, Timer' and 'import machine' in ksm.py,
# replacing the nRF hardware with browser-backed equivalents.
#
# Must be imported once before 'import ksm'. On each re-run, pop both
# '_ksm_patches' and 'machine' from sys.modules to reset Timer._active.

import sys
import _ksm_native as _nat
import time as _t

from pins import (PIN_MENU, PIN_K1, PIN_K2, PIN_K3, PIN_K4, PIN_K5,
                  PIN_K6, PIN_K7, PIN_K8, PIN_K9, PIN_K10)

_PIN_TO_KEY = {
    PIN_MENU: 0,
    PIN_K1: 1, PIN_K2: 2, PIN_K3: 3, PIN_K4: 4,  PIN_K5: 5,
    PIN_K6: 6, PIN_K7: 7, PIN_K8: 8, PIN_K9: 9,  PIN_K10: 10,
}


class _Machine:
    class Pin:
        IN = 0; OUT = 1; PULL_UP = 2; PULL_DOWN = 3

        def __init__(self, pin, mode=0, pull=0):
            self._key = _PIN_TO_KEY.get(pin, -1)

        def value(self, v=None):
            if v is not None: return
            if self._key < 0: return 0
            # Real keys are active-low (PULL_UP): pressed reads 0, released 1.
            # ksm._scan_keys treats value()==0 as pressed, so invert the host's
            # "is down" boolean to match the hardware the firmware expects.
            return 0 if _nat.get_key_down(self._key) else 1

    class Timer:
        ONE_SHOT = 0; PERIODIC = 1
        _active = []

        def __init__(self, id=-1, *, period=0, mode=0, callback=None):
            self._period_ms = period // 1000  # µs → ms
            self._cb = callback
            self._last = _t.ticks_ms()

        def start(self):
            _Machine.Timer._active.append(self)

        def deinit(self):
            if self in _Machine.Timer._active:
                _Machine.Timer._active.remove(self)

        @classmethod
        def _tick(cls):
            now = _t.ticks_ms()
            for t in list(cls._active):
                if _t.ticks_diff(now, t._last) >= t._period_ms:
                    t._last = now
                    if t._cb: t._cb(t)

    @staticmethod
    def reset(): pass

    @staticmethod
    def soft_reset(): pass


sys.modules['machine'] = _Machine


def tick():
    _Machine.Timer._tick()

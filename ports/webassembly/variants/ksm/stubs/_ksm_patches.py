# Registers a Python object as sys.modules['machine'] before ksm.py is imported.
# This intercepts 'from machine import Pin' and 'import machine' in ksm.py,
# replacing the nRF hardware with a browser-backed Pin (keys read from the host).
#
# Keys are scanned inside ksm.tick() (no background timer), so this stub only
# needs to provide Pin + machine.reset — the same shared ksm.py runs here and on
# the device. Under the restart model main.ts recreates the whole instance per
# Run, so there is no module state to reset between scripts.

import sys
import _ksm_native as _nat

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

    @staticmethod
    def reset(): pass

    @staticmethod
    def soft_reset(): pass


sys.modules['machine'] = _Machine

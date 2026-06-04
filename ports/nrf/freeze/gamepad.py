# gamepad.py — USB HID gamepad API for KSM1 (10 buttons, no axes)
#
# Usage:
#   import gamepad
#   gamepad.press(1, 3)      # hold buttons 1 and 3
#   gamepad.release(1, 3)    # release buttons 1 and 3
#   gamepad.click(2)         # press and release button 2
#   gamepad.release_all()    # release all buttons

try:
    from hid import hid_gamepad as _send
except ImportError:
    def _send(buttons): pass

from ksm import wait as _wait

_state = 0  # bitmask of currently held buttons (bit 0 = button 1, bit 9 = button 10)

def press(*buttons):
    """Hold one or more buttons (1–10)."""
    global _state
    for b in buttons:
        if 1 <= b <= 10:
            _state |= (1 << (b - 1))
    _send(_state)

def release(*buttons):
    """Release one or more buttons (1–10)."""
    global _state
    for b in buttons:
        if 1 <= b <= 10:
            _state &= ~(1 << (b - 1))
    _send(_state)

def release_all():
    """Release all buttons."""
    global _state
    _state = 0
    _send(0)

def click(*buttons, hold_ms=30):
    """Press and release one or more buttons."""
    press(*buttons)
    _wait(hold_ms)
    release(*buttons)
    _wait(20)

from machine import Pin, I2C
import time
import machine
import uos
import ksm

# ── Power ──────────────────────────────────────────────────────────────────────
Pin(25, Pin.OUT).value(1)
time.sleep_ms(500)

# ── EEPROM raw flags ───────────────────────────────────────────────────────────
# The first 2 EEPROM blocks (512 bytes) are reserved for raw flags.
# LittleFS starts at block 2 (byte 0x0200) via OffsetBlockDev.
#
# Raw layout:
#   0x0000 : POWER flag  (0xDE = off, 0x00 = on)
#   0x0001 : MODE flag   (0x00 = MENU mode, 0x01 = USER mode)

EEPROM_ADDR       = 0x50
EEPROM_POWER_ADDR = 0x0000
EEPROM_MODE_ADDR  = 0x0001
EEPROM_POWER_OFF  = 0xDE
EEPROM_MODE_MENU  = 0x00
EEPROM_MODE_USER  = 0x01
EEPROM_PAGE_SIZE  = 64

def _i2c():
    return I2C(0, scl=Pin(11), sda=Pin(4))

def _eeprom_read_raw(addr, n=1):
    i2c = _i2c()
    i2c.writeto(EEPROM_ADDR, bytes([addr >> 8, addr & 0xFF]))
    return i2c.readfrom(EEPROM_ADDR, n)

def _eeprom_write_raw(addr, data):
    """Write up to 64 bytes, page-aligned. Waits for write cycle."""
    i2c = _i2c()
    i2c.writeto(EEPROM_ADDR, bytes([addr >> 8, addr & 0xFF]) + bytes(data))
    time.sleep_ms(10)

def _eeprom_read_mode():
    try:
        return _eeprom_read_raw(EEPROM_MODE_ADDR)[0]
    except Exception:
        return EEPROM_MODE_USER  # safe default

def _eeprom_write_mode(mode):
    _eeprom_write_raw(EEPROM_MODE_ADDR, bytes([mode]))

# ── Sleep loop ─────────────────────────────────────────────────────────────────
# Entered when POWER flag is 0xDE. Board appears "off".
# MENU press → clears flag → resets → normal boot.
# Note: ksm.start() has NOT been called yet — no Timer running here.

def _sleep_loop():
    ksm.clear_all()
    menu = Pin(42, Pin.IN, Pin.PULL_UP)
    prev = menu.value()
    while True:
        curr = menu.value()
        if curr == 0 and prev == 1:
            time.sleep_ms(50)  # debounce
            if menu.value() == 0:
                _eeprom_write_raw(EEPROM_POWER_ADDR, bytes([0x00]))
                machine.reset()
        prev = curr
        time.sleep_ms(20)

try:
    if _eeprom_read_raw(EEPROM_POWER_ADDR)[0] == EEPROM_POWER_OFF:
        _sleep_loop()
except Exception:
    pass

# ── Start key scanner ──────────────────────────────────────────────────────────
# Only called after the sleep check — the Timer must not run during _sleep_loop
# because it would trigger a reset on 2s MENU hold, keeping the board in sleep.
ksm.start()

# /eeprom is mounted by _boot.py on every boot/soft-reset, before main.py runs.

# ── Boot animation + safety window ────────────────────────────────────────────
# The boot animation (~480ms) and safety window (~1500ms) together give ~2s
# to triple-press MENU before the user script loads.
# This is the only way to escape a broken script (crash-on-boot, infinite loop).
for _ in range(3):
    for i in range(ksm.NB_LEDS):
        ksm.np[i] = (0, 255, 0)
    ksm.np.write()
    time.sleep_ms(80)
    ksm.clear_all()
    time.sleep_ms(80)

# Safety window: dim blue pulse, triple-press MENU → force MENU mode
_deadline = time.ticks_ms() + 1500
while time.ticks_diff(_deadline, time.ticks_ms()) > 0:
    _rem    = time.ticks_diff(_deadline, time.ticks_ms())
    _bright = _rem * 20 // 1500       # fades from 20 to 0 as deadline approaches
    ksm.np[0] = (0, 0, _bright)
    ksm.np.write()
    time.sleep_ms(50)
    if ksm.menu_triple_press():
        ksm.clear_all()
        print("Safety: switching to MENU mode.")
        _eeprom_write_mode(EEPROM_MODE_MENU)
        machine.reset()
ksm.clear_all()

# ── Mode check ─────────────────────────────────────────────────────────────────
# MODE determines what runs after boot:
#   MENU mode (0x00): no user script, shows a standby pattern.
#                     Triple-press MENU → switch to USER mode.
#   USER mode (0x01): loads /eeprom/app.py and runs it.
#                     Triple-press MENU → switch to MENU mode.
#
# To switch mode: triple-press MENU quickly (<500ms between presses).
# To upload a new script: mpremote connect <PORT> cp app.py :/eeprom/app.py

_mode = _eeprom_read_mode()

if _mode == EEPROM_MODE_MENU:
    print("MENU mode. Triple-press MENU to load user script.")
    _t = 0
    while True:
        # Slow orange pulse on LED 0 to signal standby
        _t = (_t + 1) % 20
        ksm.np[0] = (20, 8, 0) if _t < 10 else (0, 0, 0)
        ksm.np.write()
        time.sleep_ms(50)
        if ksm.menu_triple_press():
            print("Switching to USER mode...")
            _eeprom_write_mode(EEPROM_MODE_USER)
            machine.reset()

# ── USER mode: load and run /eeprom/app.py ────────────────────────────────────
# exec() runs the script in an isolated namespace.
# The script accesses the KSM API via: from ksm import *

_setup = None
_loop  = None

try:
    _src = open('/eeprom/app.py').read()
    _ns  = {}
    exec(_src, _ns)
    _setup = _ns.get('setup')
    _loop  = _ns.get('loop')
except OSError:
    print("No app.py on /eeprom/. Upload one with:")
    print("  mpremote connect <PORT> cp app.py :/eeprom/app.py")
except Exception as e:
    print("app load error:", e)

if _setup:
    try:
        _setup()
    except Exception as e:
        print("setup() error:", e)

# ── Main loop ──────────────────────────────────────────────────────────────────
# Pre-write POWER flag now. If MENU is held 2s (in Timer callback),
# machine.reset() fires and the next boot finds the flag → enters _sleep_loop().
# On clean wake (MENU press in _sleep_loop), flag is cleared before rebooting.
_eeprom_write_raw(EEPROM_POWER_ADDR, bytes([EEPROM_POWER_OFF]))

print("Running." if _loop else "Waiting for app.")

while True:
    if _loop:
        try:
            _loop()
        except Exception as e:
            print("loop() error:", e)
            _loop = None  # stop calling after crash

    # Triple-press MENU → switch to MENU mode
    if ksm.menu_triple_press():
        print("Switching to MENU mode...")
        _eeprom_write_mode(EEPROM_MODE_MENU)
        machine.reset()

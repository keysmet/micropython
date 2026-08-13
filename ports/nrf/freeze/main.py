from machine import Pin, I2C
import time
import machine
import ksm

# ── Power ──────────────────────────────────────────────────────────────────────
Pin(ksm.PIN_PWR_ON, Pin.OUT).value(1)
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
EEPROM_POWER_OFF  = 0xDE  # arbitrary sentinel: non-zero (≠ on) and non-0xFF (≠ blank EEPROM)
EEPROM_MODE_MENU  = 0x00
EEPROM_MODE_USER  = 0x01

_i2c = I2C(0, scl=Pin(ksm.PIN_I2C_SCL), sda=Pin(ksm.PIN_I2C_SDA))

def _eeprom_read_raw(addr, n=1):
    _i2c.writeto(EEPROM_ADDR, bytes([addr >> 8, addr & 0xFF]))
    return _i2c.readfrom(EEPROM_ADDR, n)

def _eeprom_write_raw(addr, data):
    _i2c.writeto(EEPROM_ADDR, bytes([addr >> 8, addr & 0xFF]) + bytes(data))
    time.sleep_ms(10)

def _eeprom_read_mode():
    return _eeprom_read_raw(EEPROM_MODE_ADDR)[0]

def _eeprom_write_mode(mode):
    _eeprom_write_raw(EEPROM_MODE_ADDR, bytes([mode]))

# ── Sleep loop ─────────────────────────────────────────────────────────────────
# Entered when POWER flag is 0xDE. Board appears "off".
# MENU press → clears flag → resets → normal boot.
# Note: ksm.start() has NOT been called yet — no Timer running here.

def _sleep_loop():
    # nRF52840 register addresses for System OFF (same names as nRF5 SDK)
    NRF_P1_PIN_CNF_MENU       = 0x50000A28  # GPIO P1 PIN_CNF[10] — config for P1.10 (MENU)
    NRF_P1_LATCH              = 0x50000820  # GPIO P1 LATCH — clear to avoid stale DETECT
    NRF_POWER_SYSTEMOFF       = 0x40000500  # POWER.SYSTEMOFF — write 1 to enter System OFF
    NRF_PIN_CNF_PULLUP_SENSE_LOW = 0x0003000C  # bits[3:2]=11 (pull-up), bits[17:16]=11 (sense low)

    ksm.clearAll()
    menu = Pin(ksm.PIN_MENU, Pin.IN, Pin.PULL_UP)

    # ── Wake-up path ───────────────────────────────────────────────────────────
    # After System OFF, the board does a cold boot. main.py sees POWER=0xDE and
    # enters here. If MENU is already pressed, we just woke from sleep.
    # Require a 2s hold to confirm — show a green progress bar on the LEDs.
    # If released before 2s, fall through and go back to System OFF.
    if menu.value() == 0:
        _WAKE_HOLD_MS = 2000
        start = time.ticks_ms()
        while menu.value() == 0:
            elapsed = time.ticks_diff(time.ticks_ms(), start)
            if elapsed >= _WAKE_HOLD_MS:
                ksm.clearAll()
                _eeprom_write_raw(EEPROM_POWER_ADDR, bytes([0x00]))
                machine.reset()
            n = elapsed * ksm.NB_LEDS // _WAKE_HOLD_MS
            for i in range(ksm.NB_LEDS):
                ksm.np[i] = (0, 20, 0) if i < n else (0, 0, 0)
            ksm.np.write()
            time.sleep_ms(20)
        ksm.clearAll()  # released before 2s — go back to sleep

    # ── System OFF ─────────────────────────────────────────────────────────────
    # Configure MENU pin (P1.10) to wake the chip on low level.
    machine.mem32[NRF_P1_PIN_CNF_MENU] = NRF_PIN_CNF_PULLUP_SENSE_LOW
    # Clear port-1 LATCH — stale DETECT can prevent entering System OFF
    machine.mem32[NRF_P1_LATCH] = 0xFFFFFFFF
    # Release I2C pull-ups before cutting power — prevents overvoltage into
    # EEPROM/IMU input pins (spec: max VCC+0.3V; VCC=0 after power cut).
    Pin(ksm.PIN_I2C_SDA, Pin.IN)
    Pin(ksm.PIN_I2C_SCL, Pin.IN)
    # Cut external power (EEPROM, IMU) before sleeping
    Pin(ksm.PIN_PWR_ON, Pin.OUT).value(0)
    # Enter System OFF (~0.4µA). Cold boot on MENU press. Never returns.
    machine.mem32[NRF_POWER_SYSTEMOFF] = 1

_usb = bool(machine.mem32[0x40000438] & 1)  # nRF POWER.USBREGSTATUS.VBUSDETECT

# Enter sleep if the flag was written by the MENU 2s hold (battery or USB).
# On upload, the flag is never pre-armed so this is always skipped.
if _eeprom_read_raw(EEPROM_POWER_ADDR)[0] == EEPROM_POWER_OFF:
    _sleep_loop()

# ── Start key scanner ──────────────────────────────────────────────────────────
# Only called after the sleep check — the Timer must not run during _sleep_loop
# because it would trigger a reset on 2s MENU hold, keeping the board in sleep.
ksm.start()

# Register the sleep flag write so it fires only on the MENU 2s hold reset,
# not on upload soft resets or any other reset.
ksm._pre_reset_hooks.append(
    lambda: _eeprom_write_raw(EEPROM_POWER_ADDR, bytes([EEPROM_POWER_OFF]))
)

# /eeprom is mounted by _boot.py on every boot/soft-reset, before main.py runs.

# ── Boot animation + safety window ────────────────────────────────────────────
# Skipped when USB is connected — the user can recover broken scripts via USB.
# On battery: run the full sequence so triple-press MENU can force MENU mode.
if not _usb:
    for _ in range(3):
        for i in range(ksm.NB_LEDS):
            ksm.np[i] = (0, 255, 0)
        ksm.np.write()
        time.sleep_ms(80)
        ksm.clearAll()
        time.sleep_ms(80)

    # Safety window: dim blue pulse, triple-press MENU → force MENU mode
    _deadline = time.ticks_ms() + 1500
    while time.ticks_diff(_deadline, time.ticks_ms()) > 0:
        _rem    = time.ticks_diff(_deadline, time.ticks_ms())
        _bright = _rem * 20 // 1500
        ksm.np[0] = (0, 0, _bright)
        ksm.np.write()
        time.sleep_ms(50)
        if ksm.menu_triple_press():
            ksm.clearAll()
            print("Safety: switching to MENU mode.")
            _eeprom_write_mode(EEPROM_MODE_MENU)
            machine.reset()

ksm.clearAll()

# ── Mode check ─────────────────────────────────────────────────────────────────
# MODE determines what runs after boot:
#   MENU mode (0x00): no user script, shows a standby pattern.
#                     Single-press MENU → switch to USER mode.
#   USER mode (0x01): loads /eeprom/app.py and runs it.
#                     Triple-press MENU → switch to MENU mode.
#
# MENU→USER: single press (safe — no script running to interrupt).
# USER→MENU: triple-press quickly (<500ms between presses).
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
        if ksm.press(ksm.KEY_MENU):
            print("Switching to USER mode...")
            _eeprom_write_mode(EEPROM_MODE_USER)
            machine.reset()

# ── USER mode: load and run /eeprom/app.py ────────────────────────────────────
# exec() runs the script in an isolated namespace.
# The script accesses the KSM API via: from ksm import *

_setup = None
_loop  = None

_CALLBACKS = ('onPress', 'onRelease', 'onTap', 'onUpdate',
              'onMenuPress', 'onMenuRelease', 'onMenuTap')

def _load_app():
    global _setup, _loop
    try:
        _ns = {}
        with open('/eeprom/app.py') as _f:
            exec(_f.read(), _ns)
        _setup = _ns.get('setup')
        _loop  = _ns.get('loop')
        for cb in _CALLBACKS:
            if cb in _ns:
                setattr(ksm, cb, _ns[cb])
    except OSError:
        print("No app.py on /eeprom/. Upload one with:")
        print("  mpremote connect <PORT> cp app.py :/eeprom/app.py")
    except Exception as e:
        print("app load error:", e)

_load_app()

if _setup:
    try:
        _setup()
    except Exception as e:
        print("setup() error:", e)

# ── Main loop ──────────────────────────────────────────────────────────────────
# A new script is loaded by rebooting into it, not by reloading in place: the
# uploader writes /eeprom/app.py then resets the board, so this loop only ever
# runs one script from a clean boot. (No mtime file-watcher / in-place re-exec —
# that path had to tear down timers and module state and was a source of bugs.)
print("Running." if _loop else "Waiting for app.")

while True:
    if _loop:
        try:
            _loop()
        except Exception as e:
            print("loop() error:", e)
            _loop = None  # stop calling after crash

    ksm.tick()  # fire after() callbacks + 10ms yield

    # Triple-press MENU → switch to MENU mode
    if ksm.menu_triple_press():
        print("Switching to MENU mode...")
        _eeprom_write_mode(EEPROM_MODE_MENU)
        _eeprom_write_raw(EEPROM_POWER_ADDR, bytes([0x00]))  # don't enter sleep on reboot
        machine.reset()

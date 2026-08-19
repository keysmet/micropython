from machine import Pin, I2C
import time
import machine
import ksm

# main.py is DEVICE-ONLY — the WASM sim runs ksm.py but never this file. So all
# the hardware boot/power/EEPROM/System-OFF logic lives here, kept simple; ksm.py
# stays clean and shared. (The scanner only *latches* the 2s-hold; the actual
# shutdown is owned here.)

# ── Wake latch (EEPROM byte) ─────────────────────────────────────────────────
# One reserved EEPROM byte remembers "we are asleep" across System OFF, instead
# of GPREGRET — reading NRF_POWER->GPREGRET via machine.mem32 hard-faults on this
# board (resets the chip in a ~1s loop), so we avoid that register. The EEPROM is
# on the always-on VDD rail, so the byte survives System OFF like a retention
# register. LittleFS starts at block 2 (byte 0x200); bytes below are reserved, so
# byte 0x0000 is ours.
#   SLEEPING (0xA3) — set by _system_off() before sleeping; this boot is a wake.
#   MENU     (0xA2) — triple-press exit asked for MENU mode; don't auto-run.
#   anything else   — a normal boot (fresh power, reset, crash) → auto-run.
# Power-off is decided live (2s hold), not latched.
EEPROM_ADDR        = 0x50
INTENT_ADDR        = 0x0000
INTENT_SLEEPING    = 0xA3
INTENT_MENU        = 0xA2

_i2c = I2C(0, scl=Pin(ksm.PIN_I2C_SCL), sda=Pin(ksm.PIN_I2C_SDA))

def _intent_read():
    _i2c.writeto(EEPROM_ADDR, bytes([INTENT_ADDR >> 8, INTENT_ADDR & 0xFF]))
    return _i2c.readfrom(EEPROM_ADDR, 1)[0]

def _intent_write(v):
    _i2c.writeto(EEPROM_ADDR, bytes([INTENT_ADDR >> 8, INTENT_ADDR & 0xFF, v]))
    time.sleep_ms(10)   # EEPROM write cycle

_intent = _intent_read()
_intent_write(0x00)     # clear immediately — a crash/reset loop can't replay it

# ── System OFF ───────────────────────────────────────────────────────────────
# Cuts the switched rail (LEDs/audio/IMU via PWR_ON) and puts the nRF in System
# OFF (~µA). EEPROM and the nRF core stay on the always-on VDD rail. Wakes on a
# MENU press, which cold-boots back into main.py. Never returns.
def _system_off():
    NRF_P1_PIN_CNF_MENU          = 0x50000A28  # P1 PIN_CNF[10] — MENU
    NRF_P1_LATCH                 = 0x50000820  # P1 LATCH — clear stale DETECT
    NRF_POWER_SYSTEMOFF          = 0x40000500  # POWER.SYSTEMOFF
    NRF_PIN_CNF_PULLUP_SENSE_LOW = 0x0003000C  # pull-up + sense low

    # Latch SLEEPING first (EEPROM write needs I2C, done before we release it).
    _intent_write(INTENT_SLEEPING)
    ksm.clearAll()
    Pin(ksm.PIN_PWR_LED, Pin.OUT).value(0)  # power LED off (it's on the always-on
                                            # rail, so System OFF won't clear it)
    # Wake on MENU low.
    machine.mem32[NRF_P1_PIN_CNF_MENU] = NRF_PIN_CNF_PULLUP_SENSE_LOW
    machine.mem32[NRF_P1_LATCH] = 0xFFFFFFFF
    # Release I2C pull-ups before cutting the rail — prevents back-powering the
    # EEPROM/IMU inputs above their VCC while the switched rail drops.
    Pin(ksm.PIN_I2C_SDA, Pin.IN)
    Pin(ksm.PIN_I2C_SCL, Pin.IN)
    Pin(ksm.PIN_PWR_ON, Pin.OUT).value(0)   # cut LEDs/audio/IMU rail (not EEPROM)
    machine.mem32[NRF_POWER_SYSTEMOFF] = 1  # never returns

# ── Power-down sequence (2s MENU hold), owned here ───────────────────────────
# Red flash across all keys as "powering off" feedback (like the Arduino), wait
# for MENU release, then go straight to System OFF — no reboot. Sleeping directly
# avoids the boot window where the DAC/amp are powered but undriven (the audible
# glitch). The scan timer is stopped first so its ISR can't run while we cut power.
def _power_off():
    for i in range(ksm.NB_LEDS):
        ksm.setColor(i, ksm.RED)
    ksm.tick()
    time.sleep_ms(500)
    ksm.clearAll()
    ksm.tick()
    # Wait for MENU release using the scanner's own state (like the Arduino) — do
    # NOT create a second Pin on MENU; the scan timer already owns that GPIO.
    while ksm.down(ksm.KEY_MENU):
        time.sleep_ms(20)
    ksm.stop()          # halt the scan ISR before touching pins / cutting power
    _system_off()       # never returns

# ── Wake confirmation ────────────────────────────────────────────────────────
# Only a wake from System OFF reaches here (intent == SLEEPING). The MENU press
# that woke us is still held; require a full 1s hold to confirm. Released early →
# straight back to System OFF. Power LED on during the confirm; a short blue
# flash (like the Arduino) once we commit to booting.
if _intent == INTENT_SLEEPING:
    _pwr_led = Pin(ksm.PIN_PWR_LED, Pin.OUT)
    _pwr_led.value(1)
    _menu = Pin(ksm.PIN_MENU, Pin.IN, Pin.PULL_UP)
    _HOLD_MS = 1000
    _start = time.ticks_ms()
    while _menu.value() == 0:
        if time.ticks_diff(time.ticks_ms(), _start) >= _HOLD_MS:
            break
        time.sleep_ms(20)
    else:
        _system_off()                             # released before the hold
    # Confirmed: 3× blue flash as the "powering on" cue.
    for _ in range(3):
        for i in range(ksm.NB_LEDS):
            ksm.setColor(i, 0x000030)
        ksm.tick()
        time.sleep_ms(50)
        ksm.clearAll()
        time.sleep_ms(50)

# ── Start the cooperative key scanner ────────────────────────────────────────
ksm.start()

# ── Decide: user script or MENU mode ─────────────────────────────────────────
# By default boot straight into the user script if there is one. Force MENU mode
# instead when either: a K1..K10 is held at boot (recovery / upload), or the last
# reset asked for it (triple-press exit set INTENT_MENU). MENU is NOT a key
# trigger — it's the power/wake button, naturally held right after waking.
# User Python errors are caught (try/except below), so a buggy script can't trap
# the board; you can still triple-press MENU or 2s-hold to power off.
_has_app = False
try:
    open('/eeprom/app.py').close()
    _has_app = True
except OSError:
    pass

# Any K1..K10 held at boot? Give the scan timer a moment to read the pins, then
# ask the ksm API.
time.sleep_ms(30)
_key_held = any(ksm.down(k) for k in range(1, 11))
_run_user = _has_app and not _key_held and _intent != INTENT_MENU

# ── USER mode: run /eeprom/app.py ────────────────────────────────────────────
_CALLBACKS = ('onPress', 'onRelease', 'onTap', 'onUpdate',
              'onMenuPress', 'onMenuRelease', 'onMenuTap')

if _run_user:
    _setup = None
    _loop = None
    try:
        _ns = {}
        with open('/eeprom/app.py') as _f:
            exec(_f.read(), _ns)
        _setup = _ns.get('setup')
        _loop = _ns.get('loop')
        for _cb in _CALLBACKS:
            if _cb in _ns:
                setattr(ksm, _cb, _ns[_cb])
    except Exception as e:
        print("app load error:", e)

    if _setup:
        try:
            _setup()
        except Exception as e:
            print("setup() error:", e)

    print("Running." if _loop else "Waiting for app.")
    while True:
        if _loop:
            try:
                _loop()
            except Exception as e:
                print("loop() error:", e)
                _loop = None            # stop calling after a crash
        ksm.tick()                      # scheduled callbacks + onUpdate + 10ms yield
        if ksm.menu_power_off():        # MENU held 2s → power off
            _power_off()
        if ksm.menu_triple_press():     # triple-press MENU → back to MENU mode
            print("Exiting to MENU.")
            _intent_write(INTENT_MENU)  # so the reset lands in MENU, not auto-run
            machine.reset()

# ── MENU mode: idle until a key launches the user script ──────────────────────
# Reached when there's no app, or a key was held at boot to force MENU. Pressing
# a key launches the script — via a keyless reboot (a plain reset boots straight
# into the script now), so we wait for release first, else the boot-time
# key-held check would just bounce back to MENU.
print("MENU mode. Press any key to run your script." if _has_app
      else "MENU mode. No app.py — upload one to /eeprom/app.py.")

# If a key was held at boot to reach MENU, wait for it to be released and clear
# any pending press edges, so letting go doesn't immediately launch the script.
while ksm.down():
    ksm.wait(20)
ksm.wait(50)                            # let the release edge settle
while ksm.press():                      # drain any latched press edges
    pass

_t = 0
while True:
    _t = (_t + 1) % 20
    ksm.setColor(ksm.KEY_MENU, 0x140800 if _t < 10 else 0)   # slow orange standby pulse
    ksm.wait(50)                        # wait() runs tick(), which flushes LEDs
    if ksm.menu_power_off():            # MENU held 2s → power off
        _power_off()
    if _has_app and ksm.press():        # any K1..K10
        print("Launching user script...")
        while ksm.down():               # wait for all keys released
            ksm.wait(20)
        machine.reset()                 # keyless reboot → auto-runs the script

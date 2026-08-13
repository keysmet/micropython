from machine import Pin
import time
import machine
import ksm

# ── Boot intent latch (GPREGRET) ─────────────────────────────────────────────
# GPREGRET is a retained register: it survives reset AND System OFF wake, and
# the Adafruit bootloader passes it through (unless it is the 0x57 UF2 magic).
# It is the ONLY thing main.py reads to decide what kind of boot this is:
#   RUN_USER  — MENU mode launched a script
#   POWER_OFF — board.c saw the 2s MENU hold; go to System OFF
#   SLEEPING  — set by _system_off() just before sleeping; means "this boot is a
#               wake from System OFF", so run the hold-to-confirm gate
#   0 / other — a normal boot (fresh power, reset, crash, triple-press exit) → MENU
# We read it once, clear it immediately (so a crash/reset loop can't repeat a
# stuck boot), then act. No other signal (MENU pin, RESETREAS) gates the flow —
# a boot with MENU held only matters when GPREGRET says we were SLEEPING.
NRF_POWER_GPREGRET = 0x4000051C
GPREGRET_RUN_USER  = 0xA1
GPREGRET_POWER_OFF = 0xA2
GPREGRET_SLEEPING  = 0xA3

_intent = machine.mem32[NRF_POWER_GPREGRET]
machine.mem32[NRF_POWER_GPREGRET] = 0

# /eeprom is mounted by _boot.py before main.py runs, on every boot/soft-reset,
# and it also drives PWR_ON high — so the power rail and EEPROM are already up.

# ── System OFF ───────────────────────────────────────────────────────────────
# Cuts the switched rail (LEDs/audio/IMU via PWR_ON) and puts the nRF in System
# OFF (~µA). EEPROM and the nRF core stay on the always-on VDD rail. Wakes on a
# MENU press, which cold-boots back into main.py. Never returns.

def _system_off():
    NRF_P1_PIN_CNF_MENU          = 0x50000A28  # P1 PIN_CNF[10] — MENU
    NRF_P1_LATCH                 = 0x50000820  # P1 LATCH — clear stale DETECT
    NRF_POWER_SYSTEMOFF          = 0x40000500  # POWER.SYSTEMOFF
    NRF_PIN_CNF_PULLUP_SENSE_LOW = 0x0003000C  # pull-up + sense low

    ksm.clearAll()
    # Wake on MENU low.
    machine.mem32[NRF_P1_PIN_CNF_MENU] = NRF_PIN_CNF_PULLUP_SENSE_LOW
    machine.mem32[NRF_P1_LATCH] = 0xFFFFFFFF
    # Release I2C pull-ups before cutting the rail — prevents back-powering the
    # EEPROM/IMU inputs above their VCC while the switched rail drops.
    Pin(ksm.PIN_I2C_SDA, Pin.IN)
    Pin(ksm.PIN_I2C_SCL, Pin.IN)
    Pin(ksm.PIN_PWR_ON, Pin.OUT).value(0)   # cut LEDs/audio/IMU rail (not EEPROM)
    # Latch SLEEPING so the next boot (a wake) runs the hold-to-confirm gate.
    # Survives System OFF; cleared by main.py on that next boot.
    machine.mem32[NRF_POWER_GPREGRET] = GPREGRET_SLEEPING
    machine.mem32[NRF_POWER_SYSTEMOFF] = 1  # never returns

# ── Power-off request from the board (2s MENU hold) ──────────────────────────
if _intent == GPREGRET_POWER_OFF:
    _system_off()

# ── Wake confirmation ────────────────────────────────────────────────────────
# Only a wake from System OFF reaches here (GPREGRET == SLEEPING, set by
# _system_off before it slept). The MENU press that woke us is still held;
# require a full 1s hold to confirm — a brush against MENU in a bag shouldn't
# power the board up. Released too early → straight back to System OFF.
# Silent (no LEDs) while confirming. Every other boot (fresh power, reset, crash,
# triple-press exit, launch with MENU held) has a different intent and skips this
# entirely — the MENU pin is never consulted, so nothing here fires on a non-wake.
if _intent == GPREGRET_SLEEPING:
    _menu = Pin(ksm.PIN_MENU, Pin.IN, Pin.PULL_UP)
    _HOLD_MS = 1000
    _start = time.ticks_ms()
    while _menu.value() == 0:
        if time.ticks_diff(time.ticks_ms(), _start) >= _HOLD_MS:
            break
        time.sleep_ms(20)
    else:
        _system_off()                             # released before the hold

# ── Start the cooperative key scanner ────────────────────────────────────────
ksm.start()

# ── USER mode: run /eeprom/app.py once (RUN_USER intent) ──────────────────────
# Reached only when MENU mode launched the script. Any other boot — fresh power,
# crash, watchdog, soft reset — has no RUN_USER flag and falls through to MENU,
# so a bad script can never trap the board: it runs once, and you return to MENU.
_CALLBACKS = ('onPress', 'onRelease', 'onTap', 'onUpdate',
              'onMenuPress', 'onMenuRelease', 'onMenuTap')

if _intent == GPREGRET_RUN_USER:
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
        if ksm.menu_triple_press():     # triple-press MENU → back to MENU mode
            print("Exiting to MENU.")
            machine.reset()

# ── MENU mode: idle until a key launches the user script ──────────────────────
# Any key press loads the user script (via a clean reboot into RUN_USER). If
# there is no script on the board, a press does nothing. USB/REPL stays live the
# whole time, so uploads and recovery always work here.
_has_app = False
try:
    open('/eeprom/app.py').close()
    _has_app = True
except OSError:
    pass

print("MENU mode. Press any key to run your script." if _has_app
      else "MENU mode. No app.py — upload one to /eeprom/app.py.")

_t = 0
while True:
    _t = (_t + 1) % 20
    ksm.setColor(ksm.KEY_MENU, 0x140800 if _t < 10 else 0)   # slow orange standby pulse
    ksm.wait(50)                        # wait() runs tick(), which flushes LEDs
    if _has_app and ksm.press():        # any K1..K10
        print("Launching user script...")
        machine.mem32[NRF_POWER_GPREGRET] = GPREGRET_RUN_USER
        machine.reset()

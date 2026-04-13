from machine import Pin, I2C
from neopixel import NeoPixel
import time
import machine

# Power latch — must be driven HIGH immediately or board cuts power
Pin(25, Pin.OUT).value(1)

# Delay to let the Adafruit bootloader detect a double-tap reset.
# Without this, MicroPython takes over USB too quickly and the bootloader
# never gets a chance to enumerate as a UF2 drive.
time.sleep_ms(500)

# ── EEPROM shutdown flag ──────────────────────────────────────────────────────
# On shutdown, Python writes EEPROM_FLAG_OFF and calls machine.reset().
# On next boot, this code reads the flag and enters sleep loop if set.
# MENU button press clears the flag and resets into normal boot.

EEPROM_I2C_ADDR  = 0x50
EEPROM_FLAG_ADDR = 0x0000
EEPROM_FLAG_OFF  = 0xDE

def _eeprom_read(addr):
    i2c = I2C(0, scl=Pin(11), sda=Pin(4))
    i2c.writeto(EEPROM_I2C_ADDR, bytes([addr >> 8, addr & 0xFF]))
    return i2c.readfrom(EEPROM_I2C_ADDR, 1)[0]

def _eeprom_write(addr, val):
    i2c = I2C(0, scl=Pin(11), sda=Pin(4))
    i2c.writeto(EEPROM_I2C_ADDR, bytes([addr >> 8, addr & 0xFF, val]))
    time.sleep_ms(10)  # M24256 write cycle ~5ms

def _sleep_loop():
    menu = Pin(42, Pin.IN, Pin.PULL_UP)  # MENU = P1.10, active low
    prev = menu.value()
    while True:
        curr = menu.value()
        if curr == 0 and prev == 1:  # falling edge
            time.sleep_ms(50)        # debounce
            if menu.value() == 0:
                _eeprom_write(EEPROM_FLAG_ADDR, 0x00)
                machine.reset()
        prev = curr
        time.sleep_ms(20)

try:
    if _eeprom_read(EEPROM_FLAG_ADDR) == EEPROM_FLAG_OFF:
        _sleep_loop()  # never returns
except Exception:
    pass  # I2C error — assume normal boot

# ── Boot animation ────────────────────────────────────────────────────────────
np = NeoPixel(Pin(0), 11)  # PIN_LED = P0.00
for _ in range(3):
    for i in range(11):
        np[i] = (0, 255, 0)  # green
    np.write()
    time.sleep_ms(80)
    for i in range(11):
        np[i] = (0, 0, 0)
    np.write()
    time.sleep_ms(80)

# ── Run user app ──────────────────────────────────────────────────────────────
# Copy app.py to the device with: python micropython/upload.py COM9
try:
    import app
except ImportError:
    pass

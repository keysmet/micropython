# pins.py — KSM1 pin numbers
# Single source of truth — matches variant.h in keysmet-arduino.
# No imports, no objects: safe to import from _boot.py at early boot.

PIN_PWR_ON  = 25  # P0.25 — external power rail (EEPROM, IMU)
PIN_LED     = 0   # P0.00 — NeoPixel data (chain: MENU_LED → LED10 → … → LED1)
PIN_PWR_LED = 7   # P0.07 — power/charging indicator LED (plain GPIO)
PIN_I2C_SCL = 11  # P0.11
PIN_I2C_SDA = 4   # P0.04
PIN_MENU    = 42  # P1.10
PIN_K1      = 22  # P0.22
PIN_K2      = 17  # P0.17
PIN_K3      = 13  # P0.13
PIN_K4      = 8   # P0.08
PIN_K5      = 6   # P0.06
PIN_K6      = 24  # P0.24
PIN_K7      = 20  # P0.20
PIN_K8      = 15  # P0.15
PIN_K9      = 41  # P1.09
PIN_K10     = 27  # P0.27

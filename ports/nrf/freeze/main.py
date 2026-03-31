from machine import Pin
import time

led = Pin(7, Pin.OUT)  # PWR_LED = P0.07

for _ in range(6):
    led.value(1)
    time.sleep_ms(100)
    led.value(0)
    time.sleep_ms(100)

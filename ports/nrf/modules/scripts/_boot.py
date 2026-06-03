def setup_eeprom_fs():
    # Mount the M24256 EEPROM as LittleFS at /eeprom.
    # Runs on every soft reset (same as setup_fs for /flash) so the VFS
    # is available to mpremote cp and Thonny after Ctrl+C interrupts main.py.
    #
    # PWR_ON (P25) must be driven high before any I2C access — on hard reset
    # the GPIO is in default input state so the power rail is off.
    # On soft reset the pin is already high (GPIO state preserved), so the
    # value(1) call is a no-op and no extra delay is needed.
    try:
        import vfs
        import sys
        import os
        import time
        from machine import I2C, Pin
        from m24256 import M24256, OffsetBlockDev

        from pins import PIN_PWR_ON, PIN_I2C_SCL, PIN_I2C_SDA

        pwr = Pin(PIN_PWR_ON, Pin.OUT)
        if not pwr.value():
            pwr.value(1)
            time.sleep_ms(100)  # EEPROM power-on settling time

        bdev = M24256(I2C(0, scl=Pin(PIN_I2C_SCL), sda=Pin(PIN_I2C_SDA)))
        fs   = OffsetBlockDev(bdev, start_block=2)
        try:
            vfs.mount(vfs.VfsLfs2(fs), "/eeprom")
        except OSError:
            vfs.VfsLfs2.mkfs(fs)
            vfs.mount(vfs.VfsLfs2(fs), "/eeprom")

        sys.path.append("/eeprom")
        sys.path.append("/eeprom/lib")
        os.chdir("/eeprom")
    except Exception as e:
        print("EEPROM mount failed:", e)


setup_eeprom_fs()
del setup_eeprom_fs

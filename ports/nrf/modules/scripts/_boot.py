def setup_fs():
    import gc
    import vfs
    import sys
    import nrf
    import os

    fs_type = getattr(vfs, "VfsLfs2", getattr(vfs, "VfsLfs1", getattr(vfs, "VfsFat", None)))
    try:
        bdev = nrf.Flash()
        vfs.mount(bdev, "/flash")
    except OSError:
        if fs_type is not None:
            try:
                fs_type.mkfs(bdev)
                vfs.mount(bdev, "/flash")
            except OSError:
                return

    os.chdir("/flash")
    sys.path.append("/flash")
    sys.path.append("/flash/lib")

    gc.collect()


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
    except Exception as e:
        print("EEPROM mount failed:", e)


setup_fs()
del setup_fs

setup_eeprom_fs()
del setup_eeprom_fs

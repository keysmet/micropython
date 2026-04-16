# m24256.py — M24256 32KB I2C EEPROM block device driver
#
# Implements the MicroPython block device protocol so this EEPROM can be
# mounted as a LittleFS filesystem (uos.VfsLfs2).
#
# Hardware constraints:
#   - 64-byte pages: writes that cross a page boundary wrap around and corrupt data.
#     writeblocks() splits every write at page boundaries automatically.
#   - 5ms write cycle per page: the EEPROM is busy for 5ms after each page write.
#     writeblocks() waits after every page write.
#
# Layout (with OffsetBlockDev reserving the first 2 blocks for raw use):
#   Block 0  (0x0000–0x00FF) : raw — POWER flag at 0x0000, MODE flag at 0x0001
#   Block 1  (0x0100–0x01FF) : raw — reserved
#   Blocks 2–127             : LittleFS VFS (126 blocks = ~31.5KB usable)

import time

EEPROM_ADDR = 0x50
_PAGE_SIZE  = 64    # M24256 hardware write page (bytes)
BLOCK_SIZE  = 256   # VFS block size — must be a multiple of _PAGE_SIZE
NUM_BLOCKS  = 128   # 128 × 256B = 32768B = 32KB


class M24256:
    def __init__(self, i2c, addr=EEPROM_ADDR):
        self._i2c  = i2c
        self._addr = addr

    def _read(self, byte_addr, n):
        self._i2c.writeto(self._addr, bytes([byte_addr >> 8, byte_addr & 0xFF]))
        return self._i2c.readfrom(self._addr, n)

    def _write_page(self, byte_addr, data):
        """Write one chunk that stays within a single 64-byte page, then wait."""
        self._i2c.writeto(self._addr, bytes([byte_addr >> 8, byte_addr & 0xFF]) + bytes(data))
        time.sleep_ms(5)

    # ── Block device protocol ──────────────────────────────────────────────────

    def readblocks(self, block, buf, offset=0):
        """Read len(buf) bytes from block+offset into buf."""
        byte_addr = block * BLOCK_SIZE + offset
        data = self._read(byte_addr, len(buf))
        for i in range(len(buf)):
            buf[i] = data[i]

    def writeblocks(self, block, buf, offset=0):
        """Write buf to block+offset, splitting at 64-byte page boundaries."""
        byte_addr = block * BLOCK_SIZE + offset
        n   = len(buf)
        pos = 0
        while pos < n:
            page_offset = (byte_addr + pos) % _PAGE_SIZE
            chunk       = min(_PAGE_SIZE - page_offset, n - pos)
            self._write_page(byte_addr + pos, buf[pos:pos + chunk])
            pos += chunk

    def ioctl(self, op, arg):
        if op == 4: return NUM_BLOCKS   # block count
        if op == 5: return BLOCK_SIZE   # block size
        if op == 6: return 0            # erase block — no-op for EEPROM


class OffsetBlockDev:
    """Wraps a block device and hides its first `start_block` blocks from the VFS.

    This lets us keep raw data (POWER/MODE flags) at the bottom of the EEPROM
    while LittleFS uses the rest, without either side interfering with the other.

    Usage:
        eeprom  = M24256(i2c)
        fs_bdev = OffsetBlockDev(eeprom, start_block=2)
        uos.mount(uos.VfsLfs2(fs_bdev), '/eeprom')
    """

    def __init__(self, bdev, start_block):
        self._bdev  = bdev
        self._start = start_block
        self._num   = bdev.ioctl(4, 0) - start_block

    def readblocks(self, block, buf, offset=0):
        self._bdev.readblocks(block + self._start, buf, offset)

    def writeblocks(self, block, buf, offset=0):
        self._bdev.writeblocks(block + self._start, buf, offset)

    def ioctl(self, op, arg):
        if op == 4: return self._num    # report reduced block count to VFS
        return self._bdev.ioctl(op, arg)

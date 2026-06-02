import _ksm_native as _nat

class NeoPixel:
    def __init__(self, pin, n):
        self._n = n
        self._data = [(0, 0, 0)] * n

    def __setitem__(self, i, v):
        self._data[i] = v

    def __getitem__(self, i):
        return self._data[i]

    def write(self):
        # nRF stores np[NB_LEDS - key], so index 0 = K10, index 9 = K1.
        # _nat.set_color(key, r, g, b) expects key 1–10.
        for i in range(self._n):
            r, g, b = self._data[i]
            _nat.set_color(self._n - i, r, g, b)

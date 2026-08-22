# ksynth

Chiptune synthesizer for the KSM1 (`synth` MicroPython module).

- `ksynth.c` / `ksynth.h` are a **vendored copy** of the engine developed and
  validated in the `ksm-synth` repo — do not edit here; change them there and
  run `ksm-synth/tools/sync-mpy.sh`.
- `modsynth.c` / `modsynth.h` are the MicroPython bindings, shared by every
  port (nRF I2S, WebAssembly). Each port provides `ksynth_port_start()` to
  begin pumping `ksyn_render()` into its audio output.

Python quick reference:

```python
import synth

coin = synth.Sound(decay=400, sustain=0, hold=0,
                   arp=(0, 5), arp_ms=80, arp_mode=synth.ARP_ONCE)
v = synth.play(coin, 83)          # note 83, velocity 255, auto voice
synth.release(v)                  # start the release phase
synth.stop()                      # hard-stop everything

synth.tempo(128, 4)               # 4 ticks per beat (16ths)
synth.queue(6, 45, 2, bass)       # voice 6: note 45 for 2 ticks
synth.queue(6, synth.REST, 2)     # rest
synth.ticks()                     # tempo ticks since last call (poll & refill)
```

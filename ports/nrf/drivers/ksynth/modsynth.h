/*
 * modsynth.h - MicroPython `synth` module (ksynth bindings).
 *
 * The module itself is port-agnostic (modsynth.c). Each port implements
 * ksynth_port_start(), called once when the module is first used. It must
 * call ksyn_init() with the true output sample rate of its backend (the
 * engine adapts pitch/tempo math to it), then begin pumping ksyn_render()
 * into that backend (nRF: I2S DMA at ~22727 Hz, WebAssembly: Web Audio
 * pull at 22050 Hz).
 */
#ifndef MICROPY_INCLUDED_KSYNTH_MODSYNTH_H
#define MICROPY_INCLUDED_KSYNTH_MODSYNTH_H

void ksynth_port_start(void);

#endif

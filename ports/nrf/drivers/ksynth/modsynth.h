/*
 * modsynth.h - MicroPython `synth` module (ksynth bindings).
 *
 * The module itself is port-agnostic (modsynth.c). Each port implements
 * ksynth_port_start(), called once when the module is first used, to begin
 * pumping ksyn_render() output into its audio backend (nRF: I2S DMA,
 * WebAssembly: Web Audio pull via JS).
 */
#ifndef MICROPY_INCLUDED_KSYNTH_MODSYNTH_H
#define MICROPY_INCLUDED_KSYNTH_MODSYNTH_H

void ksynth_port_start(int sample_rate);

#endif

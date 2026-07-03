/*
 * sfxr_mp.h - MicroPython glue for the sfxr synthesizer.
 *
 * Converts a MicroPython params dict into an sfxr_params struct. Kept next to
 * sfxr.c (rather than in a port's modaudio.c) so every backend that exposes the
 * `audio` module shares one source of truth for the parameter names and their
 * default values. The nRF (I2S) and WebAssembly (Web Audio) backends both use
 * this, so the browser preview stays in sync with the hardware.
 *
 * This depends on the MicroPython runtime (py/obj.h) but not on any port
 * hardware, so it compiles unchanged on every port.
 */
#ifndef SFXR_MP_H
#define SFXR_MP_H

#include "py/obj.h"
#include "sfxr.h"

// Populate *p from a MicroPython dict of sfxr parameters. Missing keys take
// their sfxr defaults. Assumes `dict` is a dict (callers validate the type so
// they can raise a friendlier TypeError first).
void sfxr_params_from_mp_dict(mp_obj_t dict, sfxr_params *p);

#endif // SFXR_MP_H

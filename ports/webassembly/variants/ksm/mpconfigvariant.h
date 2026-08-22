#define MICROPY_VARIANT_ENABLE_JS_HOOK (1)

// Enable the native `audio` module (sfxr → Web Audio, see modaudio.c).
#define MICROPY_PY_AUDIO (1)

// Enable the native `synth` module (ksynth → Web Audio stream, see
// modsynth_port.c and ports/nrf/drivers/ksynth/).
#define MICROPY_PY_SYNTH (1)

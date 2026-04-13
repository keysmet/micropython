// modksm.c — MicroPython "ksm" module for KSM1 board
//
// Handles key state (press/down/hold/release).
// Power management is handled in freeze/main.py via EEPROM flag + machine.reset().
// NeoPixel LEDs stay in Python — the neopixel module is already frozen.
//
// Python usage:
//   from ksm import *
//   update()
//   if press(KEY_K1): ...
//   if hold(KEY_MENU, 1000): shutdown()

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "nrf_gpio.h"

// ── Pin definitions ──────────────────────────────────────────────────────────
// Same order as KEY_PINS in keysmet.cpp: MENU, K1..K10
// Absolute pin numbers: P1.xx = 32 + xx
#define KEY_COUNT 11
static const uint8_t KEY_PINS[KEY_COUNT] = {
    42,  // MENU  P1.10
    22,  // K1    P0.22
    17,  // K2    P0.17
    13,  // K3    P0.13
     8,  // K4    P0.08
     6,  // K5    P0.06
    24,  // K6    P0.24
    20,  // K7    P0.20
    15,  // K8    P0.15
    41,  // K9    P1.09
    27,  // K10   P0.27
};

#define KEY_MENU 0
#define KEY_K1   1
#define KEY_K10  10

// ── Key state ────────────────────────────────────────────────────────────────

typedef struct {
    bool down;
    bool was_down;
    uint32_t press_time_ms;
} KeyState;

static KeyState keys[KEY_COUNT];
static bool pins_initialized = false;

static void ksm_init_pins(void) {
    for (int i = 0; i < KEY_COUNT; i++) {
        nrf_gpio_cfg_input(KEY_PINS[i], NRF_GPIO_PIN_PULLUP); // active low
    }
    pins_initialized = true;
}

// ── ksm.update() ─────────────────────────────────────────────────────────────
// Read all key states. Call once per main loop iteration.

static mp_obj_t ksm_update(void) {
    if (!pins_initialized) ksm_init_pins();
    uint32_t now = mp_hal_ticks_ms();
    for (int i = 0; i < KEY_COUNT; i++) {
        keys[i].was_down = keys[i].down;
        keys[i].down = (nrf_gpio_pin_read(KEY_PINS[i]) == 0);
        if (keys[i].down && !keys[i].was_down) {
            keys[i].press_time_ms = now;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ksm_update_obj, ksm_update);

// ── ksm.down(key) → bool ────────────────────────────────────────────────────

static mp_obj_t ksm_down(mp_obj_t key_obj) {
    int k = mp_obj_get_int(key_obj);
    if (k < 0 || k >= KEY_COUNT) return mp_const_false;
    return mp_obj_new_bool(keys[k].down);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ksm_down_obj, ksm_down);

// ── ksm.press(key) → bool ───────────────────────────────────────────────────
// True only on the falling edge (first call after key is pressed)

static mp_obj_t ksm_press(mp_obj_t key_obj) {
    int k = mp_obj_get_int(key_obj);
    if (k < 0 || k >= KEY_COUNT) return mp_const_false;
    return mp_obj_new_bool(keys[k].down && !keys[k].was_down);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ksm_press_obj, ksm_press);

// ── ksm.release(key) → bool ─────────────────────────────────────────────────
// True only on the rising edge (first call after key is released)

static mp_obj_t ksm_release(mp_obj_t key_obj) {
    int k = mp_obj_get_int(key_obj);
    if (k < 0 || k >= KEY_COUNT) return mp_const_false;
    return mp_obj_new_bool(!keys[k].down && keys[k].was_down);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ksm_release_obj, ksm_release);

// ── ksm.hold(key, ms) → bool ────────────────────────────────────────────────
// True if key has been held for at least `ms` milliseconds

static mp_obj_t ksm_hold(mp_obj_t key_obj, mp_obj_t ms_obj) {
    int k = mp_obj_get_int(key_obj);
    if (k < 0 || k >= KEY_COUNT) return mp_const_false;
    if (!keys[k].down) return mp_const_false;
    uint32_t elapsed = mp_hal_ticks_ms() - keys[k].press_time_ms;
    return mp_obj_new_bool(elapsed >= (uint32_t)mp_obj_get_int(ms_obj));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ksm_hold_obj, ksm_hold);

// ── Export table ─────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t ksm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ksm) },

    // Key index constants
    { MP_ROM_QSTR(MP_QSTR_KEY_COUNT), MP_ROM_INT(KEY_COUNT) },
    { MP_ROM_QSTR(MP_QSTR_KEY_MENU),  MP_ROM_INT(KEY_MENU) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K1),    MP_ROM_INT(KEY_K1) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K2),    MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K3),    MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K4),    MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K5),    MP_ROM_INT(5) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K6),    MP_ROM_INT(6) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K7),    MP_ROM_INT(7) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K8),    MP_ROM_INT(8) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K9),    MP_ROM_INT(9) },
    { MP_ROM_QSTR(MP_QSTR_KEY_K10),   MP_ROM_INT(KEY_K10) },

    // Functions
    { MP_ROM_QSTR(MP_QSTR_update),    MP_ROM_PTR(&ksm_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_down),      MP_ROM_PTR(&ksm_down_obj) },
    { MP_ROM_QSTR(MP_QSTR_press),     MP_ROM_PTR(&ksm_press_obj) },
    { MP_ROM_QSTR(MP_QSTR_release),   MP_ROM_PTR(&ksm_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_hold),      MP_ROM_PTR(&ksm_hold_obj) },
};
static MP_DEFINE_CONST_DICT(ksm_module_globals, ksm_module_globals_table);

const mp_obj_module_t ksm_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ksm_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ksm, ksm_module);

// modksm.c — USB HID interface for KSM1 (keyboard + gamepad)
//
// Exposes two functions via the `hid` built-in module:
//   hid.hid_keys([keycodes], modifier=0)  — keyboard report (report ID 1)
//   hid.hid_gamepad(bitmask)              — gamepad report  (report ID 2)
//
// Consumed by freeze/keyboard.py and freeze/gamepad.py respectively.
// Keycodes: USB HID usage page 0x07 (4=A, 40=Enter, 44=Space, 80=Left arrow)

#include "py/runtime.h"
#include "py/obj.h"
#if MICROPY_HW_USB_HID
#ifndef NO_QSTR
#include "tusb.h"
#endif
#endif

// ── hid.hid_keys([keycode, ...], modifier=0) ─────────────────────────────────
// Send a USB HID keyboard report. Pass an empty list [] to release all keys.
// Up to 6 simultaneous keycodes (USB HID limit).

#if MICROPY_HW_USB_HID
static mp_obj_t hid_hid_keys(size_t n_args, const mp_obj_t *args) {
    uint8_t modifier = n_args >= 2 ? (uint8_t)mp_obj_get_int(args[1]) : 0;
    uint8_t keycodes[6] = {0};
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(args[0], &len, &items);
    if (len > 6) {
        len = 6;
    }
    for (size_t i = 0; i < len; i++) {
        keycodes[i] = (uint8_t)mp_obj_get_int(items[i]);
    }
    tud_hid_keyboard_report(1, modifier, keycodes);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hid_hid_keys_obj, 1, 2, hid_hid_keys);

// ── hid.hid_gamepad(buttons) ──────────────────────────────────────────────────
// Send a gamepad report. buttons is a bitmask: bit 0 = button 1, bit 9 = button 10.
// Pass 0 to release all buttons.

static mp_obj_t hid_hid_gamepad(mp_obj_t buttons_obj) {
    uint16_t buttons = (uint16_t)mp_obj_get_int(buttons_obj) & 0x3FF;
    uint8_t report[2] = {
        (uint8_t)(buttons & 0xFF),
        (uint8_t)((buttons >> 8) & 0x03),
    };
    tud_hid_report(2, report, sizeof(report));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_hid_gamepad_obj, hid_hid_gamepad);
#endif

// ── Export table ─────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t hid_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hid) },
    #if MICROPY_HW_USB_HID
    { MP_ROM_QSTR(MP_QSTR_hid_keys),     MP_ROM_PTR(&hid_hid_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_hid_gamepad),  MP_ROM_PTR(&hid_hid_gamepad_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(hid_module_globals, hid_module_globals_table);

const mp_obj_module_t hid_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hid_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hid, hid_module);

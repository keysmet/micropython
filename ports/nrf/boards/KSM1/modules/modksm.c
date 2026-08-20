// modksm.c — native helpers for KSM1, exposed via the `hid` built-in module
//
// USB HID (report ID in parens), consumed by freeze/keyboard.py + gamepad.py:
//   hid.hid_keys([keycodes], modifier=0)  — keyboard report (1)
//   hid.hid_gamepad(bitmask)              — gamepad report  (2)
//   hid.hid_consumer(usage)               — consumer/media  (3)
// Keycodes: USB HID usage page 0x07 (4=A, 40=Enter, 44=Space, 80=Left arrow)
//
// Hang watchdog (see boards/KSM1/board.c), used by freeze/ksm.py:
//   hid.feed()    — ksm.tick() calls this each yield; missing it for ~1s reboots to MENU
//   hid.disarm()  — main.py calls this before the shutdown sequence stops ticking

#include "py/runtime.h"
#include "py/obj.h"
#include "mpconfigboard.h"   // KSM1_watchdog_feed / KSM1_watchdog_disarm
#if MICROPY_HW_USB_HID
#ifndef NO_QSTR
#include "tusb.h"
#endif
#endif

// ── hid.feed() / hid.disarm() — hang watchdog control ────────────────────────
static mp_obj_t hid_feed(void) {
    KSM1_watchdog_feed();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(hid_feed_obj, hid_feed);

static mp_obj_t hid_disarm(void) {
    KSM1_watchdog_disarm();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(hid_disarm_obj, hid_disarm);

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

// ── hid.hid_consumer(usage) ───────────────────────────────────────────────────
// Send a consumer-control report. usage is a 16-bit HID consumer usage code
// (usage page 0x0C): 0xCD play/pause, 0xE9/0xEA volume up/down, 0xE2 mute, ...
// Pass 0 to release.

static mp_obj_t hid_hid_consumer(mp_obj_t usage_obj) {
    uint16_t usage = (uint16_t)mp_obj_get_int(usage_obj);
    uint8_t report[2] = {
        (uint8_t)(usage & 0xFF),
        (uint8_t)((usage >> 8) & 0xFF),
    };
    tud_hid_report(3, report, sizeof(report));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_hid_consumer_obj, hid_hid_consumer);
#endif

// ── Export table ─────────────────────────────────────────────────────────────

static const mp_rom_map_elem_t hid_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hid) },
    { MP_ROM_QSTR(MP_QSTR_feed),         MP_ROM_PTR(&hid_feed_obj) },
    { MP_ROM_QSTR(MP_QSTR_disarm),       MP_ROM_PTR(&hid_disarm_obj) },
    #if MICROPY_HW_USB_HID
    { MP_ROM_QSTR(MP_QSTR_hid_keys),     MP_ROM_PTR(&hid_hid_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_hid_gamepad),  MP_ROM_PTR(&hid_hid_gamepad_obj) },
    { MP_ROM_QSTR(MP_QSTR_hid_consumer), MP_ROM_PTR(&hid_hid_consumer_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(hid_module_globals, hid_module_globals_table);

const mp_obj_module_t hid_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hid_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hid, hid_module);

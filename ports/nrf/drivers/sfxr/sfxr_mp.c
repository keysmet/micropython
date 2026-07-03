/*
 * sfxr_mp.c - see sfxr_mp.h
 *
 * Single source of truth for the sfxr parameter names and defaults shared by
 * every `audio` backend (nRF I2S, WebAssembly Web Audio).
 */
#include <string.h>
#include "py/runtime.h"
#include "sfxr_mp.h"

// Fetch key from the params dict as a float, or `defval` if absent.
static float sfxr_get_float(mp_obj_t dict, qstr key, float defval) {
    mp_map_t *map = mp_obj_dict_get_map(dict);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL) {
        return defval;
    }
    return mp_obj_get_float(elem->value);
}

static int sfxr_get_int(mp_obj_t dict, qstr key, int defval) {
    mp_map_t *map = mp_obj_dict_get_map(dict);
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    if (elem == NULL) {
        return defval;
    }
    return mp_obj_get_int(elem->value);
}

void sfxr_params_from_mp_dict(mp_obj_t dict, sfxr_params *p) {
    memset(p, 0, sizeof(*p));

    p->wave_type      = sfxr_get_int(dict, MP_QSTR_wave_type, 0);

    p->p_env_attack   = sfxr_get_float(dict, MP_QSTR_p_env_attack, 0.0f);
    p->p_env_sustain  = sfxr_get_float(dict, MP_QSTR_p_env_sustain, 0.3f);
    p->p_env_punch    = sfxr_get_float(dict, MP_QSTR_p_env_punch, 0.0f);
    p->p_env_decay    = sfxr_get_float(dict, MP_QSTR_p_env_decay, 0.4f);

    p->p_base_freq    = sfxr_get_float(dict, MP_QSTR_p_base_freq, 0.3f);
    p->p_freq_limit   = sfxr_get_float(dict, MP_QSTR_p_freq_limit, 0.0f);
    p->p_freq_ramp    = sfxr_get_float(dict, MP_QSTR_p_freq_ramp, 0.0f);
    p->p_freq_dramp   = sfxr_get_float(dict, MP_QSTR_p_freq_dramp, 0.0f);

    p->p_vib_strength = sfxr_get_float(dict, MP_QSTR_p_vib_strength, 0.0f);
    p->p_vib_speed    = sfxr_get_float(dict, MP_QSTR_p_vib_speed, 0.0f);

    p->p_arp_mod      = sfxr_get_float(dict, MP_QSTR_p_arp_mod, 0.0f);
    p->p_arp_speed    = sfxr_get_float(dict, MP_QSTR_p_arp_speed, 0.0f);

    p->p_duty         = sfxr_get_float(dict, MP_QSTR_p_duty, 0.0f);
    p->p_duty_ramp    = sfxr_get_float(dict, MP_QSTR_p_duty_ramp, 0.0f);

    p->p_repeat_speed = sfxr_get_float(dict, MP_QSTR_p_repeat_speed, 0.0f);

    p->p_pha_offset   = sfxr_get_float(dict, MP_QSTR_p_pha_offset, 0.0f);
    p->p_pha_ramp     = sfxr_get_float(dict, MP_QSTR_p_pha_ramp, 0.0f);

    p->p_lpf_freq     = sfxr_get_float(dict, MP_QSTR_p_lpf_freq, 1.0f);
    p->p_lpf_ramp     = sfxr_get_float(dict, MP_QSTR_p_lpf_ramp, 0.0f);
    p->p_lpf_resonance = sfxr_get_float(dict, MP_QSTR_p_lpf_resonance, 0.0f);
    p->p_hpf_freq     = sfxr_get_float(dict, MP_QSTR_p_hpf_freq, 0.0f);
    p->p_hpf_ramp     = sfxr_get_float(dict, MP_QSTR_p_hpf_ramp, 0.0f);

    p->sound_vol      = sfxr_get_float(dict, MP_QSTR_sound_vol, 0.25f);
}

# keyboard.py — USB HID keyboard for KSM1
#
# The board sends *physical key positions* (USB HID usage page 0x07); the host
# OS turns them into characters using whatever layout it is set to. So Ctrl+C,
# Enter, arrows and F-keys are reliable everywhere, while a letter/digit lands
# as that physical key's glyph on the host's layout (US unless remapped).
#
# Usage:
#   import keyboard as kb
#   kb.press(kb.C, kb.CTRL)     # tap Ctrl+C (press then release)
#   kb.press(kb.ENTER)          # tap Enter
#   kb.down(kb.SPACE)           # hold Space (e.g. from onPress)
#   kb.up(kb.SPACE)             # let go     (e.g. from onRelease)
#   kb.media(kb.PLAY_PAUSE)     # media/consumer key

try:
    from hid import hid_keys as _keys, hid_consumer as _consumer
except ImportError:                       # simulator: fall back to the sim native
    try:
        from _ksm_native import hid_keys as _keys, hid_consumer as _consumer
    except ImportError:
        def _keys(codes, modifier=0): pass
        def _consumer(usage): pass

from ksm import wait as _wait

# ── Modifiers (combine with |) ────────────────────────────────────────────────
CTRL  = 0x01
SHIFT = 0x02
ALT   = 0x04
GUI   = 0x08

# ── Keycodes (USB HID usage page 0x07) ────────────────────────────────────────
A=4;  B=5;  C=6;  D=7;  E=8;  F=9;  G=10; H=11; I=12; J=13
K=14; L=15; M=16; N=17; O=18; P=19; Q=20; R=21; S=22; T=23
U=24; V=25; W=26; X=27; Y=28; Z=29
N1=30; N2=31; N3=32; N4=33; N5=34; N6=35; N7=36; N8=37; N9=38; N0=39
ENTER=40; ESC=41; BACKSPACE=42; TAB=43; SPACE=44
MINUS=45; EQUAL=46; LBRACKET=47; RBRACKET=48; BACKSLASH=49
SEMICOLON=51; APOSTROPHE=52; GRAVE=53; COMMA=54; DOT=55; SLASH=56; CAPS=57
F1=58;  F2=59;  F3=60;  F4=61;  F5=62;  F6=63
F7=64;  F8=65;  F9=66;  F10=67; F11=68; F12=69
PRINT_SCREEN=70; SCROLL_LOCK=71; PAUSE=72
INSERT=73; HOME=74; PAGE_UP=75; DELETE=76; END=77; PAGE_DOWN=78
RIGHT=79; LEFT=80; DOWN=81; UP=82
# Keypad
KP_DIV=84; KP_MUL=85; KP_SUB=86; KP_ADD=87; KP_ENTER=88
KP_1=89; KP_2=90; KP_3=91; KP_4=92; KP_5=93
KP_6=94; KP_7=95; KP_8=96; KP_9=97; KP_0=98; KP_DOT=99

# ── Consumer/media usages (USB HID usage page 0x0C) ───────────────────────────
PLAY_PAUSE=0xCD; NEXT=0xB5; PREV=0xB6; STOP=0xB7
MUTE=0xE2; VOL_UP=0xE9; VOL_DOWN=0xEA
BRIGHT_UP=0x6F; BRIGHT_DOWN=0x70

# ── State: the set of keycodes currently held (for chords / hold-to-repeat) ────
_held = []

def _report(mod):
    _keys(_held[:6], mod)                 # USB report carries up to 6 keycodes

def down(code, mod=0):
    """Press and hold a key (with optional modifier). Wire to onPress."""
    if code not in _held:
        _held.append(code)
    _report(mod)

def up(code, mod=0):
    """Release a held key. Passing mod=0 also clears modifiers. Wire to onRelease."""
    if code in _held:
        _held.remove(code)
    _report(mod)

def press(code, mod=0):
    """Tap a key: press then release. The macropad workhorse — press(C, CTRL)."""
    _keys([code], mod)
    _wait(30)
    _keys([])
    _wait(20)

def media(usage):
    """Send a media/consumer key (PLAY_PAUSE, VOL_UP, ...): press then release."""
    _consumer(usage)
    _wait(30)
    _consumer(0)
    _wait(20)

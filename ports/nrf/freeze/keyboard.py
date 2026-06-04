# keyboard.py — USB HID keyboard API for KSM1
#
# Usage:
#   import keyboard
#   keyboard.set_layout('azerty')       # optional, default: 'qwerty'
#   keyboard.type_text("hello world")
#   keyboard.shortcut(keyboard.C, modifier=keyboard.MOD_CTRL)
#   keyboard.key(keyboard.ENTER)

try:
    from hid import hid_keys as _send
except ImportError:
    def _send(keycodes, modifier=0): pass

from ksm import wait as _wait

# ── Modifiers ─────────────────────────────────────────────────────────────────
MOD_CTRL  = 0x01
MOD_SHIFT = 0x02
MOD_ALT   = 0x04
MOD_GUI   = 0x08

# ── Named key constants (USB HID usage page 0x07) ─────────────────────────────
A=4;  B=5;  C=6;  D=7;  E=8;  F=9;  G=10; H=11; I=12; J=13
K=14; L=15; M=16; N=17; O=18; P=19; Q=20; R=21; S=22; T=23
U=24; V=25; W=26; X=27; Y=28; Z=29
N1=30; N2=31; N3=32; N4=33; N5=34; N6=35; N7=36; N8=37; N9=38; N0=39
ENTER=40; ESC=41; BACKSPACE=42; TAB=43; SPACE=44
MINUS=45; EQUAL=46; LBRACKET=47; RBRACKET=48; BACKSLASH=49
SEMICOLON=51; APOSTROPHE=52; GRAVE=53; COMMA=54; DOT=55; SLASH=56
F1=58;  F2=59;  F3=60;  F4=61;  F5=62;  F6=63
F7=64;  F8=65;  F9=66;  F10=67; F11=68; F12=69
RIGHT=79; LEFT=80; DOWN=81; UP=82

# ── Keymaps: character → (keycode, needs_shift) ───────────────────────────────
# Keycodes are physical key positions on a US keyboard.
# The OS translates them to characters based on the active layout.

_QWERTY = {
    'a':(4,0), 'b':(5,0), 'c':(6,0), 'd':(7,0), 'e':(8,0), 'f':(9,0), 'g':(10,0),
    'h':(11,0),'i':(12,0),'j':(13,0),'k':(14,0),'l':(15,0),'m':(16,0),'n':(17,0),
    'o':(18,0),'p':(19,0),'q':(20,0),'r':(21,0),'s':(22,0),'t':(23,0),'u':(24,0),
    'v':(25,0),'w':(26,0),'x':(27,0),'y':(28,0),'z':(29,0),
    '1':(30,0),'2':(31,0),'3':(32,0),'4':(33,0),'5':(34,0),
    '6':(35,0),'7':(36,0),'8':(37,0),'9':(38,0),'0':(39,0),
    '\n':(40,0),'\t':(43,0),' ':(44,0),
    '-':(45,0),'=':(46,0),'[':(47,0),']':(48,0),'\\':(49,0),
    ';':(51,0),"'":(52,0),'`':(53,0),',':(54,0),'.':(55,0),'/':(56,0),
    'A':(4,1), 'B':(5,1), 'C':(6,1), 'D':(7,1), 'E':(8,1), 'F':(9,1), 'G':(10,1),
    'H':(11,1),'I':(12,1),'J':(13,1),'K':(14,1),'L':(15,1),'M':(16,1),'N':(17,1),
    'O':(18,1),'P':(19,1),'Q':(20,1),'R':(21,1),'S':(22,1),'T':(23,1),'U':(24,1),
    'V':(25,1),'W':(26,1),'X':(27,1),'Y':(28,1),'Z':(29,1),
    '!':(30,1),'@':(31,1),'#':(32,1),'$':(33,1),'%':(34,1),
    '^':(35,1),'&':(36,1),'*':(37,1),'(':(38,1),')':(39,1),
    '_':(45,1),'+':(46,1),'{':(47,1),'}':(48,1),'|':(49,1),
    ':':(51,1),'"':(52,1),'~':(53,1),'<':(54,1),'>':(55,1),'?':(56,1),
}

# AZERTY: digit row unshifted = & é " ' ( - è _ ç à
# Digits require Shift. Letters are at different physical positions.
_AZERTY = {
    # Letters
    'a':(20,0),'z':(26,0),'e':(8,0), 'r':(21,0),'t':(23,0),'y':(28,0),
    'u':(24,0),'i':(12,0),'o':(18,0),'p':(19,0),
    'q':(4,0), 's':(22,0),'d':(7,0), 'f':(9,0), 'g':(10,0),'h':(11,0),
    'j':(13,0),'k':(14,0),'l':(15,0),'m':(51,0),
    'w':(29,0),'x':(27,0),'c':(6,0), 'v':(25,0),'b':(5,0), 'n':(17,0),
    'A':(20,1),'Z':(26,1),'E':(8,1), 'R':(21,1),'T':(23,1),'Y':(28,1),
    'U':(24,1),'I':(12,1),'O':(18,1),'P':(19,1),
    'Q':(4,1), 'S':(22,1),'D':(7,1), 'F':(9,1), 'G':(10,1),'H':(11,1),
    'J':(13,1),'K':(14,1),'L':(15,1),'M':(51,1),
    'W':(29,1),'X':(27,1),'C':(6,1), 'V':(25,1),'B':(5,1), 'N':(17,1),
    # Digit row symbols (unshifted)
    '&':(30,0),'"':(32,0),"'":(33,0),'(':(34,0),'-':(35,0),'_':(37,0),
    ')':(45,0),
    # Digits (Shift + digit row)
    '1':(30,1),'2':(31,1),'3':(32,1),'4':(33,1),'5':(34,1),
    '6':(35,1),'7':(36,1),'8':(37,1),'9':(38,1),'0':(39,1),
    # Common
    '\n':(40,0),'\t':(43,0),' ':(44,0),
    ',':(16,0),';':(54,0),':':(55,0),'.':(55,1),
}

_layout = _QWERTY

def set_layout(name):
    """Select the active keymap. 'qwerty' (default) or 'azerty'."""
    global _layout
    _layout = _AZERTY if name == 'azerty' else _QWERTY

# ── Core functions ─────────────────────────────────────────────────────────────

def key(code, modifier=0):
    """Press and release a single key. Use named constants: keyboard.ENTER, keyboard.A, etc."""
    _send([code], modifier)
    _wait(30)
    _send([])
    _wait(20)

def shortcut(*codes, modifier=0):
    """Send a key combination. Ex: shortcut(C, modifier=MOD_CTRL) → Ctrl+C"""
    _send(list(codes), modifier)
    _wait(30)
    _send([])
    _wait(20)

def type_text(text):
    """Type a string character by character using the active layout."""
    for ch in text:
        entry = _layout.get(ch)
        if entry is None:
            continue
        code, shift = entry
        key(code, MOD_SHIFT if shift else 0)

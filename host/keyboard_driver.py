#!/usr/bin/env python3
"""
Runs on the Raspberry Pi Zero (2)W. This driver polls the RP2040 keyboard driver over I2C
and, replays each keycode as a virtual USB keyboard via /dev/uinput.

Setup on the Pi:
    sudo raspi-config nonint do_i2c 0     # enable the I2C interface
    pip3 install smbus2 evdev
    sudo usermod -aG i2c,input "$USER"    # or run this script as root
    sudo python3 keyboard_driver.py

Shift is a one-shot modifier, meaning pressing shift
just capitalizes/shifts the next character typed, then clears itself.

Modifier keys act are chorded/one-shot modifiers, meaning tapping one
starts a short window (SUPER_CHORD_TIMEOUT_S). If another key arrives inside
that window, it's replayed with the modifier held. If nothing
follows before the window closes, a bare tap of the modifier key is emitted.
"""

import time

import smbus2
from evdev import UInput, ecodes as e

I2C_BUS = 1
I2C_ADDR = 0x1F
POLL_INTERVAL_S = 0.005
KEY_HOLD_S = 0.008
SUPER_CHORD_TIMEOUT_S = 0.4

# control bytes from the firmware
SYM = "\x01"
SHIFT_R = "\x02"
ALT = "\x03"
MIC = "\x04"
SHIFT_L = "\x05"
MUTE = "\x06"
CTRL = "\x07"
HOST_ALT = "\x08"
CAPS_LOCK = "\x0a"
DELETE = "\x7f"

KEY_UP_BYTE = "\x11"
KEY_LEFT_BYTE = "\x12"
KEY_DOWN_BYTE = "\x13"
KEY_RIGHT_BYTE = "\x14"

MUTE_KEYCODE = e.KEY_MUTE
SUPER_KEYCODE = e.KEY_LEFTMETA
CTRL_KEYCODE = e.KEY_LEFTCTRL
ALT_KEYCODE = e.KEY_LEFTALT
SHIFT_KEYCODE = e.KEY_LEFTSHIFT
CAPS_KEYCODE = e.KEY_CAPSLOCK

# char -> (evdev keycode, needs_shift)
SYMBOL_MAP = {
    "1": (e.KEY_1, False),
    "2": (e.KEY_2, False),
    "3": (e.KEY_3, False),
    "4": (e.KEY_4, False),
    "5": (e.KEY_5, False),
    "6": (e.KEY_6, False),
    "7": (e.KEY_7, False),
    "8": (e.KEY_8, False),
    "9": (e.KEY_9, False),
    "0": (e.KEY_0, False),
    "#": (e.KEY_3, True),
    "_": (e.KEY_MINUS, True),
    "-": (e.KEY_MINUS, False),
    "+": (e.KEY_EQUAL, True),
    "/": (e.KEY_SLASH, False),
    ":": (e.KEY_SEMICOLON, True),
    ";": (e.KEY_SEMICOLON, False),
    "'": (e.KEY_APOSTROPHE, False),
    '"': (e.KEY_APOSTROPHE, True),
    "(": (e.KEY_9, True),
    ")": (e.KEY_0, True),
    "*": (e.KEY_8, True),
    "@": (e.KEY_2, True),
    "?": (e.KEY_SLASH, True),
    "!": (e.KEY_1, True),
    "$": (e.KEY_4, True),
    "`": (e.KEY_GRAVE, False),
    "|": (e.KEY_BACKSLASH, True),
    ".": (e.KEY_DOT, False),
    " ": (e.KEY_SPACE, False),
    "\r": (e.KEY_ENTER, False),
    "\b": (e.KEY_BACKSPACE, False),
    "\t": (e.KEY_TAB, False),
    "\x1b": (e.KEY_ESC, False),
    "\x7f": (e.KEY_DELETE, False),
    "\x11": (e.KEY_UP, False),
    "\x12": (e.KEY_LEFT, False),
    "\x13": (e.KEY_DOWN, False),
    "\x14": (e.KEY_RIGHT, False),
    "[": (e.KEY_LEFTBRACE, False),
    "]": (e.KEY_RIGHTBRACE, False),
    "{": (e.KEY_LEFTBRACE, True),
    "}": (e.KEY_RIGHTBRACE, True),
    "<": (e.KEY_COMMA, True),
    ">": (e.KEY_DOT, True),
    "=": (e.KEY_EQUAL, False),
    "^": (e.KEY_6, True),
    "%": (e.KEY_5, True),
    "\\": (e.KEY_BACKSLASH, False),
    "~": (e.KEY_GRAVE, True),
}

LETTER_KEYCODES = {c: getattr(e, f"KEY_{c}") for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}

MODIFIER_KEYCODES = {
    SHIFT_KEYCODE,
    CTRL_KEYCODE,
    ALT_KEYCODE,
    SUPER_KEYCODE,
    CAPS_KEYCODE,
}

CAPABILITIES = {
    e.EV_KEY: sorted(
        set(LETTER_KEYCODES.values())
        | {code for code, _ in SYMBOL_MAP.values()}
        | MODIFIER_KEYCODES
        | {MUTE_KEYCODE}
    ),
}


def resolve(ch, pending_shift):
    """Map a received character to (keycode, needs_shift), or (None, ...)."""
    if ch in LETTER_KEYCODES:
        return LETTER_KEYCODES[ch], pending_shift

    if ch in SYMBOL_MAP:
        code, needs_shift = SYMBOL_MAP[ch]

        return code, pending_shift or needs_shift

    return None, pending_shift


def tap(ui, code, modifiers=()):
    for mod in modifiers:
        ui.write(e.EV_KEY, mod, 1)
        ui.syn()

    ui.write(e.EV_KEY, code, 1)
    ui.syn()

    time.sleep(KEY_HOLD_S)

    ui.write(e.EV_KEY, code, 0)
    ui.syn()

    for mod in reversed(modifiers):
        ui.write(e.EV_KEY, mod, 0)
        ui.syn()


def main():
    bus = smbus2.SMBus(I2C_BUS)
    ui = UInput(CAPABILITIES, name="q10-keyboard")

    pending_shift = False
    pending_super_since = None
    pending_ctrl_since = None
    pending_alt_since = None

    print(f"Listening on I2C bus {I2C_BUS}, address 0x{I2C_ADDR:02X}...")

    try:
        while True:
            try:
                byte = bus.read_byte(I2C_ADDR)
            except OSError:
                byte = 0

            if byte != 0:
                ch = chr(byte)

                if ch in (SHIFT_L, SHIFT_R):
                    pending_shift = True
                elif ch in (ALT, SYM):
                    pass
                elif ch == MIC:
                    pending_super_since = time.monotonic()
                elif ch == CTRL:
                    pending_ctrl_since = time.monotonic()
                elif ch == HOST_ALT:
                    pending_alt_since = time.monotonic()
                elif ch == CAPS_LOCK:
                    tap(ui, CAPS_KEYCODE)
                elif ch == MUTE:
                    modifiers = []

                    if pending_shift:
                        modifiers.append(SHIFT_KEYCODE)
                        pending_shift = False

                    if pending_ctrl_since is not None:
                        modifiers.append(CTRL_KEYCODE)
                        pending_ctrl_since = None

                    if pending_alt_since is not None:
                        modifiers.append(ALT_KEYCODE)
                        pending_alt_since = None

                    if pending_super_since is not None:
                        modifiers.append(SUPER_KEYCODE)
                        pending_super_since = None

                    tap(ui, MUTE_KEYCODE, modifiers)
                else:
                    code, shift = resolve(ch, pending_shift)
                    pending_shift = False

                    if code is not None:
                        modifiers = []

                        if shift:
                            modifiers.append(SHIFT_KEYCODE)

                        if pending_ctrl_since is not None:
                            modifiers.append(CTRL_KEYCODE)
                            pending_ctrl_since = None

                        if pending_alt_since is not None:
                            modifiers.append(ALT_KEYCODE)
                            pending_alt_since = None

                        if pending_super_since is not None:
                            modifiers.append(SUPER_KEYCODE)
                            pending_super_since = None

                        tap(ui, code, modifiers)

            now = time.monotonic()
            if (
                pending_ctrl_since is not None
                and now - pending_ctrl_since > SUPER_CHORD_TIMEOUT_S
            ):
                modifiers = []

                if pending_shift:
                    modifiers.append(SHIFT_KEYCODE)
                    pending_shift = False

                tap(ui, CTRL_KEYCODE, modifiers)
                pending_ctrl_since = None

            if (
                pending_alt_since is not None
                and now - pending_alt_since > SUPER_CHORD_TIMEOUT_S
            ):
                modifiers = []

                if pending_shift:
                    modifiers.append(SHIFT_KEYCODE)
                    pending_shift = False

                tap(ui, ALT_KEYCODE, modifiers)
                pending_alt_since = None

            if (
                pending_super_since is not None
                and now - pending_super_since > SUPER_CHORD_TIMEOUT_S
            ):
                modifiers = []

                if pending_shift:
                    modifiers.append(SHIFT_KEYCODE)
                    pending_shift = False

                tap(ui, SUPER_KEYCODE, modifiers)
                pending_super_since = None

            time.sleep(POLL_INTERVAL_S)

    finally:
        ui.close()
        bus.close()


if __name__ == "__main__":
    main()


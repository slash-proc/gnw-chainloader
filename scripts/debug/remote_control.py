#!/usr/bin/env python3
"""Manually drive the Game & Watch UI over the debug probe — keyboard or gamepad.

Thin front-end over the shared backend in `scripts/common/remote_input.py`. The
device mechanism lives there; this file is input capture + a key map, exposed as
pollable InputSource objects so the fastcap capture tool can drive the device
live while it records.

REQUIRES a firmware built with the remote-input hook compiled in (default on;
opt out with `make REMOTE_INPUT=0`). A build without the hook does nothing.

Modes
-----
  auto (default)  try a gamepad first (Linux evdev); fall back to the keyboard if
                  none is found.
  --gamepad       force a controller via evdev (pip install evdev). OS key-up
                  events give TRUE press/release — let go and it releases instantly.
  --keyboard      force raw-terminal keypresses. Terminals send key-down but no
                  key-up, so a held key stays down for --hold ms and releases when
                  the terminal's auto-repeat stops (release lags by ~--hold ms).

`WindowKeySource` here is the same idea for the fastcap OpenCV live window
(`fastcap.py capture --live`): watch the stream and drive in one window.

Keyboard map
------------
    arrows = D-pad      a/Enter = A      b/Backspace = B
    s = Start           Tab = Select     p = Pause
    g = Game            t = Time         P (shift) = Power
    q / Ctrl-C = quit (clears the shadow cell on the way out)

The Left+Game combo is the launcher-escape macro (works in the menu and inside a
running stock game). Hold Left, tap Game — or just press both.
"""
from __future__ import annotations

import argparse
import atexit
import select
import sys
import termios
import time
import tty
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri

# Single-byte / control keys -> button bit.
KEYMAP = {
    "\r": ri.BTN_A, "\n": ri.BTN_A, "a": ri.BTN_A,
    "\x7f": ri.BTN_B, "\b": ri.BTN_B, "b": ri.BTN_B,
    "s": ri.BTN_START, "\t": ri.BTN_SELECT,
    "p": ri.BTN_PAUSE, "g": ri.BTN_GAME, "t": ri.BTN_TIME,
    "P": ri.BTN_PWR,
}
ARROWS = {
    "\x1b[A": ri.BTN_UP, "\x1b[B": ri.BTN_DOWN,
    "\x1b[C": ri.BTN_RIGHT, "\x1b[D": ri.BTN_LEFT,
}


def _parse_keys(buf: str):
    """Yield button bits from a raw stdin chunk; None signals quit."""
    i = 0
    while i < len(buf):
        if buf[i] == "\x1b":
            seq = buf[i:i + 3]
            if seq in ARROWS:
                yield ARROWS[seq]
                i += 3
                continue
            yield None  # lone ESC = quit
            return
        ch = buf[i]
        if ch in ("q", "\x03"):
            yield None
            return
        if ch in KEYMAP:
            yield KEYMAP[ch]
        i += 1


# ---- Pollable input sources ----------------------------------------------
#
# Each source maps a physical keyboard/gamepad to the device's 12-bit button
# mask (bit positions = remote_input.BTN_*).  poll(now) is NON-BLOCKING: it
# returns the current held mask, or None to request quit.  This lets one source
# drive remote_control's own loop AND be interleaved inside fastcap's
# single-threaded capture loop (one shared OpenOCD session, no threads).

class InputSource:
    def open(self) -> "InputSource":
        return self

    def close(self) -> None:
        pass

    def poll(self, now: float):  # -> int mask | None
        raise NotImplementedError


class KeyboardSource(InputSource):
    """Raw-terminal keypresses with a hold timer (no OS key-up on a terminal)."""

    def __init__(self, hold_ms: int = 150):
        self.hold_s = hold_ms / 1000.0
        self._expiry: dict[int, float] = {}
        self._fd = None
        self._old = None

    def open(self) -> "KeyboardSource":
        self._fd = sys.stdin.fileno()
        self._old = termios.tcgetattr(self._fd)
        atexit.register(self._restore)
        tty.setraw(self._fd)
        return self

    def _restore(self) -> None:
        if self._old is not None and self._fd is not None:
            try:
                termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old)
            except Exception:
                pass

    def close(self) -> None:
        self._restore()

    def poll(self, now: float):
        r, _, _ = select.select([sys.stdin], [], [], 0)
        if r:
            for bit in _parse_keys(sys.stdin.read(64)):
                if bit is None:
                    return None
                self._expiry[bit] = now + self.hold_s
        self._expiry = {b: t for b, t in self._expiry.items() if t > now}
        mask = 0
        for b in self._expiry:
            mask |= 1 << b
        return mask


def _find_gamepad(path: str | None):
    from evdev import InputDevice, ecodes, list_devices
    if path:
        return InputDevice(path)
    for dp in list_devices():
        d = InputDevice(dp)
        caps = d.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.BTN_SOUTH in caps or ecodes.BTN_GAMEPAD in caps:
            return d
    raise RuntimeError("no gamepad found (pass --device /dev/input/eventN, or check permissions)")


class GamepadSource(InputSource):
    """A controller via Linux evdev — true press/release, polled non-blocking.

    Shoulders map to Game/Time so the Left+Game launcher-escape combo is
    reachable; D-pad comes in on the HAT0 axes.
    """

    def __init__(self, path: str | None = None):
        self.path = path
        self._pad = None
        self._held = 0
        self._keymap = None
        self._ecodes = None

    def open(self) -> "GamepadSource":
        from evdev import ecodes
        self._ecodes = ecodes
        self._pad = _find_gamepad(self.path)
        self._keymap = {
            ecodes.BTN_SOUTH: ri.BTN_A, ecodes.BTN_EAST: ri.BTN_B,
            ecodes.BTN_START: ri.BTN_START, ecodes.BTN_SELECT: ri.BTN_SELECT,
            ecodes.BTN_TR: ri.BTN_GAME, ecodes.BTN_TL: ri.BTN_TIME,
            ecodes.BTN_THUMBL: ri.BTN_PAUSE, ecodes.BTN_MODE: ri.BTN_PWR,
        }
        return self

    @property
    def name(self) -> str:
        return f"{self._pad.name} ({self._pad.path})" if self._pad else "?"

    def close(self) -> None:
        if self._pad is not None:
            try:
                self._pad.close()
            except Exception:
                pass

    def _apply(self, ev) -> None:
        ec = self._ecodes
        if ev.type == ec.EV_KEY and ev.code in self._keymap:
            bit = 1 << self._keymap[ev.code]
            if ev.value:
                self._held |= bit
            else:
                self._held &= ~bit
        elif ev.type == ec.EV_ABS:
            if ev.code == ec.ABS_HAT0X:
                self._held &= ~((1 << ri.BTN_LEFT) | (1 << ri.BTN_RIGHT))
                if ev.value < 0:
                    self._held |= 1 << ri.BTN_LEFT
                elif ev.value > 0:
                    self._held |= 1 << ri.BTN_RIGHT
            elif ev.code == ec.ABS_HAT0Y:
                self._held &= ~((1 << ri.BTN_UP) | (1 << ri.BTN_DOWN))
                if ev.value < 0:
                    self._held |= 1 << ri.BTN_UP
                elif ev.value > 0:
                    self._held |= 1 << ri.BTN_DOWN

    def poll(self, now: float):
        while True:
            try:
                ev = self._pad.read_one()
            except (BlockingIOError, OSError):
                break
            if ev is None:
                break
            self._apply(ev)
        return self._held


class WindowKeySource(InputSource):
    """Keyboard control fed from an OpenCV window's waitKeyEx() — the fastcap live
    cockpit (watch + drive in one window). Like the terminal, an OpenCV window
    gives key-down but no key-up, so it uses the same hold timer. The window owner
    (LivePreview) calls feed(key, now) with each waitKeyEx result; poll(now)
    returns the held button mask. 'q'/Esc sets .quit; 'f' is handled by the owner
    (FPS toggle), so it is not mapped here.
    """

    DPAD = {
        ord("w"): ri.BTN_UP,   ord("a"): ri.BTN_LEFT,
        ord("s"): ri.BTN_DOWN, ord("d"): ri.BTN_RIGHT,
        65362: ri.BTN_UP, 65364: ri.BTN_DOWN,        # Linux/Qt/GTK arrow extended codes
        65361: ri.BTN_LEFT, 65363: ri.BTN_RIGHT,
    }
    BTNS = {
        ord("j"): ri.BTN_A,     13: ri.BTN_A,        # J or Enter
        ord("k"): ri.BTN_B,      8: ri.BTN_B,        # K or Backspace
        ord("u"): ri.BTN_START, ord("i"): ri.BTN_SELECT,
        ord("g"): ri.BTN_GAME,  ord("t"): ri.BTN_TIME,
        ord("p"): ri.BTN_PAUSE, ord("o"): ri.BTN_PWR,
    }
    QUIT = {ord("q"), 27}
    KEY_HELP = ("WASD/arrows=D-pad  J/Enter=A  K/Bksp=B  U=Start  I=Select  "
                "G=Game  T=Time  P=Pause  O=Power  |  F=FPS  Q/Esc=quit")

    def __init__(self, hold_ms: int = 150):
        self.hold_s = hold_ms / 1000.0
        self._expiry: dict[int, float] = {}
        self.quit = False

    def feed(self, key: int, now: float) -> None:
        if key < 0:
            return
        if key in self.QUIT:
            self.quit = True
        elif key in self.DPAD:
            self._expiry[self.DPAD[key]] = now + self.hold_s
        elif key in self.BTNS:
            self._expiry[self.BTNS[key]] = now + self.hold_s

    def poll(self, now: float):
        self._expiry = {b: t for b, t in self._expiry.items() if t > now}
        mask = 0
        for b in self._expiry:
            mask |= 1 << b
        return mask


def open_input_source(prefer: str = "auto", device: str | None = None, hold_ms: int = 150):
    """Open and return an InputSource.

    prefer: 'auto' (gamepad first, else keyboard), 'gamepad', 'keyboard', or
    'none' (returns None).  A forced source raises if it is unavailable; 'auto'
    silently falls back to the keyboard when no gamepad/evdev is present.
    """
    if prefer == "none":
        return None
    if prefer in ("auto", "gamepad"):
        try:
            src = GamepadSource(device).open()
            print(f"  Input: gamepad {src.name}")
            return src
        except Exception as e:
            if prefer == "gamepad":
                raise
            print(f"  Input: no gamepad ({e}); using keyboard")
    src = KeyboardSource(hold_ms).open()
    print("  Input: keyboard (raw terminal)")
    return src


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_mutually_exclusive_group()
    g.add_argument("--gamepad", action="store_true", help="force a controller via evdev")
    g.add_argument("--keyboard", action="store_true", help="force raw-terminal keyboard")
    p.add_argument("--device", help="explicit evdev path, e.g. /dev/input/event5")
    p.add_argument("--hold", type=int, default=150,
                   help="keyboard: ms a key stays held before auto-release (default 150)")
    args = p.parse_args()

    prefer = "gamepad" if args.gamepad else "keyboard" if args.keyboard else "auto"
    with ri.session() as dev:
        src = open_input_source(prefer, args.device, args.hold)
        print("--- driving (q / Ctrl-C to quit) ---")
        last = -1
        try:
            while True:
                now = time.monotonic()
                mask = src.poll(now)
                if mask is None:
                    break
                if mask != last:
                    dev.transport.write_mask(mask)
                    last = mask
                time.sleep(0.005)
        except KeyboardInterrupt:
            pass
        finally:
            src.close()
    print("\nCleared shadow cell. Bye.")


if __name__ == "__main__":
    main()

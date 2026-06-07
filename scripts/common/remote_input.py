"""Remote button injection backend — the shared engine for driving the device.

This is the DRY core that both the manual control front-end (`scripts/debug/
remote_control.py`) and the automated tests (`scripts/tests/`) build on. Nothing
here is interactive; it is pure mechanism.

How it works
------------
A `REMOTE_INPUT` firmware build OR's a 32-bit "shadow" word at
`SRAM_REMOTE_INPUT_ADDR` (0x2001FFF4, DTCM) into its live button state every
input poll, so a debug-probe write to that cell is indistinguishable from a
physical press. See DESIGN.md §10.1 for the firmware side.

The whole point of this module is **one persistent OpenOCD connection**. A tap is
`set mask` → ~80 ms → `clear`, all over a socket that stays open — milliseconds of
overhead. (The original mistake was making a tap out of two separate `memory.py`
invocations, each spinning up its own OpenOCD session; press→release ended up
*seconds* apart, so the firmware's auto-repeat treated every "tap" as a long hold
and scrolled the whole menu.)

Transport abstraction
----------------------
`InputTransport` is the seam for the future: today the only transport is
`ShadowCellTransport` (writes the SWD shadow cell). A Raspberry-Pi rig that drives
NRST / the power button / real button lines over GPIO would implement the same
interface, and every `button_press()` caller — including all tests — keeps working
unchanged.

API (mirrors the shape the maintainer asked for)
------------------------------------------------
    from common import remote_input as ri

    dev = ri.RemoteInput(); dev.open()
    dev.button_press([ri.BTN_DOWN])               # single clean tap
    dev.button_press([ri.BTN_DOWN], repeat=3)     # three taps
    dev.button_press([ri.BTN_A])

    with dev.button_press([ri.BTN_LEFT, ri.BTN_GAME], hold=True):
        time.sleep(0.5)                            # held for the block, auto-released

    dev.close()                                    # always clears the cell

Module-level convenience functions (`connect()`, `button_press()`, …) delegate to
a single default instance for quick scripting; tests should prefer an explicit
`RemoteInput` instance (or the context manager).
"""
from __future__ import annotations

import time
from contextlib import contextmanager

from . import REPO_ROOT  # noqa: F401  (ensures gnwmanager is importable)

# --- Shadow cell: keep in sync with src/common/memory_map.h ---
SHADOW_ADDR = 0x2001FFF4

# --- Button bits: keep in sync with input_button_t in
#     src/chainloader/system/input.h (the "unified format"). ---
BTN_UP = 0
BTN_DOWN = 1
BTN_LEFT = 2
BTN_RIGHT = 3
BTN_A = 4
BTN_B = 5
BTN_START = 6
BTN_SELECT = 7
BTN_PAUSE = 8
BTN_GAME = 9
BTN_TIME = 10
BTN_PWR = 11

# Name <-> bit, handy for CLIs / logging.
NAMES = {
    "UP": BTN_UP, "DOWN": BTN_DOWN, "LEFT": BTN_LEFT, "RIGHT": BTN_RIGHT,
    "A": BTN_A, "B": BTN_B, "START": BTN_START, "SELECT": BTN_SELECT,
    "PAUSE": BTN_PAUSE, "GAME": BTN_GAME, "TIME": BTN_TIME, "PWR": BTN_PWR,
}
BIT_NAME = {v: k for k, v in NAMES.items()}

# Default tap timing. The firmware auto-repeat fires at 350 ms, and it polls the
# cell once per UI frame, so a tap must be held long enough to span >=2 frames
# (~33 ms at 30 fps) but well under 350 ms to register as exactly one press.
TAP_MS = 80
GAP_MS = 120


def mask_of(keys) -> int:
    """OR a button or iterable of buttons into a single bitmask."""
    if isinstance(keys, int):
        keys = (keys,)
    m = 0
    for k in keys:
        m |= 1 << int(k)
    return m


def mask_str(mask: int) -> str:
    """Human-readable button list for a mask (e.g. 'LEFT+GAME')."""
    parts = [BIT_NAME[b] for b in range(16) if mask & (1 << b)]
    return "+".join(parts) if parts else "none"


class InputTransport:
    """Abstract way to assert a raw button bitmask on the device.

    Implement `write_mask` (and optionally open/close) to add a new way of
    pressing buttons (e.g. Raspberry-Pi GPIO). Everything above this — taps,
    holds, combos, repeat — is transport-agnostic.
    """

    def open(self) -> "InputTransport":
        return self

    def close(self) -> None:
        pass

    def reconnect(self) -> "InputTransport":
        """Re-establish the link after a device reset. Default: close then open."""
        try:
            self.close()
        except Exception:
            pass
        return self.open()

    def write_mask(self, mask: int) -> None:  # pragma: no cover - interface
        raise NotImplementedError


class ShadowCellTransport(InputTransport):
    """Writes the button bitmask to the SWD shadow cell over one OpenOCD socket.

    Pass an already-open backend to share it (e.g. with framebuffer capture);
    otherwise this opens its own and owns its lifecycle. The CPU is left RUNNING
    — `mww`/`mdw` go through the AHB-AP as background accesses, so the menu loop
    keeps drawing while we inject.
    """

    def __init__(self, backend=None, addr: int = SHADOW_ADDR):
        self._backend = backend
        self._own_backend = backend is None
        self._addr = addr

    def open(self) -> "ShadowCellTransport":
        if self._backend is None:
            from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
            self._backend = OpenOCDBackend()
            self._backend.open()
        return self

    def close(self) -> None:
        if self._own_backend and self._backend is not None:
            self._backend.close()
            self._backend = None

    def reconnect(self) -> "ShadowCellTransport":
        """Open a brand-new OpenOCD after a device reset killed the old one.

        A bank swap / escape resets the MCU, which tears down the SWD session at
        the process level — the old backend's socket and subprocess are dead, and
        even close() on it throws. So drop the reference outright and open fresh;
        OpenOCDBackend.open() kills any leftover openocd, so no zombie is left.
        """
        if not self._own_backend:
            raise RuntimeError("cannot reconnect a shared (externally-owned) backend")
        self._backend = None
        return self.open()

    def write_mask(self, mask: int) -> None:
        self._backend.write_uint32(self._addr, mask & 0xFFFFFFFF)

    @property
    def backend(self):
        """The live OpenOCD backend (for memory reads / framebuffer capture)."""
        return self._backend


class _Hold:
    """Context manager that holds a mask down for the duration of a `with` block."""

    def __init__(self, dev: "RemoteInput", mask: int):
        self._dev = dev
        self._mask = mask

    def __enter__(self) -> "_Hold":
        self._dev._add_held(self._mask)
        return self

    def __exit__(self, *exc) -> bool:
        self._dev._remove_held(self._mask)
        return False


class _NullCM:
    """Inert context manager so `with button_press([X])` is harmless when hold=False."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class RemoteInput:
    """Press buttons on the device over a persistent connection.

    Tracks a `held` mask (set by `hold=True` blocks) so taps compose correctly on
    top of held buttons.
    """

    def __init__(self, transport: InputTransport | None = None):
        self.transport = transport or ShadowCellTransport()
        self._held = 0
        self._open = False

    # -- lifecycle --------------------------------------------------------
    def open(self) -> "RemoteInput":
        self.transport.open()
        self._open = True
        self._held = 0
        self.transport.write_mask(0)  # start from a known-clear state
        return self

    def close(self) -> None:
        if self._open:
            try:
                self.transport.write_mask(0)  # never leave a bit stuck
            finally:
                self.transport.close()
                self._open = False

    def __enter__(self) -> "RemoteInput":
        return self.open()

    def __exit__(self, *exc) -> bool:
        self.close()
        return False

    @property
    def backend(self):
        """The live OpenOCD backend behind the transport (None for non-OCD ones)."""
        return getattr(self.transport, "backend", None)

    def reconnect(self) -> "RemoteInput":
        """Recover the probe link after a device reset (bank swap / escape).

        The held mask is dropped (the device rebooted, nothing is held) and the
        shadow cell re-cleared. Safe to call repeatedly while polling for a
        not-yet-responsive target.
        """
        self._held = 0
        self.transport.reconnect()
        self._open = True
        try:
            self.transport.write_mask(0)
        except Exception:
            pass
        return self

    # -- low-level --------------------------------------------------------
    def _flush(self, transient: int = 0) -> None:
        self.transport.write_mask(self._held | transient)

    def _add_held(self, mask: int) -> None:
        self._held |= mask
        self._flush()

    def _remove_held(self, mask: int) -> None:
        self._held &= ~mask
        self._flush()

    # -- presses ----------------------------------------------------------
    def tap(self, keys, repeat: int = 1, tap_ms: int = TAP_MS, gap_ms: int = GAP_MS) -> None:
        """Press and release `keys` cleanly `repeat` times (one menu step each)."""
        mask = mask_of(keys)
        for i in range(repeat):
            self._flush(mask)
            time.sleep(tap_ms / 1000.0)
            self._flush(0)                 # release back to held state
            if i + 1 < repeat:
                time.sleep(gap_ms / 1000.0)

    def hold(self, keys) -> _Hold:
        """Return a context manager that holds `keys` for the `with` block."""
        return _Hold(self, mask_of(keys))

    def button_press(self, keys, hold: bool = False, repeat: int = 1):
        """Tap (default) or, with hold=True, return a hold context manager.

        Mirrors the requested API:
            button_press([BTN_DOWN])                 # tap
            button_press([BTN_DOWN], repeat=3)       # 3 taps
            with button_press([BTN_LEFT, BTN_GAME], hold=True): ...
        """
        if hold:
            return self.hold(keys)
        self.tap(keys, repeat=repeat)
        return _NullCM()

    # short aliases
    def press(self, keys, **kw):
        return self.button_press(keys, **kw)


# --- module-level convenience over a single default instance -------------
_default: RemoteInput | None = None


def connect(transport: InputTransport | None = None) -> RemoteInput:
    """Open (or reuse) the module-level default RemoteInput."""
    global _default
    if _default is None:
        _default = RemoteInput(transport)
        _default.open()
    return _default


def close() -> None:
    global _default
    if _default is not None:
        _default.close()
        _default = None


def _require() -> RemoteInput:
    if _default is None:
        raise RuntimeError("call remote_input.connect() first")
    return _default


def button_press(keys, hold: bool = False, repeat: int = 1):
    return _require().button_press(keys, hold=hold, repeat=repeat)


def tap(keys, repeat: int = 1):
    _require().tap(keys, repeat=repeat)


@contextmanager
def session(transport: InputTransport | None = None):
    """`with session() as dev:` — open a RemoteInput and guarantee cleanup."""
    dev = RemoteInput(transport)
    dev.open()
    try:
        yield dev
    finally:
        dev.close()

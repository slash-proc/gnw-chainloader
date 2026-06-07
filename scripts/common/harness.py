"""Test-harness helpers shared by everything under scripts/tests/.

Per the project layout rule, test scripts depend only on `scripts/common/`,
never on `scripts/debug/`. Anything a test needs in common goes here: resolving
firmware symbols, peeking device state (menu selection), capturing frames over the
same live connection used for input, and a tiny assertion/reporting harness.
"""
from __future__ import annotations

import signal
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field

from . import REPO_ROOT, resolve

APP_ELF = REPO_ROOT / "build" / "app" / "app.elf"

# ui_list_t layout (src/chainloader/ui/ui_list.h): const char *title; int
# num_items; int selected; ... -> `selected` is at byte offset 8.
UI_LIST_SELECTED_OFFSET = 8


@contextmanager
def time_budget(seconds: float, label: str):
    """Hard wall-clock bound (SIGALRM; main thread, Unix) so a wedged probe op
    ABORTS with a clear error instead of hanging forever.

    The OpenOCD TCL socket has NO per-command timeout -- its receive loop only
    breaks on a broken pipe, so neither a socket timeout nor a poll deadline can
    interrupt a stuck `read_uint32`; only a wall-clock signal can. EVERY block
    that touches the probe should run inside one of these. Do not nest them (one
    itimer): wrap discrete phases sequentially instead. Mirrors the same guard in
    scripts/build/push_batched.py (kept separate so this test lib stays free of a
    build-tooling dependency)."""
    def _fire(signum, frame):
        raise TimeoutError(f"{label}: exceeded {seconds:.0f}s budget (probe wedged?)")
    old = signal.signal(signal.SIGALRM, _fire)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def resolve_symbol(name: str, elf=APP_ELF) -> int:
    """Return the load address of `name` from the ELF symbol table via nm.

    Tolerates LTO name-mangling (`g_list_main` -> `g_list_main.lto_priv.0`):
    matches the first symbol whose name equals `name` or starts with `name.`.
    Addresses move every rebuild, so always resolve fresh — never hardcode.
    """
    elf = resolve(elf)
    out = subprocess.run(
        ["arm-none-eabi-nm", str(elf)],
        capture_output=True, text=True, check=True,
    ).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr, _kind, sym = parts
        if sym == name or sym.startswith(name + "."):
            return int(addr, 16)
    raise KeyError(f"symbol {name!r} not found in {elf}")


def read_u32_symbol(backend, name: str, offset: int = 0) -> int:
    """Read a uint32 at `name` (+offset). For plain scalars use offset 0."""
    return backend.read_uint32(resolve_symbol(name) + offset)


def read_menu_selection(backend, list_symbol: str = "g_list_main") -> int:
    """Read the currently highlighted index of a ui_list_t (CPU may be running)."""
    return read_u32_symbol(backend, list_symbol, UI_LIST_SELECTED_OFFSET)


def chainloader_running(backend, settle: float = 0.25, min_ticks: int = 10,
                        max_ticks: int = 100000, budget: float = 15.0):
    """Return (ok, detail): is the bank-1 chainloader's main loop actually live?

    A successful flash/push does NOT mean control reached the chainloader — the
    gnwmanager RAM flasher stays resident, and the CPU can be parked in it (or
    hung, or off in an OFW). This proves the chainloader is the thing executing by
    reading its OWN SysTick counter (uwTick, bumped by SysTick_Handler ->
    HAL_IncTick) twice with the CPU running in between: it advances every ms while
    the chainloader runs, and is frozen otherwise (the RAM flasher is halted; an
    OFW updates a different uwTick, not this one). Make it a go-to precondition
    before trusting any chainloader state read over the probe.

    `ok` requires a sane, nonzero advance over `settle` seconds (a wild delta
    means a wrong/garbage read). Self-bounded by `budget` seconds so a wedged
    probe returns (False, ...) instead of hanging — do NOT wrap the call in
    another time_budget (one itimer; no nesting).
    """
    try:
        with time_budget(budget, "chainloader_running"):
            addr = resolve_symbol("uwTick")
            backend.halt(); t0 = backend.read_uint32(addr); backend.resume()
            time.sleep(settle)
            backend.halt(); t1 = backend.read_uint32(addr); backend.resume()
    except TimeoutError as e:
        return False, str(e)
    delta = (t1 - t0) & 0xFFFFFFFF
    ok = min_ticks <= delta <= max_ticks
    return ok, f"uwTick {t0}->{t1} (+{delta} in {settle:.2f}s)"


def safe_read_u32(backend, addr: int):
    """Read a uint32, returning None instead of raising.

    After a bank swap (or when the OFW parks the core in a low-power state) the
    debug probe may return an empty response. Callers that expect that state use
    this so a transient read can't crash a test.
    """
    try:
        return backend.read_uint32(addr)
    except Exception:
        return None


def wait_u32(dev, addr: int, test_fn, timeout: float = 90.0,
             interval: float = 2.0, reconnect: bool = True):
    """Poll `addr` until `test_fn(value)` is true or `timeout` elapses.

    Designed for the across-reset case: after a bank swap / escape the probe
    session is dead, so by default this `dev.reconnect()`s the backend on each
    attempt (tolerating a target that isn't responding yet — e.g. waiting for a
    manual power-on). Returns the last value read (or None).
    """
    deadline = time.time() + timeout
    last = None
    while True:
        if reconnect:
            try:
                dev.reconnect()
            except Exception:
                pass
        last = safe_read_u32(dev.backend, addr)
        if last is not None:
            try:
                if test_fn(last):
                    return last
            except Exception:
                pass
        if time.time() >= deadline:
            return last
        time.sleep(interval)


def recover_probe():
    """Reset-halt then resume via trace.py to restore a wedged probe link.

    Returns True if it ran cleanly. Mirrors the documented OpenOCD-hang
    workaround (DESIGN.md §10) without importing anything from scripts/debug —
    it shells out to the canonical tool instead.
    """
    elf = REPO_ROOT / "scripts" / "debug" / "trace.py"
    try:
        subprocess.run([sys.executable, str(elf), "reset-halt"],
                       capture_output=True, text=True, timeout=60, check=False)
        subprocess.run([sys.executable, str(elf), "resume"],
                       capture_output=True, text=True, timeout=60, check=False)
        return True
    except Exception:
        return False


def capture_frame(backend, out_path):
    """Save a framebuffer PNG over the given backend (halts+resumes briefly)."""
    from . import device
    out_path = resolve(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    backend.halt()
    try:
        img, info = device.read_framebuffer(backend)
    finally:
        backend.resume()
    img.save(out_path)
    return out_path, info


# --------------------------------------------------------------------------
# Minimal test harness: no pytest dependency, runnable straight from the CLI.
# --------------------------------------------------------------------------
@dataclass
class Result:
    name: str
    passed: int = 0
    failed: int = 0
    notes: list = field(default_factory=list)


class TestRun:
    """Tiny check/report harness. `check()` records pass/fail without aborting;
    `require()` raises to abort a test that can't meaningfully continue."""

    def __init__(self, name: str):
        self.r = Result(name)
        print(f"\n=== {name} ===")

    def check(self, cond: bool, msg: str) -> bool:
        if cond:
            self.r.passed += 1
            print(f"  [PASS] {msg}")
        else:
            self.r.failed += 1
            print(f"  [FAIL] {msg}")
        return cond

    def require(self, cond: bool, msg: str) -> None:
        if not self.check(cond, msg):
            raise AssertionError(msg)

    def note(self, msg: str) -> None:
        self.r.notes.append(msg)
        print(f"  [note] {msg}")

    def summary(self) -> int:
        print(f"--- {self.r.name}: {self.r.passed} passed, {self.r.failed} failed ---")
        return 0 if self.r.failed == 0 else 1


def settle(seconds: float = 0.25) -> None:
    """Give the UI a few frames to update after an input before reading state."""
    time.sleep(seconds)


def wake(dev) -> None:
    """Dismiss the idle-hidden menu without moving the selection.

    PAGE_MAIN has allow_idle_hide=1: after ~30 s idle the menu box hides and the
    next input only un-hides it (no navigation). Tapping a no-op direction
    (SELECT isn't bound on the main list) wakes it so the following taps actually
    move. Cheap to call before any navigation.
    """
    import common.remote_input as ri
    dev.button_press([ri.BTN_SELECT])
    settle(0.2)


def navigate_to(dev, target_index: int, num_items: int,
                list_symbol: str = "g_list_main", max_taps: int = 24) -> int:
    """Closed-loop menu navigation: read selection, step toward target, repeat.

    Robust against idle-hide, auto-repeat overshoot, and selection drift (e.g.
    after an OFW switch). Returns the final selection index. Open-loop "tap N
    times" is intentionally avoided — it desynced when the menu was idle-hidden.
    """
    import common.remote_input as ri
    wake(dev)
    cur = read_menu_selection(dev.backend, list_symbol)
    taps = 0
    while cur != target_index and taps < max_taps:
        # Step the short way around the (small) circular list.
        down = (target_index - cur) % num_items
        up = (cur - target_index) % num_items
        dev.button_press([ri.BTN_DOWN] if down <= up else [ri.BTN_UP])
        settle(0.18)
        new = read_menu_selection(dev.backend, list_symbol)
        if new == cur:
            # No movement (e.g. a wake tap was swallowed) — nudge once more.
            dev.button_press([ri.BTN_DOWN] if down <= up else [ri.BTN_UP])
            settle(0.18)
            new = read_menu_selection(dev.backend, list_symbol)
        cur = new
        taps += 1
    return cur

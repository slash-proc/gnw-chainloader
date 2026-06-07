#!/usr/bin/env python3
"""Idle/activity timer drives the ~30 s menu auto-hide, asserted over SWD.

PAGE_MAIN auto-hides the menu box after ~30 s of no input (animations keep
running). This asserts the underlying timer over pure SWD without waiting the
full 30 s: idle time (uwTick - g_last_activity_tick) advances with no input, and
any input resets it. The visual hide + continued animation is a framebuffer check
left as a follow-on (noted, not required here).

  python3 scripts/tests/power/idle_hide_test.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L2", subsystem="power", envs=["ANY"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[6, 7])

HIDE_MS = 30000     # PAGE_MAIN idle auto-hide threshold (~30 s)


def main() -> int:
    t = h.TestRun("idle/activity timer (SWD)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")
        t.require(h.go_home(dev), "on the main menu")

        # go_home pressed B (activity), so idle time starts near zero.
        i0 = observe.idle_ticks(be)
        t.require(i0 is not None, "idle time readable (uwTick - g_last_activity_tick)")
        t.note(f"idle time just after input: {i0} ms")

        # Advance ~3 s with no input; idle time should grow by about that much.
        wait_s = 3.0
        time.sleep(wait_s)
        i1 = observe.idle_ticks(be)
        grew = i1 is not None and (i1 - i0) >= (wait_s * 1000 * 0.7)
        t.check(grew, f"idle time advances with no input ({i0} -> {i1} ms over {wait_s:.0f}s)")
        t.check(i1 is not None and i1 < HIDE_MS,
                f"still below the {HIDE_MS} ms auto-hide threshold ({i1} ms)")

        # Any input resets the activity clock.
        dev.button_press([ri.BTN_SELECT]); h.settle(0.2)
        i2 = observe.idle_ticks(be)
        t.check(i2 is not None and i2 < i1,
                f"input resets the idle clock ({i1} -> {i2} ms)")

        t.note("visual auto-hide (menu box gone, animations continue) is a "
               "framebuffer follow-on; this asserts the timer mechanism over SWD")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

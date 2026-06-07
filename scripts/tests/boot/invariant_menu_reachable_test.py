#!/usr/bin/env python3
"""The stability invariant: the launcher menu is reachable and navigable.

The single non-negotiable law (CLAUDE.md): in EVERY device state the chainloader
reaches its menu and the menu responds. This asserts it over pure SWD (no OCR):

  * the chainloader main loop is live (uwTick advancing),
  * the main list has its full item set (>= 4: LAUNCH / TOOLS / SETTINGS / POWER),
  * the cursor moves on input (DOWN changes the selection),
  * TOOLS, SETTINGS and POWER are each reachable (navigate-and-confirm).

Tagged for every environment, so the matrix runs it after provisioning each one
(ENV-BARE / ENV-CORRUPT / ENV-NO-EXTFLASH are where it bites). On bare/empty
states LAUNCH should grey out rather than hang; that is noted when the probe sees
no boot target, but the menu reachability itself is the assertion.

  python3 scripts/tests/boot/invariant_menu_reachable_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L4", subsystem="boot", envs=["ANY"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[8])

# Unified main menu order (menu.c): LAUNCH=0, TOOLS=1, SETTINGS=2, POWER=3.
MM_LAUNCH, MM_TOOLS, MM_SETTINGS, MM_POWER = 0, 1, 2, 3


def main() -> int:
    t = h.TestRun("invariant: menu reachable & navigable")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader main loop is live ({detail})")

        # Back out to the main menu first: a device left on a sub-page leaves
        # g_list_main dormant. SWD-driven (g_stack_ptr), no OCR.
        t.require(h.go_home(dev), "reached the main menu (page/modal stack empty)")

        n = observe.menu_count(be)
        t.require(n is not None and n >= 4,
                  f"main menu has its full item set (num_items={n}, expected >= 4)")

        sel = observe.menu_selection(be)
        t.require(sel is not None and 0 <= sel < n,
                  f"a valid item is selected (selected={sel})")

        # Navigability: a DOWN tap must move the cursor.
        h.wake(dev)
        before = observe.menu_selection(be)
        dev.button_press([ri.BTN_DOWN]); h.settle(0.25)
        after = observe.menu_selection(be)
        t.check(after is not None and after != before,
                f"cursor moves on input ({before} -> {after})")

        # Each core destination is reachable (closed-loop navigate + confirm).
        for idx, name in [(MM_TOOLS, "TOOLS"), (MM_SETTINGS, "SETTINGS"),
                          (MM_POWER, "POWER OFF"), (MM_LAUNCH, "LAUNCH")]:
            landed = h.navigate_to(dev, idx, n)
            t.check(landed == idx, f"{name} reachable (cursor landed on {landed}, wanted {idx})")

        # Adaptive note: on a bare state with no boot target, LAUNCH should grey
        # out (selecting it is a no-op) rather than hang. We can't press A here
        # without risking a boot, so we just record the boot-target state.
        bt = observe.boot_target(be)
        t.note(f"current LAUNCH boot target = {bt} "
               f"(on a bare device with no target, LAUNCH must grey out, not hang)")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

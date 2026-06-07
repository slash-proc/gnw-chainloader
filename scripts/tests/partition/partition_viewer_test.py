#!/usr/bin/env python3
"""Partition Viewer reachable + the flash scan ran, asserted over SWD.

Tools menu (menu.c TOOLS_ACTIONS) is [File Browser=0, Partition Viewer=1, ...];
selecting index 1 pushes PAGE_PARTITION. This navigates there by SWD index and
asserts over pure SWD that the page opened (g_stack_ptr deepened), the external
flash was detected, and the background partition/module scan completed -- then
backs out cleanly. No OCR needed.

  python3 scripts/tests/partition/partition_viewer_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L2", subsystem="partition", envs=["ENV-DOCS"],
                 build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[5, 0])

MM_TOOLS, MAIN_ITEMS = 1, 4
PV_INDEX = 1                       # Partition Viewer is index 1 in the Tools list


def main() -> int:
    t = h.TestRun("partition viewer (SWD)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")
        t.require(h.go_home(dev), "on the main menu")
        depth0 = observe.modal_depth(be)

        # Enter Tools.
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS, "g_list_main")
        dev.button_press([ri.BTN_A]); h.settle(0.4)
        depth_tools = observe.modal_depth(be)
        t.check(depth_tools is not None and depth_tools > (depth0 or 0),
                f"Tools page opened (stack {depth0} -> {depth_tools})")

        # Navigate to Partition Viewer (index 1) and open it.
        n_tools = observe.menu_count(be, "g_list_tools") or 2
        h.navigate_to(dev, PV_INDEX, n_tools, "g_list_tools")
        dev.button_press([ri.BTN_A]); h.settle(0.6)
        depth_pv = observe.modal_depth(be)
        t.check(depth_pv is not None and depth_pv > (depth_tools or 0),
                f"Partition Viewer opened (stack {depth_tools} -> {depth_pv})")

        # The flash scan ran.
        mb = observe.extflash_mb(be)
        t.check(mb is not None and mb >= 16,
                f"external flash detected ({mb} MB)")
        t.check(observe.modules_ready(be) is True,
                "partition/module scan completed (g_modules_ready)")

        ok2, detail2 = h.chainloader_running(be)
        t.check(ok2, f"chainloader still live in the viewer ({detail2})")

        # Back out cleanly.
        t.check(h.go_home(dev), "backed out of the Partition Viewer to the main menu")
        sel = observe.menu_selection(be)
        t.check(sel is not None and 0 <= sel < MAIN_ITEMS,
                f"main menu navigable again (selection={sel})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

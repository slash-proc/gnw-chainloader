#!/usr/bin/env python3
"""File Browser reachable + a filesystem mounts, asserted over SWD (non-destructive).

Tools menu index 0 (menu.c TOOLS_ACTIONS) is action_browser_enter. This opens the
File Browser by SWD index and asserts over pure SWD that the page opened, a
filesystem tab is active, a read-write driver came up (opening the browser loads
the FS RW module on demand), the partition scan didn't wedge the loop, and a clean
back-out returns to the menu. No file operations are performed (read-only / safe).

  python3 scripts/tests/fs/file_browser_open_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L2", subsystem="fs", envs=["ENV-DOCS"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[7, 9])

MM_TOOLS, MAIN_ITEMS = 1, 4
FB_INDEX = 0                       # File Browser is index 0 in the Tools list


def main() -> int:
    t = h.TestRun("file browser open (SWD)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")
        t.require(h.go_home(dev), "on the main menu")
        depth0 = observe.modal_depth(be)

        # Enter Tools, then File Browser.
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS, "g_list_main")
        dev.button_press([ri.BTN_A]); h.settle(0.4)
        depth_tools = observe.modal_depth(be)
        n_tools = observe.menu_count(be, "g_list_tools") or 2
        h.navigate_to(dev, FB_INDEX, n_tools, "g_list_tools")
        dev.button_press([ri.BTN_A]); h.settle(1.0)        # browser opens; SD redetect + scan

        depth_fb = observe.modal_depth(be)
        t.check(depth_fb is not None and depth_fb > (depth_tools or 0),
                f"File Browser opened (stack {depth_tools} -> {depth_fb})")

        tab = observe.active_tab(be)
        t.check(tab is not None and tab >= 0, f"a filesystem tab is active ({tab})")

        lfs_rw, fat_rw, drivers = observe.rw_drivers(be)
        t.check((drivers or 0) >= 1,
                f"a read-write FS driver is loaded (lfs={lfs_rw} fat={fat_rw} count={drivers})")

        ok2, detail2 = h.chainloader_running(be)
        t.check(ok2, f"chainloader still live in the browser ({detail2})")

        t.check(h.go_home(dev), "backed out of the File Browser to the main menu")
        sel = observe.menu_selection(be)
        t.check(sel is not None and 0 <= sel < MAIN_ITEMS,
                f"main menu navigable again (selection={sel})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
fileops module device test — exercises the transient /modules/fileops.bin seam that
the in-core file browser now loads on demand for delete + folder copy (Phase 1 of the
file-browser modularization: the heavy ops moved out of the 40 KiB core).

It drives the file browser by on-screen text (common/ocrnav) and deletes three
THROWAWAY items in the main LittleFS root, each of which routes through the module:
  - alpha.bin   single-file delete  (fileops load #1)
  - bravo.bin   single-file delete  (fileops load #2)  <- the use-after-free check:
                  if load #1's mod_pool_reset had freed the resident RW driver the VFS
                  still points at, this second op would fault/fail.
  - qadir/      recursive folder delete (delete_tree in the module)

Then it confirms the menu is still alive. Distinct names (no shared prefix) so OCR
can't match the wrong row; it only ever acts after ocrnav lands the cursor on that
exact item. Push the throwaways first:

  python3 scripts/build/push_batched.py \
      /alpha.bin=build/installer.bin /bravo.bin=build/installer.bin \
      /qadir/inner.bin=build/installer.bin
  python3 scripts/tests/fileops_test.py
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # scripts/
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))   # scripts/tests/
from common import harness as h
from common import ocrnav as nav
from common import remote_input as ri
# Reuse the OCR language detection (the UI may be en_UK / de / ja / ...; labels are
# translated, file names are not). Detect once, then navigate by the live strings.
from ocr_nav_test import detect_language

S = {}   # active-language string table, filled in main()

# The context-menu OPTIONS popup is a small centered overlay: its item text is
# near-white, and ocrnav's auto-fg locks onto the gold title/border (and the dimmed
# listing behind it), so the menu reads as blank. Binarizing with the item colour
# makes Copy/Paste/Delete/Cancel locate cleanly. (Found via the offline screenshot.)
MODAL_FG = np.array([248, 252, 248], dtype=np.uint8)


def enter_littlefs_root(dev):
    """Main menu -> Tools -> File Browser -> the main LittleFS root. Returns True
    once the throwaway files are on screen (which proves we're in the right FS)."""
    if not nav.enter(dev, S["STR_TITLE_TOOLS"].strip()):
        print("  FAIL: could not reach Tools"); return False
    if not nav.enter(dev, S["STR_FILE_BROWSER"].strip()):
        print("  FAIL: could not reach File Browser"); return False
    lfs = S["STR_FS_LITTLEFS"].strip()
    # The FS list may be preceded by a brief scan; give it a moment, then pick LFS.
    for _ in range(6):
        if nav.present(dev, lfs, wake=False).get(lfs):
            break
        h.settle(0.5)
    if not nav.enter(dev, lfs, wake=False):
        print(f"  FAIL: could not enter a {lfs} partition"); return False
    h.settle(0.4)
    seen = nav.present(dev, "alpha.bin", "qadir", wake=False)
    if not (seen.get("alpha.bin") and seen.get("qadir")):
        print(f"  FAIL: throwaways not visible in this LittleFS ({seen}); "
              f"wrong partition or push missing"); return False
    print("  OK   in LittleFS root; throwaways present")
    return True


def pick_delete_in_menu(dev):
    """The OPTIONS popup is open with the cursor on the top item. READ the menu with
    the modal colour, work out Delete's position from the items actually present
    (Copy, [Paste], Delete, Cancel -- Paste only shows when something was copied), and
    press DOWN exactly that many times. This is closed-loop on the read, so it's robust
    to whether Paste is shown (which shifts Delete's index) -- the open-loop "UP x2"
    guess was not. Returns True if Delete was found + targeted."""
    sc = nav.shot(dev, fg=MODAL_FG)
    found = [(loc[1], n) for n in ("Copy", "Paste", "Delete", "Cancel")
             if (loc := sc.locate(n))]
    found.sort()                              # by y => top-to-bottom menu order
    order = [n for _, n in found]
    if "Delete" not in order:
        print(f"    menu read = {order} (no Delete)"); return False
    steps = order.index("Delete")            # cursor starts on the top item (index 0)
    for _ in range(steps):
        dev.button_press([ri.BTN_DOWN]); h.settle(0.35)
    dev.button_press([ri.BTN_A]); h.settle(0.6)   # Delete -> confirm dialog
    dev.button_press([ri.BTN_A]); h.settle(1.3)   # confirm (A) -> run fileops module
    return True


def delete_selected(dev, name):
    """Put the cursor on `name`, open the context menu (PAUSE), pick Delete, confirm.
    Success is verified by `name` disappearing from the listing. Safe: ocrnav lands the
    cursor on `name` first; the delete acts on that selected row only."""
    if not nav.navigate(dev, name, wake=False):
        print(f"  FAIL: {name} not found to select"); return False
    dev.button_press([ri.BTN_PAUSE]); h.settle(0.7)   # open OPTIONS (cursor on top item)
    if not pick_delete_in_menu(dev):
        print(f"  FAIL: could not target Delete for {name}")
        dev.button_press([ri.BTN_B]); h.settle(0.3); return False
    # "Gone" = the listing no longer contains the name. Verify by trying to navigate the
    # cursor onto it: navigate() scrolls the whole list and returns False only when the
    # row truly isn't there (robust against a stale/mid-refresh frame).
    gone = not nav.navigate(dev, name, max_steps=8, settle=0.2, wake=False)
    print(f"  {'OK  ' if gone else 'FAIL'} delete {name} -> {'gone' if gone else 'STILL PRESENT'}")
    return gone


def main():
    global S
    fails = 0
    with ri.session() as dev:
        # Dismiss any leftover modal / sub-page so we start from the main menu.
        h.wake(dev); h.settle(0.3)
        for _ in range(4):
            dev.button_press([ri.BTN_B]); h.settle(0.25)
        code, S = detect_language(dev)
        print(f"[{code}] Tools={S['STR_TITLE_TOOLS']!r} Delete={S['STR_DELETE']!r}")

        if not enter_littlefs_root(dev):
            print("RESULT: 1 FAILED (setup)"); return 1

        # Single-file deletes back-to-back: each loads the fileops module transiently.
        # The second succeeding proves the resident RW driver survived the first
        # mod_pool_reset (no use-after-free). Distinct names (no shared prefix) so OCR
        # navigate/verify can't match the wrong row.
        if not delete_selected(dev, "alpha.bin"):   fails += 1
        if not delete_selected(dev, "bravo.bin"):   fails += 1
        # Recursive folder delete -> delete_tree() in the module.
        if not delete_selected(dev, "qadir"):       fails += 1

        # Menu still alive after the transient load/run/reset cycles. From the LittleFS
        # root the path out is root -> FS list -> Tools -> main, so back out generously,
        # then check for a MAIN-MENU-ONLY item (the launch verb; "Tools" is also the
        # Tools-page title, so it wouldn't distinguish the two pages).
        launch = S.get("STR_LAUNCH", "").strip()
        for _ in range(4):
            dev.button_press([ri.BTN_B]); h.settle(0.4)
        if launch and nav.present(dev, launch, wake=False).get(launch):
            print("  OK   back on main menu (UI alive after fileops ops)")
        else:
            print("  FAIL: main menu not reachable after fileops ops"); fails += 1

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

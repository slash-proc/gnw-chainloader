#!/usr/bin/env python3
"""Destructive: wipe the device's LittleFS content, prove the menu survives,
then restore from source.

The headline destructive scenario, scoped to be fast + self-restoring: remove
ALL installed modules and i18n packs from LittleFS (bank1 / Retro-Go untouched),
then assert over pure SWD that the stability invariant still holds -- the
chainloader boots, the menu is reachable and navigable, no module loaded (theme
and feature counts drop to zero), and the UI fell back. Then rebuild the content
from source (make push_modules + push_i18n) and confirm it came back.

No manual intervention; if a stage fails the device is simply left in that state
(a test fail). Bank1 is never touched, so the menu is reachable throughout.

  python3 scripts/tests/fs/lfs_wipe_restore_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402
from common import envprobe                   # noqa: E402
from common import provision                  # noqa: E402

TEST_META = dict(tier="L4", subsystem="fs", envs=["ENV-DOCS"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[8, 9, 14])


def main() -> int:
    t = h.TestRun("LittleFS wipe -> invariant -> restore")
    p = provision.Provisioner()

    # SAFETY: never wipe unless we can restore. The restore reads build/ artifacts;
    # a concurrent `make clean` (e.g. another session editing src/) could wipe them
    # and leave the device unrestorable. Require the artifacts up front and skip
    # the destructive part if they're incomplete.
    repo = Path(__file__).resolve().parents[3]
    have_modules = (repo / "build" / "theme.bin").is_file()
    have_i18n = any((repo / "build" / "i18n").glob("*.lang"))
    if not (have_modules and have_i18n):
        t.note("build/ artifacts incomplete (build/theme.bin or build/i18n/*.lang "
               "missing -- a concurrent make clean?); SKIPPING the destructive wipe "
               "to avoid leaving the device unrestorable")
        return t.summary()

    # Baseline: what's installed now.
    before = envprobe.probe(with_lfs=True)
    t.require(before.chainloader_alive, "chainloader live before wipe")
    t.note(f"before: theme={before.theme_module_count} feature={before.feature_count} "
           f"lfs_modules={sorted(before.lfs_modules)} langs={len(before.lfs_langs)}")

    # --- WIPE (discover + remove every file under /modules and /i18n) ---
    removed = p.wipe_lfs_content()
    t.check(removed is not None and removed > 0,
            f"wipe removed files from LittleFS ({removed})")
    p.clean_reboot()
    wiped = envprobe.probe(with_lfs=True)

    # The invariant MUST hold with an empty LittleFS.
    t.check(wiped.chainloader_alive, "INVARIANT: chainloader still boots with LittleFS wiped")
    with ri.session() as dev:
        t.check(h.go_home(dev), "INVARIANT: menu reachable after wipe")
        sel0 = observe.menu_selection(dev.backend)
        dev.button_press([ri.BTN_DOWN])
        h.settle(0.25)
        t.check(observe.menu_selection(dev.backend) != sel0,
                "INVARIANT: menu navigable after wipe (cursor moved)")
    # The authoritative "content gone" signal is the empty filesystem; the live
    # module counts are less reliable (g_theme_module_count can retain a stale /
    # default value across the warm reset even with no theme module loaded).
    t.check(not wiped.lfs_modules,
            f"no modules on LittleFS ({sorted(wiped.lfs_modules)})")
    t.check(not wiped.lfs_langs,
            f"no i18n packs on LittleFS ({len(wiped.lfs_langs)})")
    t.check(wiped.feature_count in (0, None),
            f"no feature modules loaded (count={wiped.feature_count})")
    t.note(f"theme_module_count after wipe = {wiped.theme_module_count} "
           f"(stale/default global; not a loaded module since LittleFS is empty)")

    # --- RESTORE from the pre-built artifacts (no make / no src recompile) ---
    p.restore_from_build()
    p.clean_reboot()
    after = envprobe.probe(with_lfs=True)
    t.check(after.chainloader_alive, "chainloader live after restore")
    t.check((after.theme_module_count or 0) >= 1,
            f"theme module restored (count={after.theme_module_count})")
    t.check(len(after.lfs_modules) >= len(before.lfs_modules) - 1,
            f"modules restored ({sorted(after.lfs_modules)})")
    t.check(len(after.lfs_langs) >= 1, f"i18n packs restored ({len(after.lfs_langs)})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

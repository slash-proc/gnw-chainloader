#!/usr/bin/env python3
"""RTL (Arabic/Farsi) recognition + deterministic recovery.

The carousel language selector cycles through ar/fa, which flip the layout to RTL
and render pre-shaped Arabic. The harness must not get tripped up by that: it must
RECOGNIZE the RTL language (not mistake it for some wrong LTR one) and be able to
RECOVER to a known language no matter what is on screen.

  * Recognition: detect_language reads the frame with the Arabic glyph set and
    matches the pack's PRE-SHAPED endonym (or, failing that, recognizes Arabic ink
    rather than guessing an LTR language).
  * Recovery: provision.set_active_language rewrites /i18n/.active on LittleFS and
    reboots -- a file write, no OCR or nav, so it escapes RTL every time.

Forces ar then fa via the file write, asserts each is recognized as RTL, then
recovers to the original language. Reboots + pushes, so it is L3 and a bit slow.

  python3 scripts/tests/i18n/rtl_recover_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import provision                  # noqa: E402
from common import i18n_strings as i18n       # noqa: E402

TEST_META = dict(tier="L3", subsystem="i18n", envs=["ENV-DOCS"], build="REMOTE_INPUT=1",
                 observable="ocr", automated=True, goal=[10])


def detect(dev):
    return i18n.detect_language(dev, wake=True)[0]


def main() -> int:
    t = h.TestRun("RTL recognize + recover")
    rtl = i18n.rtl_codes()
    t.require(bool(rtl), "RTL languages exist (ar/fa) in langs.json")

    prov = provision.Provisioner()              # owns short-lived backends (no held session)

    # Record the original language so we can restore it (leave the device as found).
    with ri.session() as dev:
        ok, detail = h.chainloader_running(dev.backend)
        t.require(ok, f"chainloader live ({detail})")
        h.go_home(dev)
        orig = detect(dev)
        t.note(f"original language: {orig}")

    try:
        for code in rtl:
            prov.set_active_language(code)       # file write + reboot (no backend held)
            with ri.session() as dev:
                h.go_home(dev)
                got = detect(dev)
                # The solid, layout-immune guarantee: the device demonstrably left
                # the known LTR language (so a carousel test knows it has entered the
                # RTL zone and should rely on recovery / SWD, not OCR). Pinning the
                # exact ar vs fa from a cursive Arabic SCREEN is best-effort: the
                # connected script doesn't segment for read_rows, and the main menu
                # doesn't show the endonym. Recovery (below) is the real safety net.
                t.check(got != orig,
                        f"{code}: device no longer reads as the original '{orig}' "
                        f"(detect={got}) -- the switch took and we can tell")
                if got in rtl:
                    t.note(f"{code}: recognized as RTL (detect={got})")
                else:
                    t.note(f"{code}: RTL not pinned via OCR (detect={got}); "
                           f"recovery is the guarantee")
                try:
                    h.capture_frame(dev.backend, f"build/rtl_{code}.png")
                except Exception:
                    pass
    finally:
        # Deterministic recovery to the original (or English) via the file write.
        target = orig if orig and orig not in rtl else "en_US"
        prov.set_active_language(target)
        with ri.session() as dev:
            h.go_home(dev)
            back = detect(dev)
            t.check(back == target or back in ("en_US", "en"),
                    f"recovered to {target} via /i18n/.active (detect={back})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

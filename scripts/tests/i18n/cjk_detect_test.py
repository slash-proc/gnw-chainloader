#!/usr/bin/env python3
"""CJK (Japanese / Chinese / Korean) language detection + recovery.

Unlike RTL Arabic (cursive, can't segment), CJK is LTR and its characters are
discrete blocks, so the screen READS: detect_language restricts the glyph set to
the candidate label codepoints (a few dozen, so read_rows is fast despite the
31k-glyph CJK font) and ranks by substring matches. This pins the exact language.

Forces ja/zh/ko via the /i18n/.active file write, asserts each is detected
exactly, then recovers to the original language. Reboots + pushes, so L3/slow.

  python3 scripts/tests/i18n/cjk_detect_test.py
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

CJK = ["ja_JP", "zh_CN", "ko_KR"]


def detect(dev):
    return i18n.detect_language(dev, wake=True)[0]


def main() -> int:
    t = h.TestRun("CJK detect + recover")
    prov = provision.Provisioner()

    with ri.session() as dev:
        ok, detail = h.chainloader_running(dev.backend)
        t.require(ok, f"chainloader live ({detail})")
        h.go_home(dev)
        orig = detect(dev)
        t.note(f"original language: {orig}")

    try:
        for code in CJK:
            if not (Path("build") / "i18n" / f"{code}.lang").is_file():
                t.note(f"{code}: pack not built, skipping")
                continue
            prov.set_active_language(code)
            with ri.session() as dev:
                h.go_home(dev)
                got = detect(dev)
                t.check(got == code, f"{code}: detected exactly (got {got})")
    finally:
        target = orig if orig and orig not in CJK else "en_US"
        prov.set_active_language(target)
        with ri.session() as dev:
            h.go_home(dev)
            back = detect(dev)
            t.check(back == target or back in ("en_US", "en"),
                    f"recovered to {target} (detect={back})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

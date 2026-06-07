#!/usr/bin/env python3
"""Per-OFW theme choice persists across a reboot, via SWD.

Theme slots are stored per console in the battery-backed settings word
(TAMP->BKP3R, ui_manager.c theme_persist), so they survive a reset. This asserts
the persistence mechanism over pure SWD (no OCR): write specific Mario/Zelda theme
slots, reboot, and confirm the settings word read back unchanged. The original
word is saved and restored at the end.

(Language persistence is a separate mechanism -- /i18n/.active on LittleFS, not
BKP3R -- and is covered by an OCR-layered i18n test, not here.)

  python3 scripts/tests/theme/theme_persist_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402
from common import provision                  # noqa: E402

TEST_META = dict(tier="L2", subsystem="theme", envs=["ANY"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[4])


def main() -> int:
    t = h.TestRun("theme slots persist across reboot (BKP3R)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")

        original = observe.settings_word(be)
        t.require(original is not None, "read the settings word (BKP3R)")
        fb = observe.settings_fastboot(original)
        lang = observe.settings_lang(original)
        t.note(f"original word 0x{original:08X} (fastboot={fb} lang={lang} "
               f"mario={observe.settings_mario_slot(original)} "
               f"zelda={observe.settings_zelda_slot(original)})")

        prov = provision.Provisioner(backend=be)
        try:
            # Write distinct, valid slots (1=FALLBACK for Mario, 0=DEFAULT for Zelda),
            # preserving fast-boot + language so only the theme fields change.
            prov.set_settings_word(mario_slot=1, zelda_slot=0)
            before = observe.settings_word(be)
            t.check(observe.settings_mario_slot(before) == 1
                    and observe.settings_zelda_slot(before) == 0,
                    f"settings word accepted theme slots mario=1 zelda=0 "
                    f"(0x{before:08X})")

            prov.clean_reboot()
            after = observe.settings_word(be)
            t.check(after is not None and observe.settings_mario_slot(after) == 1
                    and observe.settings_zelda_slot(after) == 0,
                    f"theme slots persisted across reboot (read back 0x{after:08X})")
            t.check(after is not None and observe.settings_fastboot(after) == fb
                    and observe.settings_lang(after) == lang,
                    "fast-boot + language fields were not disturbed by the theme write")
        finally:
            if original is not None:
                with h.time_budget(15.0, "restore settings"):
                    be.halt(); be.write_uint32(observe.TAMP_BKP3R, original); be.resume()
                prov.clean_reboot()
                t.note(f"restored original settings word 0x{original:08X}")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

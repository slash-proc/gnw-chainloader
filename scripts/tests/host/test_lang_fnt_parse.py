#!/usr/bin/env python3
"""L0 host unit test: .lang and .fnt artifact integrity (the cook output).

Parses every build/i18n/<code>.lang (LNG2) and build/i18n/fonts/<script>.fnt
(FNT1) and asserts the cook produced consistent, well-formed artifacts:

  * .lang: LNG2 magic; a uniform strings-ABI across all packs; a uniform string
    count equal to the core STR_ID enum (so no pack is short/long); the embedded
    code matches the filename; the script is a known one; the endonym + script
    match i18n/lang/langs.json (the canonical metadata, not a hard-coded map).
  * .fnt: FNT1 magic; sane glyph count + cell height; loadable by ocr.Font.

Needs build/i18n (run `make i18n`); skips with a note if absent. No device.

  python3 scripts/tests/host/test_lang_fnt_parse.py
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import ocr                        # noqa: E402
from common import i18n_strings as i18n       # noqa: E402
from common.harness import TestRun            # noqa: E402

TEST_META = dict(tier="L0", subsystem="i18n", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[10, 14])

REPO = Path(__file__).resolve().parents[3]
I18N = REPO / "build" / "i18n"
LNG2_MAGIC = 0x32474E4C
FNT1_MAGIC = 0x31544E46
# Scripts the cook PRE-SHAPES into presentation forms (and reorders for RTL) so the
# device draws them with a plain LTR blitter. Their stored endonym is therefore the
# shaped form, which legitimately differs from the base-codepoint endonym in
# langs.json — so we don't byte-compare those, and their .fnt cells run taller.
SHAPED_SCRIPTS = {"arabic"}


def _str0(b):
    return b.split(b"\x00", 1)[0].decode("ascii", "ignore")


def parse_lang(p):
    d = p.read_bytes()
    magic, abi, count, version = struct.unpack_from("<IHHI", d, 0)
    script = _str0(d[0x0C:0x1C])
    code = _str0(d[0x1C:0x2C])
    endonym = d[0x2C:0x4C].split(b"\x00", 1)[0].decode("utf-8", "ignore")
    return dict(magic=magic, abi=abi, count=count, version=version,
                script=script, code=code, endonym=endonym)


def main() -> int:
    t = TestRun("lang/fnt artifact integrity")
    if not I18N.is_dir():
        t.note("build/i18n absent — run `make i18n` first; skipping")
        return t.summary()

    packs = sorted(I18N.glob("*.lang"))
    t.check(len(packs) > 0, f"found {len(packs)} .lang packs")

    meta_by_code = {m["code"]: m for m in i18n.langs_meta()}
    enum_count = len(i18n.english_strings())

    abis, counts = set(), set()
    for p in packs:
        h = parse_lang(p)
        code = p.stem
        ok = h["magic"] == LNG2_MAGIC
        t.check(ok, f"{code}: LNG2 magic" + ("" if ok else f" (got 0x{h['magic']:08X})"))
        t.check(h["code"] == code, f"{code}: embedded code matches filename (got {h['code']!r})")
        abis.add(h["abi"]); counts.add(h["count"])
        lm = meta_by_code.get(code)
        if lm:
            t.check(h["script"] == lm["script"],
                    f"{code}: script matches langs.json ({h['script']!r} vs {lm['script']!r})")
            if lm["script"] in SHAPED_SCRIPTS:
                t.note(f"{code}: endonym is pre-shaped ({h['endonym']!r}); not byte-"
                       f"compared to the base-codepoint langs.json form {lm['endonym']!r}")
            else:
                t.check(h["endonym"] == lm["endonym"],
                        f"{code}: endonym matches langs.json ({h['endonym']!r} vs {lm['endonym']!r})")

    t.check(len(abis) == 1, f"all packs share one strings-ABI (got {sorted(abis)})")
    t.check(len(counts) == 1, f"all packs share one string count (got {sorted(counts)})")
    if counts:
        t.check(enum_count in counts or abs(next(iter(counts)) - enum_count) <= 1,
                f"pack string count {sorted(counts)} matches the core STR_ID enum (~{enum_count})")

    # Fonts.
    fonts = sorted((I18N / "fonts").glob("*.fnt")) if (I18N / "fonts").is_dir() else []
    t.check(len(fonts) > 0, f"found {len(fonts)} .fnt fonts")
    for fp in fonts:
        d = fp.read_bytes()
        magic, gc, H, top, bm = struct.unpack_from("<IHBBI", d, 0)
        t.check(magic == FNT1_MAGIC, f"{fp.name}: FNT1 magic")
        # Shaped scripts (Arabic presentation forms with diacritics) run taller.
        t.check(gc > 0 and 6 <= H <= 24,
                f"{fp.name}: sane glyph count ({gc}) + cell height ({H})")

    # Loadable by the OCR font loader (proves the parser the suite depends on).
    f = ocr.Font.load()
    t.check(len(f.glyphs) > 200, f"ocr.Font.load() parsed {len(f.glyphs)} glyphs")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())

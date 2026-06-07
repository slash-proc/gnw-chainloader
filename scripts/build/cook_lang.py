#!/usr/bin/env python3
"""
cook_lang.py — build i18n language packs from per-locale JSON.

Pipeline:
  reference   write i18n/lang/_reference.json = {STR_id: English} (authoring aid)
  build       for every locale in i18n/lang/langs.json:
                - read i18n/lang/<code>/strings.json ({STR_id: "translation"})
                - emit build/i18n/<code>.lang  (self-describing pack the device reads)
                - accumulate the non-ASCII codepoints per script
              then emit build/i18n_tmp/<script>.chars (font-subset input).
              The device discovers languages from the packs themselves (each carries
              its code + endonym + script), so no compiled-in registry is generated —
              only English is baked into the firmware.

The pack's string-ID order and ABI come from ui/strings.h, so packs and firmware
can never silently disagree (lang_load rejects an ABI mismatch). Missing IDs in a
locale fall back to English on-device.

.lang binary layout (little-endian) — magic 'LNG2', self-describing:
  u32 magic 'LNG2'; u16 abi; u16 str_count; u32 version
  char script[16]; char code[16]; char endonym[32]      (76-byte header)
  u32 offsets[str_count]   (blob offset, or 0xFFFFFFFF = English fallback)
  u8  blob[]               (NUL-terminated UTF-8 strings)
"""
import argparse
import json
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI = os.path.join(REPO, "src", "chainloader", "ui")
LANG_DIR = os.path.join(REPO, "i18n", "lang")
OUT_DIR = os.path.join(REPO, "build", "i18n")
CHARS_DIR = os.path.join(REPO, "build", "i18n_tmp")   # font-subset intermediates (kept out of the SD mirror)
LANG_MAGIC = 0x32474E4C  # 'L','N','G','2' — self-describing pack format v2
FALLBACK = 0xFFFFFFFF


def parse_ids():
    """Return (abi_version, [STR_id...] in enum order) from ui/strings.h."""
    txt = open(os.path.join(UI, "strings.h"), encoding="utf-8").read()
    abi = int(re.search(r"#define\s+STRINGS_ABI_VERSION\s+(\d+)", txt).group(1))
    enum = re.search(r"typedef enum\s*\{(.*?)\}\s*string_id_t", txt, re.S).group(1)
    ids = []
    for line in enum.splitlines():
        m = re.match(r"\s*(STR_[A-Z0-9_]+)", line)
        if not m:
            continue
        name = m.group(1)
        if name == "STR_COUNT":
            break
        ids.append(name)
    return abi, ids


def parse_en():
    """Return {STR_id: English} from ui/strings.c."""
    txt = open(os.path.join(UI, "strings.c"), encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", txt):
        out[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return out


def cmd_reference(_args):
    _, ids = parse_ids()
    en = parse_en()
    os.makedirs(LANG_DIR, exist_ok=True)
    path = os.path.join(LANG_DIR, "_reference.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({i: en.get(i, "") for i in ids}, f, ensure_ascii=False, indent=2)
    print(f"wrote {path} ({len(ids)} ids, abi from strings.h)")


def _make_shaper(script):
    """For an RTL script, return a function that PRE-SHAPES a standalone string to the
    Unicode presentation forms in visual (left-to-right) order, so the device renders
    the bytes as-is with zero runtime shaping/bidi. For LTR scripts, identity.

    reshape() picks the contextual form (isolated/initial/medial/final); get_display()
    (python-bidi) reorders to visual order. Only complete fragments are shaped here;
    runtime label+value+number composition is the UI layer's job (compose_value)."""
    if script != "arabic":
        return lambda s: s
    try:
        import arabic_reshaper
        from bidi.algorithm import get_display
    except ImportError:
        sys.exit("RTL cook needs arabic_reshaper + python-bidi "
                 "(pip install arabic-reshaper python-bidi)")
    return lambda s: get_display(arabic_reshaper.reshape(s))


def build_pack(code, script, endonym, ids, abi, version):
    src = os.path.join(LANG_DIR, code, "strings.json")
    table = json.load(open(src, encoding="utf-8"))
    shape = _make_shaper(script)
    blob = bytearray()
    offsets = []
    for i in ids:
        s = table.get(i)
        if s is None or s == "":
            offsets.append(FALLBACK)
        else:
            offsets.append(len(blob))
            blob += shape(s).encode("utf-8") + b"\x00"
    # Self-describing header (76 B): magic, abi, str_count, version, then fixed
    # script[16] + code[16] + endonym[32]. offsets[] start at byte 76. The device
    # reads code/endonym/script straight from the pack — no compiled-in registry. The
    # endonym is shaped too (RTL) so the selector renders it correctly (no device shaping).
    endo_shaped = shape(endonym)
    endo_b = endo_shaped.encode("utf-8")
    if len(endo_b) > 31:
        sys.exit(f"endonym for {code} is {len(endo_b)} bytes (max 31): {endonym!r}")
    out = bytearray()
    out += struct.pack("<IHHI", LANG_MAGIC, abi, len(ids), version)
    out += script.encode("ascii")[:15].ljust(16, b"\x00")
    out += code.encode("ascii")[:15].ljust(16, b"\x00")
    out += endo_b.ljust(32, b"\x00")
    out += struct.pack(f"<{len(ids)}I", *offsets)
    out += blob
    dst = os.path.join(OUT_DIR, f"{code}.lang")
    with open(dst, "wb") as f:
        f.write(out)
    translated = sum(1 for o in offsets if o != FALLBACK)
    print(f"  {code:8} script={script:8} v{version} {translated}/{len(ids)} translated  -> {dst} ({len(out)} B)")
    # Codepoints needing the external font (>= 0x80), from the SHAPED text (presentation
    # forms for RTL) so the font is subset to exactly what the device draws, plus the
    # shaped endonym (the selector draws it in this language's font).
    # Iterate the canonical ids (not table.values()) so non-string meta keys such as
    # "_adopted" (a list) are skipped — the same keys build_pack ignores above.
    cps = {ord(c) for i in ids for c in shape(table.get(i) or "") if ord(c) >= 0x80}
    cps |= {ord(c) for c in endo_shaped if ord(c) >= 0x80}
    return cps


def cmd_build(_args):
    abi, ids = parse_ids()
    langs = json.load(open(os.path.join(LANG_DIR, "langs.json"), encoding="utf-8"))
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(CHARS_DIR, exist_ok=True)
    by_script = {}
    print(f"abi={abi}, {len(ids)} string ids")
    for lg in langs:
        # Build a pack for any manifest code that has an i18n/lang/<code>/strings.json.
        # en_US/en_UK are real packs now (mixed-case English that overrides the all-caps
        # in-core fallback); the reserved "en" sentinel is in-core only and is not in the
        # manifest, so it is never cooked. A manifest entry with no strings.json is skipped.
        if not os.path.isfile(os.path.join(LANG_DIR, lg["code"], "strings.json")):
            print(f"  {lg['code']:8} (no strings.json — skipped)")
            continue
        cps = build_pack(lg["code"], lg["script"], lg["endonym"], ids, abi, lg.get("version", 1))
        by_script.setdefault(lg["script"], set()).update(cps)
    for script, cps in by_script.items():
        chars = "".join(chr(c) for c in sorted(cps))
        cf = os.path.join(CHARS_DIR, f"{script}.chars")
        with open(cf, "w", encoding="utf-8") as f:
            f.write(chars)
        print(f"  {script}.chars: {len(cps)} codepoints -> {cf}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("reference").set_defaults(func=cmd_reference)
    sub.add_parser("build").set_defaults(func=cmd_build)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

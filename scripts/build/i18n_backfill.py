#!/usr/bin/env python3
"""i18n_backfill.py: keep every language pack complete, and report what still needs translating.

ONE job, and nothing else: for each target language, any STR_* id that is MISSING from its
strings.json gets added with the in-core English text (ui/strings.c) as a visible placeholder.
It NEVER overrides a value the file already has.

Translations are NOT stored in this script. They live in i18n/lang/<code>/strings.json and are
owned there, by a human or an LLM. Do not add translation tables, brand maps, locale/decimal
maps, or any per-language values to this file: that belongs in the language files. This tool only
fills gaps with the English baseline so a translator can see them and replace them.

Workflow when you add a UI string:
  1. Append the STR_* id to ui/strings.h (before STR_COUNT) and its English text to ui/strings.c.
  2. Run this. Every target language gains the English placeholder for the new id.
  3. Translate those placeholders in the language files. The report below lists exactly which
     ids in which languages still show English, so a human or an LLM knows what to translate.

  python3 scripts/build/i18n_backfill.py          # add placeholders + report
  python3 scripts/build/i18n_backfill.py --check   # report only (no writes)

en_US / en_UK are skipped: English IS the baseline, so there is nothing to backfill into them.
"""
import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI = os.path.join(REPO, "src", "chainloader", "ui")
LANG_DIR = os.path.join(REPO, "i18n", "lang")

# The English reference packs. English is the baseline placeholder source, so there is nothing
# to backfill into them. Every other language is a target whose strings.json owns its translations.
SKIP = {"en_US", "en_UK"}


def canonical_ids():
    """STR_* ids in enum order, from ui/strings.h (the wire order packs are cooked against)."""
    txt = open(os.path.join(UI, "strings.h"), encoding="utf-8").read()
    enum = re.search(r"typedef enum\s*\{(.*?)\}\s*string_id_t", txt, re.S).group(1)
    ids = []
    for line in enum.splitlines():
        m = re.match(r"\s*(STR_[A-Z0-9_]+)", line)
        if m and m.group(1) != "STR_COUNT":
            ids.append(m.group(1))
    return ids


def english_baseline():
    """{STR_id: English text} from the in-core table ui/strings.c. This is the placeholder a
    target language receives for any id it has not translated yet."""
    txt = open(os.path.join(UI, "strings.c"), encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", txt):
        out[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report only; do not add placeholders")
    args = ap.parse_args()

    ids = canonical_ids()
    english = english_baseline()
    langs = json.load(open(os.path.join(LANG_DIR, "langs.json"), encoding="utf-8"))
    total = len(ids)
    grand = 0

    print("i18n backfill: missing ids get the English placeholder; existing translations are "
          "never touched.\n")
    for lg in langs:
        code = lg["code"]
        if code in SKIP:
            continue
        path = os.path.join(LANG_DIR, code, "strings.json")
        existing = json.load(open(path, encoding="utf-8")) if os.path.isfile(path) else {}
        idset = set(ids)

        # "_adopted": ids this language deliberately keeps in English (a loanword like Firmware
        # in German, a product name, or a word/separator simply identical to English). It is a
        # meta key, not a string: the cook ignores it (it reads by STR_ id), and the TODO report
        # below skips these so an intentional English value is not nagged as "untranslated". The
        # adoption decision lives in the language file, per language, never in this script.
        adopted = [i for i in existing.get("_adopted", []) if i in idset]
        adoptset = set(adopted)

        # Build the pack in canonical id order: keep the language's own value where it has one,
        # fill any gap with the English placeholder, never override. Stale ids (present in the
        # file but no longer in strings.h) are dropped by only iterating `ids`.
        out = {}
        for i in ids:
            have = existing.get(i, "").strip()
            out[i] = existing[i] if have else english.get(i, "")
        if adopted:
            out["_adopted"] = adopted   # preserve the meta key (cook skips it)

        if not args.check:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2)
                f.write("\n")

        # Untranslated = value still equals the English baseline AND not declared _adopted.
        todo = [i for i in ids if out.get(i, "") == english.get(i) and i not in adoptset]
        grand += len(todo)
        extra = f"   [{len(adopted)} adopted]" if adopted else ""
        if todo:
            print(f"  {code:6} {total - len(todo)}/{total} translated   TODO({len(todo)}): "
                  f"{', '.join(todo)}{extra}")
        else:
            print(f"  {code:6} {total}/{total} translated{extra}")

    print(f"\n{grand} string(s) across all target languages still show English. Translate them in "
          f"i18n/lang/<code>/strings.json.")
    print('(A string a language keeps in English on purpose, e.g. an adopted loanword or a word '
          'identical to English, goes in that file\'s "_adopted" list and is excluded above.)')
    return 0


if __name__ == "__main__":
    sys.exit(main())

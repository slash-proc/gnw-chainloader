#!/usr/bin/env python3
"""One-time: give every language a context-aware STR_LAUNCH verb in its
i18n/lang/<code>/strings.json, replacing the obsolete per-target boot keys
(STR_BOOT_RETROGO / STR_BOOT_OFW) left over from before the unified boot selector.

The English default lives in ui/strings.c ("LAUNCH"); these are the natural
"launch / start this system" verb per language, chosen for fit and nuance rather
than a literal calque of "launch". Run once, then `make i18n` re-cooks the packs
(and the CJK font subsets) from the updated strings.
"""
import pathlib
import re
import sys

# Natural "launch / start this system or firmware" verb per language.
VERBS = {
    "de_DE": "Starten",    # launch software
    "fr_FR": "Lancer",     # lancer un jeu / une appli
    "es_ES": "Iniciar",    # iniciar
    "it_IT": "Avvia",      # avviare
    "pt_PT": "Iniciar",    # iniciar
    "nl_NL": "Starten",    # starten
    "pl_PL": "Uruchom",    # launch / run (imperative)
    "ro_RO": "Pornește",   # start (imperative)
    "ru_RU": "Запуск",     # launch (noun, common UI form)
    "uk_UA": "Запуск",     # launch
    "el_GR": "Εκκίνηση",   # startup / launch
    "ja_JP": "起動",        # boot / launch a system
    "zh_CN": "启动",        # start / launch
    "zh_TW": "啟動",        # start / launch (traditional)
    "ko_KR": "실행",        # run / execute / launch
}

LANG_DIR = pathlib.Path(__file__).resolve().parents[2] / "i18n" / "lang"
# the two obsolete keys, consecutive (any translation value)
OBSOLETE_PAIR = re.compile(
    r'[ \t]*"STR_BOOT_RETROGO":[^\n]*\n[ \t]*"STR_BOOT_OFW":[^\n]*\n')
# a lone obsolete key, if they are not adjacent
OBSOLETE_LONE = re.compile(r'[ \t]*"STR_BOOT_(?:RETROGO|OFW)":[^\n]*\n')


def main():
    missing = [c for c in VERBS if not (LANG_DIR / c / "strings.json").is_file()]
    if missing:
        sys.exit(f"missing strings.json for: {missing}")
    for code, verb in VERBS.items():
        path = LANG_DIR / code / "strings.json"
        text = path.read_text(encoding="utf-8")
        launch = f'  "STR_LAUNCH": "{verb}",\n'
        if '"STR_LAUNCH"' in text:
            text = OBSOLETE_LONE.sub("", text)      # idempotent: just clean up
            print(f"  {code}: STR_LAUNCH already present; cleaned obsolete keys")
        else:
            new, n = OBSOLETE_PAIR.subn(launch, text, count=1)
            if n == 0:                              # not adjacent: drop + insert
                new = re.sub(r'(\{\s*\n)', r'\1' + launch,
                             OBSOLETE_LONE.sub("", text), count=1)
            text = new
            print(f"  {code}: STR_LAUNCH = {verb}")
        path.write_text(text, encoding="utf-8")
    print(f"done ({len(VERBS)} languages)")


if __name__ == "__main__":
    main()

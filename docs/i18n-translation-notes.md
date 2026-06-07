# i18n translation notes

Companion to the language packs in `i18n/lang/<code>/strings.json`. It records the
translation conventions applied during the naturalization pass and flags the choices that
are best-effort and want a native-speaker review. English (`en_US`/`en_UK`) and German
(`de_DE`) were done at full confidence (the maintainer reads both); everything else is
expert best-effort and is listed here as a review checklist, not a finished authority.

## Conventions applied across all languages

- **Width budgets** (font approx. 6 px/char, screen 320 px), used to keep text off the
  marquee scroll: progress **title** approx. 22 chars; progress **status** approx. 44 chars
  (clips, no scroll); main-menu row / value selector approx. 48 chars; split-list label
  (file browser / partition list) approx. 27 chars; partition **detail value** approx. 18
  chars; footer approx. 50 chars (centered). CJK glyphs are roughly one cell each, so the
  detail-value budget is approx. 9 CJK characters.
- **Format strings.** `STR_PREPARING` (`%s`), `STR_WRITING` (`%d`), `STR_MOUNT_FAIL` (`%d`),
  `STR_LFS_BLOCKS` (`%d`), `STR_FROG_ENTRIES` (`%d`) carry a placeholder spliced at runtime.
  Each language places the placeholder in natural word order. Size units stay `KB`/`MB`
  (English, matching `format_size` elsewhere); they are not localized.
- **`Pause:` button token** in footers is written mixed-case to match `en_US`, even though
  PAUSE is a physical button label.
- **Yes/No** rows were lowered from all-caps to the language's natural mixed case.
- **`_adopted`** lists the ids a language deliberately keeps identical to English so the
  backfill TODO report does not nag them: always `STR_DETAIL_CHAINLOADER` (the
  `GNW-Chainloader` brand) and `STR_SD_ADDR` (`SD`); plus `STR_TYPE_FIRMWARE` where the
  language uses the loanword "Firmware", and `STR_PHASE_MODULES`/`STR_N_MODULES` where
  "Modules"/"modules" is identical (fr, nl); CJK keep `STR_DECIMAL_SEP` (`.`).

## German (de_DE) -- maintainer-reviewed

- Progress text uses informative **verbal nouns with no trailing `...`** (the maintainer's
  stated preference): `Kopiervorgang`, `Loeschvorgang`, `Berechnung`, `Schreibvorgang`,
  `Ext-Flash-Loeschung` / `Int-Flash-Loeschung`. The earlier machine draft used the
  imperative (`Kopiere Datei...`), which reads like a command; that was the original report.
- `STR_PREPARING` = `%s wird vorbereitet` (passive reads naturally with the leading name).
- Footers de-abbreviated (`Pause: Optionen   A: Waehlen`), `Abbruch` -> `Abbrechen` for
  consistency with the other infinitive context actions, ` [LAD]` -> ` [Laedt]`.
- "mount" kept as the German tech loanword (`gemountet`, `Mount-Fehler`); the formal
  alternative is `einbinden`/`Einbindung`. Flag if the formal register is preferred.

## Other languages -- best-effort, review checklist

Existing strings were largely sound (their progress idiom was already a gerund/verbal noun,
not the German imperative artifact), so the pass focused on the 14 new detail/phase ids, the
3 restructured format strings, mixed-case Yes/No, and de-abbreviated footers. Specific
choices to sanity-check:

- **Romance (fr/es/it/pt):** `STR_DETAIL_OFW_BACKUP` rendered as "<backup> firmware"
  (`Sauv. firmware`, `Copia firmware`, `Backup firmware`, `Copia firmware`); `STR_DETAIL_FS`
  abbreviated to fit (`Sys. fichiers`, `Sist. archivos`, `File system`, `Sist. ficheiros`);
  `STR_DETAIL_ASSETS` = resources (`Ressources`/`Recursos`/`Risorse`/`Recursos`);
  `STR_DETAIL_APP_BIN` = program. Confirm the abbreviations read naturally.
- **Slavic (ru/uk/pl):** block/entry plurals are labels in the nominative
  (`Bloki`/`Wpisy`/etc.) rather than grammatically agreeing with `%d`; acceptable for a
  fixed label but a native may prefer a count-agnostic noun. `STR_PHASE_INT_FLASH`/`EXT`
  abbreviated (`Vnutr. flesh`, `Flash wewn.`).
- **Greek (el):** `STR_FROG_ENTRIES` abbreviated (`Katachor.`); `firmware` kept as loanword.
- **Romanian (ro):** ș/ț (comma-below) used throughout; `STR_ERASE` is `Sterge tot` vs
  `STR_DELETE` `Sterge` to distinguish flash-erase from file-delete.
- **CJK (ja/zh_CN/ko/zh_TW):** the new detail terms are the least-certain. Assets =
  `アセット`/`资源`/`에셋`/`資源`; OFW backup = `FWバックアップ`/`固件备份`/`펌웨어 백업`/`韌體備份`;
  app image = program (`プログラム`/`程序`/`프로그램`/`程式`). Verify these read idiomatically and
  fit the approx. 9-glyph detail column.
- **Arabic / Farsi (ar/fa_IR):** lowest confidence. Text is authored in logical order and
  reshaped to visual order by `cook_lang.py` (no runtime shaping). Farsi compounds follow the
  existing pack's orthography (ZWNJ often omitted, e.g. `gozinehha`); a native may want proper
  ZWNJ. `STR_WRITING`/`STR_LFS_BLOCKS` etc. mix a Latin `%d`/`KB` run into RTL text -- check
  the digit lands on the expected side (same mechanism as the pre-existing `STR_FILE_N`).

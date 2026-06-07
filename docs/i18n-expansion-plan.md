# i18n Expansion Plan: English variants, Arabic + RTL, and full string coverage

Status: COMPLETE. Phases A through D landed on branch `i18n-en-ar-rtl` and
hardware-verified: real English packs + the string sweep + ABI 3->4 (A); the RTL UI
mirror + `gui_api_t` module GUI services (B); Arabic + Farsi with offline reshape/bidi,
all 19 packs translated 92/92, the gap-filler backfill with per-file `_adopted`, the
GAME theme-preview button, and `STR_DECIMAL_SEP` (C); and on-demand non-Latin font
loading via an AUX slot in `font_ext` (D). This is the granular checklist; it
complements the overarching view in [ACTIVE_WORK.md](../ACTIVE_WORK.md) (Goal 14) and
defers all architecture and format detail to [docs/i18n.md](i18n.md) and
[DESIGN.md](../DESIGN.md) rather than restating it.

The work items:

1. Real `en_US` and `en_UK` packs that override the baked all-caps ASCII English. *(done)*
2. Arabic support, including a right-to-left UI mirror. *(RTL = Phase B; Arabic = Phase C)*
3. An exhaustive sweep of UI strings that are still hardcoded. *(done)*
4. On-the-fly non-Latin font loading (Phase D): any active language renders CJK/Arabic
   glyphs when the script `.fnt` is present, generalizing the always-resident Latin base.
   Surfaced when language switching was made live-on-scroll (an A-press preview can't show
   a language whose font isn't loaded yet); lives in the language module's `font_ext`.
   *(done: a third AUX slot in `font_ext.c` loads a script font on demand by Unicode
   range, cached by path. Renders cooked non-Latin text under any active language, e.g. a
   CJK filename in the browser. Raw Arabic filenames remain unsupported, since the Arabic
   font carries only pre-shaped presentation forms and there is no runtime shaping.)*

Phase A decisions made during implementation: language switching stays **live on
LEFT/RIGHT** (an A-press-to-commit variant was tried and reverted, since it left
non-Latin endonyms unrendered until commit); the Language row is **first** in Settings;
the selector shows **`endonym (code)`** (e.g. `Deutsch (de_DE)`), which also disambiguates
en_US/en_UK and gives the OCR an ASCII handle; the in-core English sentinel is the
reserved code `en`, hidden from the selector once a real English pack exists.

Engineering invariant for all of it: stability is law. The menu must stay reachable and
the in-core English/ASCII fallback must keep working in every degraded state (no pack,
no font, corrupt asset, empty filesystem).

## Background

The i18n stack is mature (see [docs/i18n.md](i18n.md)): strings sit behind `tr()`,
languages are data-driven `.lang` packs discovered on the filesystem, per-script `.fnt`
fonts render UTF-8, and the whole implementation lives in the PIE language module
(`src/drivers/language/`) behind a thin core shim (`src/chainloader/ui/i18n.c`). Three
gaps remain before the UI is premium in every language:

- English is only the baked all-caps ASCII fallback. There is no mixed-case English
  pack. `langs.json` lists `en_US`, but `cook_lang.py` skips it, so today it is a no-op
  alias for the in-core fallback. There is no `en_UK`.
- No right-to-left language is supported. [docs/i18n.md](i18n.md) lists RTL/bidi/shaping
  as out of scope.
- A code sweep found roughly a dozen user-visible strings still hardcoded (file browser,
  partition viewer, theme names, the language-install notice).

## Decisions

These were settled before planning and drive the choices below:

- Hide the all-caps English from the live selector once a real English pack exists; keep
  it only as the silent fallback.
- Do a full RTL mirror, including the split-pane screens (file browser, partition
  viewer), not just the simple lists.
- Draft translations for everything now. Non-authored translations are machine-drafted
  and flagged for native review; Arabic is Modern Standard Arabic.
- Give `en_UK` a genuine British register, not just a different endonym.

Do the work on a feature branch off `main` (large, multi-subsystem; project rule).

## Architecture touchpoints (verified as of this writing)

Detail lives in [docs/i18n.md](i18n.md); the specific code seams this plan touches:

- Strings: `string_id_t` enum + `STRINGS_ABI_VERSION` in `src/chainloader/ui/strings.h`;
  in-core English table `i18n_en[]` + `tr()` in `strings.c`.
- Packs: `.lang` (magic `LNG2`, self-describing header) built by
  `scripts/build/cook_lang.py build` from `i18n/lang/<code>/strings.json` + the manifest
  `i18n/lang/langs.json`. ABI gate is core-authoritative (`src/common/abi.h`
  `pack_abi_ok`); a mismatched pack is skipped and that language falls back to English.
- Discovery/selection in the module `src/drivers/language/lang_mgr.c`: `i18n_init` scans
  `/i18n/*.lang`, pins an in-core English at `g_langs[0]`, sorts the rest by code;
  `restore_active` reads `/i18n/.active` and activates it; persist-by-code on Settings
  exit. Shim entry points in `src/chainloader/ui/i18n.c` delegate through the
  `lang_api_t` vtable (`src/chainloader/system/language.h`).
- Fonts: per-script `.fnt` (magic `FNT1`) built by `scripts/build/cook_font.py blob`
  from `i18n/fonts/fusion-pixel-12px-monospaced-<script>.otf`. `cook_lang.py` collects
  each script's non-ASCII codepoints into `build/i18n_tmp/<script>.chars` and the font
  is subset to exactly those.
- Rendering is left-anchored and left-to-right everywhere: `gui_draw_text` /
  `gui_draw_text_marquee` (`gui.c`); `gui_text_width` / `gui_glyph` / UTF-8 decode
  (`gui_text.c`, HAL-free and host-testable); the menu list renderer `ui_list_draw`
  (`ui/ui_list.c`, cursor drawn at `x+6` via `gui_draw_selector` which draws `>`,
  right-edge scrollbar, optional right pane at `x+188`); the header
  (`ui/ui_manager.c` `ui_draw_header`, title left, battery right); modals (text inset at
  `self->x + 20`). `gui_draw_sprite` already has a `flip_h` parameter.
- Value-selectors are composed with `strcat` in `menu.c`
  (`tr(label) + ": < " + value + " >"`). Number formatting uses `int_to_str` /
  `format_size` from `system/utils.c` plus `strcat`; there is no `printf`.

## Cross-cutting change: ABI bump 3 to 4

Appending new `STR_*` ids changes the pack wire format, so bump `STRINGS_ABI_VERSION`
from 3 to 4 in `strings.h`.

- [ ] Bump `STRINGS_ABI_VERSION` to 4.
- [ ] Add in-core English defaults in `i18n_en[]` for every new id, so the fallback is
      always populated.
- [ ] Re-cook all packs (`make i18n`) and re-deploy (`make push_i18n`, or refresh the SD
      `/i18n/`). Stale ABI-3 packs are rejected on device and that language shows English
      until refreshed. This is by design (the gate prevents garbled mixed-ABI text) and
      is safe.
- [ ] Refresh `i18n/lang/_reference.json` with `cook_lang.py reference`.

## Task 1: en_US and en_UK as real packs

Goal: proper mixed-case English that overrides the all-caps fallback; default to en_US on
a fresh device; respect an explicit en_UK choice forever; hide the raw all-caps English
from the selector once a real English pack exists.

Pack sources:

- [ ] Create `i18n/lang/en_US/strings.json` with all ids in mixed-case American English
      (for example `STR_LAUNCH` = "Launch", `STR_POWER_OFF` = "Power Off",
      `STR_TITLE_SETTINGS` = "Settings").
- [ ] Create `i18n/lang/en_UK/strings.json` with a real British register: British
      spellings and phrasing wherever they apply across current and new ids (for example
      "Initialise", "Customise", "Cancelled", "Centre", "Colour" if introduced, British
      capitalisation and wording choices). Both packs use `script` = `latin` (no new
      font).

Manifest `i18n/lang/langs.json`:

- [ ] Change the `en_US` endonym to `English (US)`.
- [ ] Add `{ "code": "en_UK", "english": "English (UK)", "endonym": "English (UK)",
      "script": "latin" }`. Distinct endonyms are required: they disambiguate the two in
      the selector and in the OCR detector.

Cook pipeline `scripts/build/cook_lang.py`:

- [ ] Remove the `en_US` skip (currently a `continue` near line 121). New rule: build a
      pack for any code that has an `i18n/lang/<code>/strings.json` directory, so en_US
      and en_UK cook into `build/i18n/en_US.lang` and `build/i18n/en_UK.lang`.

Module `src/drivers/language/lang_mgr.c` (selector model and defaulting):

- [ ] Rename the in-core fallback entry so it cannot collide with a real `en_US` pack: in
      `set_english_entry`, give `g_langs[0]` a reserved code such as `en` (endonym
      "English"). Change the `discover_cb` drop-guard to drop only that reserved `en`
      sentinel, so `en_US` and `en_UK` packs are discovered as normal sorted entries.
- [ ] Hide the sentinel from the visible selector when a real English pack exists. Add a
      predicate (the sentinel is visible only if no discovered pack has a code prefixed
      `en_`) and make the selector-facing entry points honor it: `i18n_count`,
      `i18n_cycle` (skip hidden), `i18n_endonym`, and any index-to-display mapping. The
      sentinel always remains the internal fallback target for `tr()` and stays reachable
      when no pack is present.
- [ ] Default to en_US: in `restore_active`, when `/i18n/.active` is absent or empty,
      resolve in order en_US, then en_UK, then the sentinel (via `find_code`). Explicit
      selections are already persisted and restored by code, so en_UK persists across
      reboot unchanged. Confirm the `i18n_rediscover` to `restore_active` path picks up
      en_US once a background SD install adds the pack.

## Task 2: Arabic with a full RTL mirror

Approach: pre-shape Arabic at build time so the device renders it left-to-right with zero
runtime shaping or bidi, then mirror the UI layout for RTL languages.

### 2a. Font (verify first; this gates the task)

Fusion Pixel has no Arabic. Primary choice: Unixel
(github.com/MDarvishi5124/Unixel), an SIL OFL 1.1 English-Arabic/Persian pixel font
inspired by Unifont (supplied as OTF or TTF). Persian coverage includes all base Arabic
letters, so Modern Standard Arabic is covered. Fallback: the Eternal Dream Arabization
Arabic pixel font (OFL-1.1).

- [ ] Add the chosen font under `i18n/fonts/` with its OFL license alongside the existing
      `OFL.txt` / `LICENSE/`. `cook_font.py` loads via PIL `ImageFont.truetype`, which
      accepts TTF or OTF, so no specific format is required as long as it rasterizes
      cleanly on its pixel grid.
- [ ] Pre-flight check 1, presentation-form cmap coverage. The simplest pipeline (2b)
      feeds Unicode presentation-form codepoints, so the font cmap should map Forms-B
      (U+FE70 to U+FEFF) and ideally Forms-A (U+FB50 to U+FDFF). `cook_font.py` drops any
      codepoint not in the cmap, so missing forms silently render as `?`. Unifont (the
      basis for Unixel) maps the full BMP including both blocks, so Unixel most likely
      inherits them; verify with `TTFont(path).getBestCmap()`. If the font instead
      carries only base Arabic (U+0600 to U+06FF) plus OpenType GSUB shaping, use the
      font-agnostic fallback pipeline below.
- [ ] Pre-flight check 2, pixel grid and cell height. Unifont-derived fonts often use a
      16px grid while the Latin/CJK cell is 12px. This is not a blocker: each `.fnt` blob
      carries its own `cell_h` and `ref_top`, the renderer aligns external blobs by
      `ref_top - GUI_FONT_REF_TOP` (`gui_text.c` / `font_ext.c`), and menu rows are 20px
      tall, so a taller Arabic cell fits and can improve legibility. Cook Arabic at its
      native ppem via `cook_font.py` `--style`/PPEM, and confirm with
      `cook_font.py probe --script arabic` that the inked band (dots above and below the
      baseline) is not clipped.
- [ ] `scripts/build/cook_font.py`: special-case the `arabic` script source filename in
      `font_path` / `load` (the family differs from Fusion Pixel) and allow a per-script
      ppem, leaving the other scripts untouched.

Font-agnostic fallback pipeline, only needed if the chosen font lacks presentation forms
in its cmap: instead of arabic_reshaper, shape at cook time with HarfBuzz (uharfbuzz) in
RTL direction to get visual-order glyph indices, assign each used glyph index a stable
Private-Use codepoint, emit those PUA codepoints into the pack (visual order), and cook
the Arabic `.fnt` by rasterizing those glyph indices directly (freetype load-by-index)
keyed to the same PUA codepoints. The device still renders left-to-right with zero
shaping; this decouples the build from the font cmap structure entirely. It is more
cook-side code, so prefer the presentation-form path when available.

### 2b. Offline shaping in cook_lang.py

- [ ] Detect RTL by `script == "arabic"` in `build_pack`. For each translation string,
      emit `get_display(arabic_reshaper.reshape(s))` into the blob instead of the raw
      string: `reshape` selects the correct presentation forms (isolated, initial,
      medial, final) and `get_display` (python-bidi) reorders to visual left-to-right
      order. The device then renders the bytes as-is.
- [ ] Shape the Arabic endonym as well (the selector draws it), so its presentation-form
      codepoints land in the font subset. `cook_lang.py` already adds endonym codepoints;
      confirm it shapes them for RTL.
- [ ] The codepoint-collection step then gathers presentation-form codepoints
      automatically; they feed `arabic.fnt`.
- [ ] Add `arabic_reshaper` and `python-bidi` imports, guarded so non-RTL cooks do not
      require them but an RTL cook errors clearly if they are absent.

Mixed-content rule: `get_display` orders a complete standalone fragment only. The device
composes label + value + number at runtime with `strcat`, which the shaper cannot see.
Pre-shape only the standalone translatable fragments and handle composition and order at
the UI layer (2c). Western digits stay left-to-right inside a right-anchored row, which
is correct Arabic behaviour.

### 2c. Full RTL UI mirror

Plumb a single RTL flag from the active language to the renderer:

- [ ] `lang_mgr.c`: set `g_rtl` when the active script is `arabic`; expose
      `i18n_is_rtl()`. Add `bool (*rtl)(void)` to `lang_api_t`
      (`system/language.h`), wire it in `driver_entry.c`, add the shim `i18n_is_rtl()` in
      `ui/i18n.c` and its declaration in `ui/i18n.h`. Extending the vtable is safe because
      the ABI bump rebuilds core and module together.
- [ ] `gui.c` / `gui.h`: add a plain global `bool gui_rtl` (set on language change in
      `menu_apply_language`), mirroring how `gui_bg_color` and friends are globals. Add
      two helpers: `int gui_mirror_x(int x, int w)` returning `SCREEN_WIDTH - x - w` when
      `gui_rtl` else `x`, and a right-anchoring text helper (or an alignment flag on the
      marquee path). Glyphs are never pixel-flipped; only layout and directional chrome
      flip.

Apply the mirror at every draw surface (full mirror):

- [ ] Menu list `ui/ui_list.c` `ui_list_draw`: right-align rows (anchor at the right edge
      using `gui_text_width`), move the selector cursor to the right and render `<` instead
      of `>` (the one place the flip concept applies to a glyph), move the scrollbar to the
      left edge, and mirror the split geometry (list and right pane swap sides; divider at
      the mirrored x). Dividers are already centered.
- [ ] Header `ui/ui_manager.c` `ui_draw_header`: title right-aligned, battery on the left.
- [ ] Modals (`confirm_draw`, `error_draw`, the context menu, the notice): mirror the text
      inset (`self->x + 20` becomes a right inset) and any internal layout.
- [ ] Split-pane screens: audit and mirror the absolute x-coordinates in
      `ui/partition_viewer.c` (right detail pane around x=198, separator at x+188, size and
      label columns) and `ui/ui_file_browser.c` (tab headers, the right detail pane, the
      scrollbar, the path/title row). Route them all through `gui_mirror_x` rather than
      editing each literal ad hoc.
- [ ] Footer is centered; no change.
- [ ] Value-selector composition: centralize the four `strcat` sites (`menu.c`
      `get_main_label` and `get_settings_label` x3) into one helper
      `compose_value(label, value, buf)` that builds `label: < value >` for LTR and the
      correctly framed RTL form (flipped bracket order) when `gui_rtl`. This becomes the
      single RTL-aware composition point and is also where Task 3's placeholder splicing
      lives.

### 2d. Manifest and dialects

- [ ] Add one Modern Standard Arabic entry:
      `{ "code": "ar", "english": "Arabic", "endonym": "<MSA endonym>",
      "script": "arabic" }`. MSA is the universal written form across all Arabic regions;
      applications localize to MSA, not to spoken dialects, so a single `ar` locale is
      correct (the same choice Apple, Google, and Microsoft make).
- [ ] Makefile `i18n` target: add
      `cook_font.py blob --script arabic --chars build/i18n_tmp/arabic.chars`. Add
      `arabic_reshaper` and `python-bidi` to the host/CI requirements.

## Task 3: exhaustive untranslated-string sweep

Append new ids to `string_id_t` (before `STR_COUNT`) and add their in-core English
defaults in `strings.c`. Convert these user-visible literals to `tr()`:

- [ ] File browser (`ui/ui_file_browser.c`): "SELECT FS", "DIR", "FILE", "UNKNOWN", the
      "FILE BROWSER" title (reuse the existing `STR_FILE_BROWSER`), the "FILE N" status
      (placeholder form), the "LITTLEFS" display label, and the "RW"/"RO" mode words. Do
      not force-uppercase translated output: the manual uppercasing loop must not apply to
      `tr()` results.
- [ ] Partition viewer (`ui/partition_viewer.c`): the divider inner labels "INTFLASH",
      "EXTFLASH", "SD CARD" via new ids, keeping the leading `-` sentinel. Build
      `"-" + tr(...) + "-"` into a static buffer per divider so the divider detection in
      `ui_list.c` still works and the buffers outlive the draw.
- [ ] Theme names (`ui/ui_manager.c` `ui_theme_slot_name`): "DEFAULT" and "FALLBACK"
      (UI words, not proper nouns).
- [ ] Notice title (`ui/ui_manager.c` `ui_show_notice`): "LANGUAGES" via a new id.
- [ ] Language-install summary (`menu.c` `menu_notify_langs`): "Updated N languages" via a
      placeholder template.
- [ ] OFW progress titles (`menu.c` `boot_selected_target`): the word "OFW" in
      "Mario OFW" / "Zelda OFW" via a `"%s OFW"` template. MARIO and ZELDA stay literal.

Explicitly excluded (stay literal): proper nouns MARIO, ZELDA, RETRO-GO; structural
punctuation (`": < "`, `" >"`, `/`, `:`); raw filesystem type codes (FAT, LittleFS as the
partition type); `"BANK"` plus digit, `"SD "`, `"EXT "`, `"@ 0x"`; button letters.

Placeholder convention (there is no printf):

- [ ] Add a single-placeholder splicer in `system/utils.c`, for example
      `str_fmt1_int(dst, tmpl, n)` and `str_fmt1_str(dst, tmpl, s)`, that copy `tmpl` and
      splice at the first `%d` / `%s` (using `int_to_str` for `%d`). Templates like
      "Updated %d languages", "File %d", and "%s OFW" let each language place the value
      correctly (German puts the count mid-sentence; a prefix-plus-suffix `strcat`
      cannot). Apply at `menu_notify_langs`, the file-browser file-count status, and the
      OFW titles. These splicers plus `compose_value` are the only runtime
      string-composition points, which keeps RTL ordering tractable.

## Translations

After the ids exist and the ABI is bumped:

- [ ] Author the complete `en_US` and `en_UK` packs (Task 1).
- [ ] Author a complete Arabic (MSA) `strings.json` in source form; the cook shapes it.
- [ ] Add every new `STR_*` id to all existing language `strings.json` files with drafted
      translations.
- [ ] Flag every machine-drafted translation as pending native review (in the JSON or a
      tracking note). English fallback remains for anything left blank, so no wrong text
      is ever forced on screen.
- [ ] Re-run `cook_lang.py reference` to refresh `_reference.json`.

## Tests and OCR

Keeping tests aligned with the code is a project rule. Host tests via `make test_host`,
device tests under `scripts/tests/` built on `remote_input` + `ocrnav` + `harness`.

Host tests:

- [ ] Round-trip a cooked `en_US.lang`: parse the header and assert all offsets are
      non-fallback (en_US fully overrides the in-core English).
- [ ] Feed pre-shaped Arabic presentation-form bytes through `gui_utf8_next` /
      `gui_glyph` / `gui_text_width` against a host-loaded `arabic.fnt`; assert decode and
      width resolve (proves the no-runtime-shaping path).
- [ ] Regenerate the ABI self-test fixtures for ABI 4 (an ABI-3 pack is rejected, an
      ABI-4 pack accepted) in the existing `test_abi_gate.c` / `test_abi_reject.py` flow.

OCR detector fixes (`scripts/common/i18n_strings.py`,
`scripts/tests/verify_entry_translated_in_all_languages.py`):

- [ ] Build candidate codes from the real `i18n/lang/*` directories plus the `en`
      sentinel, and de-duplicate. The old `["en_US"] + dirs` now double-counts en_US.
- [ ] en_US and en_UK share the SETTINGS label, so disambiguate by endonym
      ("English (US)" versus "English (UK)") when two candidates collide on the label.

Device tests:

- [ ] Fresh-`.active` default: with no `/i18n/.active`, assert the menu renders the
      mixed-case en_US "Settings" (distinguishes the pack from the all-caps in-core
      "SETTINGS").
- [ ] en_UK persists: cycle Language to en_UK (detect by endonym), exit Settings (commits
      `.active`), power-cycle, assert en_UK is still active.
- [ ] Selector hides the sentinel: with en_US/en_UK installed, assert the all-caps
      "English" row is absent and both proper English rows are present.
- [ ] Arabic render (screenshot-based, given the known non-Latin OCR weakness): switch to
      Arabic, save `build/i18n_test/lang_ar.png`, assert glyphs are not all `?` (a
      distinct-ink-run proxy that the font loaded).
- [ ] RTL layout: on the Arabic screen, assert the selector cursor is on the right half
      and the scrollbar on the left edge (validates the mirror without needing Arabic
      OCR), and that the split panes are mirrored.
- [ ] Extend `verify_entry_translated_in_all_languages.py` default ids with the new ones
      (for example `STR_THEME_DEFAULT`, `STR_SELECT_FS`, the divider ids); add dedicated
      checks for the placeholder strings by driving into the screen that shows them.
- [ ] Run `theme_lang_test.py` and the language carousel as regressions; re-verify the
      Retro-Go return round trip (boot magic is untouched, but re-verify per the standing
      law).

## Documentation updates

Update in the same pass (single source of truth):

- [ ] [docs/i18n.md](i18n.md): the en_US/en_UK model, an RTL section replacing the current
      "out of scope: RTL/bidi/shaping" note, the offline-shaping pipeline, the Arabic font
      and its license, and the ABI bump.
- [ ] [DESIGN.md](../DESIGN.md): the i18n section / Goal 14.
- [ ] [README.md](../README.md): the "Multi-Language Menus" feature, in plain language.
- [ ] [ACTIVE_WORK.md](../ACTIVE_WORK.md): Goal 14 tasks.
- [ ] [CHANGELOG.md](../CHANGELOG.md): once landed and hardware-verified.

Use the write/replace tools for documentation, never shell redirects.

## Verification sequence (ordered)

1. Pre-flight: once the Unixel OTF/TTF is in hand, verify its cmap covers FE70 to FEFF
   (and ideally FB50 to FDFF) and check its pixel grid (fontTools/PIL). If presentation
   forms are absent, switch to the font-agnostic HarfBuzz-to-PUA fallback (2a) rather than
   reselecting the font. Confirm `arabic_reshaper` + `python-bidi` (or `uharfbuzz` for the
   fallback) are installable in the cook environment.
2. Task 1 (lowest risk, still ABI 3): create the en_US/en_UK JSON, the manifest endonyms,
   the cook_lang.py un-skip, and the sentinel rename + hide + default-to-en_US in
   lang_mgr.c. `make i18n`; host round-trip test; deploy; run the default-en_US,
   en_UK-persist, and sentinel-hidden device tests plus theme_lang_test as a regression.
3. Task 3 (ABI bump): append ids + English defaults, wire `tr()` at all sites, add the
   splicers, do the divider/notice/OFW/placeholder conversions, bump the ABI from 3 to 4.
   `make i18n` re-cooks all packs; `make test_host`; deploy all packs; run the extended
   verify-all-languages across every language.
4. Task 2 (most invasive; depends on the font + ABI 4): add the Arabic font + license, the
   cook_font.py source handling, the cook_lang.py reshape/bidi, the `ar` manifest entry,
   the RTL flag plumbing, `compose_value`, and the full UI mirror (lists, header, modals,
   both split-pane screens). `make i18n`; deploy; run the Arabic render and RTL-layout
   device tests; rerun the carousel + theme_lang_test as regressions.
5. Translations: author and draft all packs; re-cook; re-deploy; spot-check on device.
6. Final: full `make test_host`; full on-device i18n suite; boot with `/i18n/` empty to
   confirm the all-caps emergency English still renders and the menu is reachable; confirm
   the Retro-Go return round trip.

## Risks and blockers

1. Arabic font coverage (medium): the presentation-form path needs the font cmap to map
   U+FE70 to U+FEFF (Unixel, being Unifont-derived, very likely does; verify). If it does
   not, the font-agnostic HarfBuzz-to-PUA fallback (2a) removes the hard blocker at the
   cost of more cook code. Verify cmap and pixel grid before writing the pipeline.
2. Cook host deps (environmental): `arabic_reshaper` and `python-bidi` may not be present
   in the build environment; Task 2 cooking cannot run until they are installed where
   cooking happens.
3. RTL composed-string correctness (medium): offline bidi shapes standalone fragments
   only; runtime label + value + number order is handled by `compose_value` plus
   right-anchoring and must be validated on device.
4. Full split-pane mirror (medium): `partition_viewer.c` and `ui_file_browser.c` carry
   many absolute x-literals; route them all through `gui_mirror_x` and verify on the
   Arabic screen.
5. ABI-4 redeploy discipline (operational): every pack must be re-cooked and re-pushed or
   it shows English until refreshed; note this in release notes. The SD install auto
   updates by version.
6. Translation quality (medium): machine-drafted strings in many languages and Arabic MSA
   need native review before they can be called professional; ship with English fallback
   for anything unverified.

## Font reference

- Unixel: github.com/MDarvishi5124/Unixel, SIL OFL 1.1, English-Arabic/Persian pixel
  font inspired by Unifont. Primary Arabic candidate.
- Eternal Dream Arabization Arabic pixel font: OFL-1.1, bundled by hajimehoshi/bitmapfont
  at 12x13px. Fallback Arabic candidate.
- Existing scripts use Fusion Pixel 12px monospaced (OFL-1.1), per
  [docs/i18n.md](i18n.md).

## Critical files

- `src/drivers/language/lang_mgr.c` (sentinel rename + hide, default-to-en_US, RTL flag +
  `i18n_is_rtl`)
- `scripts/build/cook_lang.py` (un-skip en_US, RTL reshape/bidi, endonym shaping)
- `scripts/build/cook_font.py` (Arabic source filename handling, per-script ppem)
- `src/chainloader/ui/strings.h` + `strings.c` (append ids, ABI 3 to 4, English defaults)
- `src/chainloader/ui/i18n.c` + `i18n.h`, `src/chainloader/system/language.h`,
  `src/drivers/language/driver_entry.c` (RTL plumbing through the vtable)
- `src/chainloader/gui.c` + `gui.h` (`gui_rtl`, `gui_mirror_x`, right-anchor helper,
  selector glyph)
- `src/chainloader/ui/ui_list.c`, `ui/ui_manager.c`, `ui/partition_viewer.c`,
  `ui/ui_file_browser.c`, `menu.c` (the full RTL mirror + `compose_value` + `tr()` wiring
  + placeholder splicing)
- `src/chainloader/system/utils.c` + `utils.h` (`str_fmt1_int` / `str_fmt1_str`)
- `i18n/lang/langs.json`, `i18n/lang/en_US/strings.json`, `i18n/lang/en_UK/strings.json`,
  `i18n/lang/ar/strings.json`, the new ids added to all existing
  `i18n/lang/<code>/strings.json`
- `i18n/fonts/` (Unixel Arabic OTF/TTF + OFL license; Eternal Dream as fallback)
- `scripts/common/i18n_strings.py`, `scripts/tests/*` (detector de-dup + endonym
  disambiguation; new tests)

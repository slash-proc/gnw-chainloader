# QA Test Coverage Plan

Status: infrastructure built and validated on hardware; test breadth in progress
(branch `qa-test-suite`). This is the granular QA scope and checklist; it
complements the overarching goals in [ACTIVE_WORK.md](../ACTIVE_WORK.md) and defers
subsystem mechanism detail to [DESIGN.md](../DESIGN.md) and the per-feature docs
rather than restating it.

## Context

The project has a capable harness (`scripts/common/`: `remote_input`, `ocrnav`,
`ocr`, `harness`, `i18n_strings`, `devstate`) plus the original ~15 on-device
tests and 2 host unit tests, but coverage was uneven: it clustered on i18n, the
boot-magic Retro-Go round trip, ABI gating, and remote-input timing, while whole
subsystems had no assertions, and the suite assumed one pre-provisioned device
state. This plan defines the QA scope to test nearly every feature across nearly
every device setup, holding the two fixed points that always hold: the chainloader
is always at `0x08000000` and any OFW is always at `0x08100000`. Everything else
(Bank 2 contents, external OSPI flash, SD card, LittleFS modules and packs, backup
registers, boot magic) is a variable the suite must provision and sweep. The
outcome is a parametrized, environment-aware suite: point it at a device in any
state and it detects what is installed, runs every test valid for that state,
proves the stability invariant, and reports feature coverage.

## Guiding principle

Stability is law: the launcher menu must be reachable in every device state. The
master axis is the **environment matrix** (what is on the device) crossed with the
**feature set**. The spine of every on-device run is: provision (or detect) a known
environment, prove the invariant (menu reachable, navigable, and the Retro-Go
return round trip if Retro-Go is present), then run the feature assertions valid in
that environment.

**Assert over SWD first, OCR is layered and optional.** Device state is read
through named SWD observables (`scripts/common/observe.py`) — theme/feature counts,
SD detect, RW drivers, the settings word, boot-magic cells — not by reading the
screen. OCR (`ocr.py`) is an *additional* check behind `if ocr.ensure_fonts():`,
never a hard dependency. This was validated: a live `envprobe` reads 14 device
fields over pure SWD with zero OCR.

## What changed since the first draft (reconciliation)

The first draft drifted from the codebase. Corrections now folded in:

- **19 languages, not 16.** Counts are derived from `i18n/lang/<code>/` and
  `langs.json`, never a literal.
- **Theme, language, and installer are PIE modules** (`/modules/*.bin`), not
  in-core. `g_current`/`g_langs` left `app.elf`, so language detection is OCR/
  `(code)`-suffix only. The persisted **language lives in `/i18n/.active`** on
  LittleFS (the authoritative store), with a *copy* in the BKP3R settings word
  (`SETTINGS_LANG`). Persistence tests must use `/i18n/.active`, not BKP3R.
- **`EXTFLASH_SIZE` and `RG_SD_CARD` are not build flags** (external size is
  runtime-detected via `total_ext_flash_size`; the FrogFS/LittleFS split is a
  patcher-output layout). The real build-variant matrix is `REMOTE_INPUT`,
  `ABI_SELFTEST`, `BOOT_BENCH`, `CRASH_TEST`, `DUMMY_ABI={0,1,2}`, `RTL_TEST`,
  `FONT_STYLE`. `ENV-SIZES` is therefore an extflash-*content* fixture.
- **No `flash_all`/`flash_chainloader` Make targets.** Flashing is
  `gnwmanager flash bank1 build/gnw_chainloader.bin` + `push_batched.py` for
  LittleFS.
- **New since the draft:** the feature-module framework, the MP3 player feature
  module, the transient installer handling both packs and modules, module
  versioning, and FAT/exFAT + lfs_rw RW driver modules. **FrogFS is scan-only**
  (recognized + sized, not browsable).
- **A device left on a sub-page leaves `g_list_main` dormant.** Device tests must
  back out to the main menu first (`harness.go_home`, which presses B until
  `g_stack_ptr == 0` — SWD-driven, no OCR). This had been mis-read as broken input.

## Test taxonomy

| Level | What | Hardware | CI |
| :--- | :--- | :--- | :--- |
| L0 Host unit | Pure logic: settings-word, boot-magic table, `.lang`/`.fnt` parse, OCR offline oracle | none | yes |
| L1 Build / static | Build-variant matrix compiles; 40 KiB ceiling; clean-build determinism | none | yes |
| L2 Component | Single feature, probe-driven on device (nav, file op, theme, language, SD detect, modules) | yes | batched |
| L3 Scenario | Multi-step flows (OFW switch + escape, Retro-Go round trip, SD install, fast-boot) | yes | batched |
| L4 Environment sweep | The invariant plus applicable L2/L3 across each named environment | yes | batched |
| Manual / semi-auto | Cold-boot god-mode, SD insert/remove, hardware-mod variant, post-bank-swap power-on, fast-boot restore | yes | manual |

L0/L1 gate every change with no device. L2-L4 run in batched hardware sessions
because probe access must be serialized and bank swaps need a manual power-on.

## The environment model

### Dimensions (the "setup" space)

1. **Chainloader build (always at `0x08000000`).** `REMOTE_INPUT` 1 (test) vs 0
   (ship); `ABI_SELFTEST`; `BOOT_BENCH`; `CRASH_TEST`; `DUMMY_ABI` 0/1/2;
   `RTL_TEST`; `FONT_STYLE`. (Not `EXTFLASH_SIZE`/`RG_SD_CARD` — see above.)
2. **Bank 2 (OFW slot, `0x08100000`).** Empty/erased, Mario, Zelda, garbage.
3. **Retro-Go (`0x0800A000`).** Present (valid reset-vector pair, validated by a
   `board_is_valid_app` proxy) vs absent (8-byte vector cleared — cheap toggle).
4. **External OSPI flash (`0x90000000`).** Unprogrammed (`0xFF`); OFW asset blocks;
   OFW internal backups; Retro-Go FrogFS/LittleFS; content layouts.
5. **LittleFS content.** Bare; modules (`theme/language/installer/fatfs/lfs_rw/
   fileops/features/mp3`); i18n packs (subset/all 19) + fonts; stale-ABI artifacts;
   multi-version modules.
6. **SD card.** Absent; FAT32; exFAT; content (LFN, UTF-8, nesting); `/i18n` folder;
   hardware mod SPI1 ("Tim", this unit) vs SoftSPI ("Yota9").
7. **Persistent registers + magic cells.** Clean boot; fast-boot (`BKP3R` bit);
   injected magic (`BOOT`+target, `CORE`, `RESET`, `STANDBY`); theme slots (`BKP3R`);
   language (`/i18n/.active`).

### Named environment fixtures (IMPLEMENTED — `scripts/tests/env/`)

Each is a declarative `Recipe` with an `expect()` closure verifying the provisioned
state. `provision.apply()` provisions in dependency order, prompts manual steps,
then `envprobe.probe()` + `expect()` confirm the device reached the intended state.

| Name | Description |
| :--- | :--- |
| `ENV-BARE` | Chainloader only: no Retro-Go, Bank 2 erased, extflash head erased, no SD, bare LittleFS. The "sole thing on the device" case. |
| `ENV-RG` | Chainloader + Retro-Go + minimal extflash. No OFW, no SD, no modules/packs. |
| `ENV-DOCS` | Full golden setup: reflashed chainloader + Retro-Go + all modules + all 19 packs + SD with content. The only full-reflash recipe. |
| `ENV-OFW-RESIDENT` | Mario (or Zelda) resident in Bank 2 (flashed via the LAUNCH menu). |
| `ENV-NO-EXTFLASH` | Extflash head erased to `0xFF`; OSPI probe must degrade gracefully. |
| `ENV-STALE-ABI` | Baseline for the ABI test, which builds the mismatched artifacts itself. |
| `ENV-CORRUPT` | Invalid Retro-Go vector + erased Bank 2; the invariant must still hold. |

`ENV-SIZES` (extflash content variants) and `ENV-SD-*` (SD insert variants, manual)
are follow-ons.

## Infrastructure (BUILT)

| Piece | File | Status |
| :--- | :--- | :--- |
| Named SWD observables | `scripts/common/observe.py` | done, hardware-validated |
| Environment probe | `scripts/common/envprobe.py` | done, hardware-validated |
| Provisioning library | `scripts/common/provision.py` | cheap toggles + clean_reboot validated; heavy flashes authored |
| Named env fixtures | `scripts/tests/env/` | done (7 recipes + registry) |
| Suite runner + report | `scripts/tests/run_suite.py` | done (adaptive/matrix/tier modes) |
| OCR un-break + oracle | `scripts/common/ocr.py`, `scripts/tests/ocr/` | done (offline oracle green) |
| SWD `go_home` helper | `scripts/common/harness.py` | done, hardware-validated |

`provision.py` reuses the existing tools rather than re-typing their solved
sequences: it imports `bank.py` register constants for the swap-bit read, shells
`push_batched.py` for LittleFS (batched, deadlock-safe), and `gnwmanager flash` for
internal/external flash. Cheap toggles (magic cells, settings word, Retro-Go vector)
are direct register/RAM writes over a short-lived backend; `dry=True` makes the
whole orchestration verifiable off-hardware.

## SWD observable inventory

These cover almost every "no observable" gap the first draft worried about, so the
gated-firmware-hook proposal is mostly unnecessary (only `ABI_SELFTEST`,
`BOOT_BENCH`, `CRASH_TEST` remain as gated observables, and they already exist):

`uwTick` (liveness), `g_list_*` (`selected`@8, `num_items`@4), `g_stack_ptr` (modal
depth — the `go_home` signal), `ui_theme_slot`, `g_theme_module_count`,
`g_feat_count`/`g_feat_ran`, `g_modules_ready`, `total_ext_flash_size`, `g_hw`/
`g_card_type` (SD), `g_lfs_rw_loaded`/`g_fat_rw_loaded`/`g_driver_count`,
`g_pool_next`/`g_mod_load_err` (pool + leak check), `g_last_activity_tick` (idle),
`g_boot_target`, `g_op_cancelled`/`g_active_tab` (browser), the boot-magic cells
(`SRAM_MAGIC`/`TARGET`/`RG_MAGIC`), and the BKP3R settings word
(`TAMP_BKP3R = 0x5800450C`: fast-boot, theme slots, lang copy).

## OCR strategy (findings)

The offline oracle (`scripts/tests/ocr/ocr_offline_test.py`, L0, no device) renders
the device's own glyphs (synthetic) and also checks a captured device-frame corpus.
Findings:

- **Synthetic `locate()` scores 1.00 for every script** — but that HID a real bug.
- **On dense REAL frames, rigid template `locate()`/`has()` is not
  discriminative.** Common letters find lucky partial hits, so a not-on-screen word
  ("Nintendo") scores ~0.74, higher than the correct words. This made
  `i18n.detect_language` mislabel a German screen as `it_IT` (its fallback ranked
  candidates by rigid-locate score). The corpus capture exposed it.
- **The robust device-frame matcher is `Screen.contains(needle)`** = read the
  actual rows (`read_rows`, following the device's own glyph layout) and
  substring-check. Strongly discriminative; also separates the German/Dutch title
  near-collision. `read_rows` is minutes-slow over the full 31k-glyph CJK set, so
  build `ocr.Screen(frame, max_cp=0x600)` to drop the CJK/Hangul bulk for a
  Latin-family frame (~2 s). `detect_language` now ranks candidates by read-row
  substring matches. `has()` stays rigid (ocrnav consumers depend on it; adopt
  `contains()` for nav as a verified follow-on).
- **Language detection per script (validated on hardware):**
  - *Latin/Greek/Cyrillic* — read-substring with `max_cp=0x600`; detect fixed.
  - *CJK (ja/zh/ko)* — WORKS. CJK is LTR and its characters are discrete blocks,
    so the screen reads. `detect_language` restricts the glyph set to the candidate
    label codepoints (a few dozen, fast despite the 31k-glyph font) and ranks by
    substring matches. Forced ja_JP/zh_CN/ko_KR each detect EXACTLY (`cjk_detect_test`).
  - *Arabic/Farsi (RTL)* — recovery-only. The layout is mirrored AND the script is
    cursive (connected), so `read_rows` can't segment it and the main menu doesn't
    show the endonym; an Arabic screen is not reliably pinned by OCR. The robust
    handling is **deterministic recovery**: `provision.set_active_language()`
    rewrites `/i18n/.active` and reboots (a file write, no OCR/nav), which escapes
    RTL every time (`rtl_recover_test`). The `(code)` ASCII suffix on the selector
    row is the reliable in-place signal.
- **Clean-build fragility fixed.** `ocr.ensure_fonts()` regenerates
  `build/i18n/fonts` (wiped by `make clean`) and `Font.load()` warns loudly when
  zero `.fnt` load. Proven end-to-end (delete the fonts, a test self-heals).
- `capture_corpus.py` (SWD-navigated, so it doesn't depend on the OCR it captures)
  records device-realistic reference frames per language × screen; the offline
  oracle replays them (green incl. real German-frame checks). `scripts/debug/
  ocr_corpus_diag.py` scores a frame under different fg/tolerance/matchers for tuning.

## Exhaustive test list (by tier × subsystem)

Status: **[done]** validated on hardware / CI · **[auth]** authored, not yet run on
hardware · **[plan]** specified, not yet written.

### L0 host (no device, CI)
- [done] `host/test_settings_word` — BKP3R make/decode mirror, field isolation, invalid-word defaults.
- [done] `host/test_boot_magic_table` — locks the 6 magic constants + decision priority (CORE/RESET outrank a staged BOOT).
- [done] `host/test_lang_fnt_parse` — all 19 `.lang` (LNG2) + `.fnt` (FNT1) headers, uniform ABI/count, endonym/script vs langs.json.
- [done] `ocr/ocr_offline_test` — per-script `locate()` oracle (+ `--roundtrip` diagnostic).

### L1 build (no device, CI)
- [done] `build/size_ceiling_test` — 40 KiB cap + free-headroom budget.
- [done] `build/determinism_test` — `assets_gen.{c,h}` regenerate byte-identical.
- [auth] `build/build_matrix_test` — every build variant compiles (heavy; subset default, `--full` all).

### Stability invariant (L4, every env)
- [done] `boot/invariant_menu_reachable_test` — chainloader live, full 4-item menu, cursor moves, TOOLS/SETTINGS/POWER/LAUNCH reachable (pure SWD). 9/0 on hardware.

### Boot / magic / fast-boot
- [done] `retrogo_return_test` (existing) — CORE/RESET re-launch Retro-Go (the law).
- [auth] `boot/fast_boot_test` — fast-boot boots Retro-Go; PAUSE/START forces menu (operator-present; restore can need a physical button).
- [done] L0 boot-magic decision table (above).
- [plan] `boot/boot_magic_test` — inject each magic + reset on device (overlaps retrogo_return; lower priority).
- [manual] cold-boot physical god-mode (LEFT/RIGHT/B/START/PAUSE at power-on).

### OFW lifecycle (L3)
- [done] `boot_selector_test` (existing) — boot each target, escape via LEFT+GAME.
- [plan] `ofw/ofw_flash_from_backup_test` — switch to an OFW only on extflash; assert it flashes into Bank 2 then boots.
- [plan] `ofw/ofw_warmswitch_test` — framebuffer non-black after the warm switch.

### Themes
- [done] `theme/theme_persist_test` — per-OFW theme slots round-trip through reboot (BKP3R, pure SWD). 5/0 on hardware.
- [done] `theme_lang_test` (existing) — DEFAULT/FALLBACK theme via `ui_theme_slot`.
- [plan] `theme/theme_module_test` / `theme_fallback_test` — sprite-module load + cursor; color-only fallback when absent.

### i18n (19 langs)
- [done] `module_presence_test` covers the language module loaded.
- [plan] `i18n/lang_persist_test` — change language via UI, reboot, verify via `/i18n/.active` + OCR `(code)` suffix (OCR-layered).
- [plan] `i18n/lang_fallback_test` — `/i18n` empty -> English ASCII.
- [done] `installer_test` (existing) — SD->LittleFS install (both classes).
- [done] `test_abi_reject` (existing, refactored to shared `clean_reboot`) — module + pack ABI rejection.
- [plan] `i18n/module_version_test`, `i18n/rtl_test` (RTL_TEST build).
- [done] `verify_entry_translated_in_all_languages`, `i18n_switch` (existing, OCR carousel).

### Filesystems / browser
- [done] `module_presence_test` — VFS drivers registered (lfs_rw/fat_rw, count).
- [done] `fileops_test`, `sd_fonttest` (existing).
- [plan] `fs/lfs_rw_test`, `fs/fat_browse_test`, `fs/recursive_copy_test`, `fs/space_cancel_test` (`g_op_cancelled`), `fs/frogfs_scanonly_test`, `fs/rtc_timestamp_test` — with host filesystem read-back.

### Partition / file browser / destructive
- [done] `partition/partition_viewer_test` — Tools->Partition Viewer opens (g_stack_ptr deepens), extflash detected, scan completed, clean back-out (pure SWD). 9/0 on hardware.
- [done] `fs/file_browser_open_test` — Tools->File Browser opens, a filesystem tab is active, a RW driver loads, no crash, clean exit (pure SWD). 8/0 on hardware.
- [done] `fs/lfs_wipe_restore_test` — the LIGHT destructive case: wipe all of /modules + /i18n (discover+remove), prove the stability invariant holds with LittleFS empty, restore from pre-built artifacts. Refuses to wipe unless it can restore. Invariant verified on hardware.
- [deferred] `partition/secure_delete_test`, `diag/diagnostics_test` — OCR-layered; pending (see Deferred below).
- [deferred] `partition/full_erase_destructive_test` — the HEAVY destructive case (see Deferred).

### Power / SD / modules
- [done] `sd/sd_detect_test` — `g_hw`/`g_card_type` (pure SWD). 3/0 on hardware.
- [done] `power/idle_hide_test` — idle timer (`uwTick - g_last_activity_tick`) advances + resets on input (pure SWD). 6/0 on hardware.
- [done] `modules/module_presence_test` — scan done, drivers, theme/feature counts, pool sane, no load error (pure SWD). 6/0 on hardware.
- [done] `test_remote_input` (existing, fixed to `go_home` first). 6/0 on hardware.
- [done] `feature_menu_test`, `ocr_nav_test` (existing) — feature discovery / OCR nav (validated with the hardened ocrnav).
- [deferred] `modules/pool_overflow_test`. Module/MP3 feature tests deprioritized per direction.

## Running the suite (Make stages)

Each tier has a Make stage that wraps `scripts/tests/run_suite.py`. Every stage
self-heals missing OCR fonts (regenerates `build/i18n` via `make i18n` when absent)
and re-renders the single `qa-report.html` after the run, so a stage never fails on
a clean tree and always leaves an up-to-date report:

| Stage | Runs | Device? |
| --- | --- | --- |
| `make qa` | self-detects (`QA_SCOPE=auto`): full bench run if the programmer is free + a device is connected, else host tiers | auto |
| `make qa QA_SCOPE=full` | force the full bench run | yes (SWD) |
| `make qa QA_SCOPE=host-only` | force host tiers L0+L1 | no |
| `make qa-auto` | same as `make qa` (the gate) | auto |
| `make qa-full` | force the full bench run (every applicable device test) | yes (SWD) |
| `make qa-host-only` | host tiers L0+L1 only | no |
| `make qa-l0` | L0 host unit (settings-word, boot-magic, parse, offline OCR) | no |
| `make qa-l1` | L1 build gates (size ceiling, determinism, build matrix) | no |
| `make qa-l2` | L2 component device tests | yes (SWD) |
| `make qa-l3` | L3 scenario device tests | yes (SWD) |
| `make qa-l4` | L4 environment matrix sweep | yes (SWD) |
| `make qa-report` | re-render `qa-report.html` only (runs nothing) | no |

`make qa` **uses the device by default** and is best-effort: the `--auto` gate
`pgrep`s for an openocd/gnwmanager already holding the ST-Link (so it never
collides with another session) and, if the programmer is free, runs the full bench
suite whenever a device answers the probe — it does *not* require the chainloader
to be confirmed alive, since the device tests report their own state. It falls back
to host tiers L0+L1 only when the programmer is busy or no device is connected, so
it works with or without hardware. For deterministic behavior, override with
`QA_SCOPE=full` (force the full run) or `QA_SCOPE=host-only` (force host tiers). **Every test-running stage** re-renders
the report; only `qa-report` skips running tests. The HTML accumulates across
stages (latest result per test in `build/qa_results.json`), so running several
builds one combined report. `run_suite.py` also takes `--ids` and `--matrix ENV-…`
directly for finer control.

## Deferred tests (heavy / special-build / device-contention)

Some specified tests need either a long UNCONTENDED device session or a firmware
REBUILD — both of which conflict with an active parallel source-editing session
sharing the one ST-Link, and several wear flash or risk stranding the shared
device. They are tracked, with run conditions and acceptance criteria, in their own
backlog: **[qa-deferred-tests.md](qa-deferred-tests.md)**. In brief: full
external-flash erase/restore, the `CRASH_TEST`/`BOOT_BENCH` hook tests, fast-boot
RUN, the full build-variant compile sweep, the L4 environment matrix, and the
OCR-nav file-operation/diagnostics tests (now technically unblocked by the hardened
`ocrnav`, pending a dedicated device session).

## The report phase

`run_suite.py` ends with a tactful, informative report: per-test status grouped,
goal coverage traced to the DESIGN goals, skips grouped by actionable reason
("needs the ABI_SELFTEST build flashed", "OCR fonts absent: run make i18n", "manual:
insert SD"), failures with their output tail and observable, and an honest,
non-alarmist bottom line. A skip reads as "not applicable in this run", never a
failure. Device tests skip (not fail) when their required build flag isn't flashed,
gated by sentinel symbols (`g_abi_selftest_mod`, `g_boot_bench`, `crash_test_check`)
in `app.elf`.

## Execution strategy

- Serialize probe access: one openocd/gnwmanager at a time; tests run as isolated
  subprocesses so each owns its single session; batch LittleFS pushes in groups of
  3-4.
- Provision once per environment; group tests; minimize bank swaps (each needs a
  manual power-on).
- Bound every probe phase with `time_budget`; `recover_probe()` on stall; assert
  `chainloader_running()` before trusting any read.
- CI runs L0 + L1 on every change. L2-L4 run in scheduled hardware sessions —
  adaptive mode for a quick "whatever is on the bench" pass, matrix mode for release
  validation.

## Risks and known limitations

1. Cold-boot physical god-mode is not injectable (`app_early_logic` reads GPIO, not
   the shadow cell), so it stays a documented manual procedure.
2. Some setups need physical intervention: SD insert/remove, the SoftSPI unit, the
   post-bank-swap power-on, and the **fast-boot restore** (the clean escape is the
   physical PAUSE override). These are semi-automated (prompted) or manual.
3. Arabic OCR uses the distinct-ink-run proxy (the font holds shaped presentation
   forms, not base codepoints).
4. Large-image provisioning is slow and wears flash; prefer cheap toggles (vector
   pair, registers, targeted writes) and reserve full flashes for `ENV-DOCS`.
5. Bank swaps tear down the SWD session and need a manual power-on; group
   swap-dependent tests.
6. Destructive tests must be `--destructive`-gated, env-restricted, and always
   restore + verify; never run them in adaptive mode on an arbitrary bench device.

## Definition of done

- Every goal traces to at least one automated test or a documented manual procedure.
- `ENV-BARE` and `ENV-DOCS` invariant sweeps pass; the menu is reachable in every
  named environment.
- The Retro-Go return round trip passes after any boot-path change (the law).
- L0 + L1 gate every change in CI.
- An adaptive-mode run on an arbitrary bench device reports coverage and passes the
  invariant.
- Manual-only items are documented with explicit step-by-step procedures.

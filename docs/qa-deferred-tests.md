# QA Deferred Tests — Backlog & Run Conditions

This is the authoritative, actionable backlog for QA tests that are **specified
and (where noted) authored**, but **not yet run/validated on hardware**. Each is
deferred for a concrete reason: it needs either a **dedicated/uncontended device**
(long device sessions, flash wear, or it can strand a shared unit) or a **firmware
rebuild** (a `src/` recompile, which conflicts with a parallel source-editing
session sharing the one ST-Link). The live test plan in
[qa-test-coverage-plan.md](qa-test-coverage-plan.md) links here rather than
restating this.

For each item: **what it validates · blocker · prerequisites · how to run +
acceptance · status.** Check the box only after hardware validation (the project's
standing rule — a green build is never sufficient).

---

## 1. Full external-flash erase + restore-from-source (destructive)

- [ ] `partition/full_erase_destructive_test`
- **Validates:** the stability invariant survives a *whole external flash* erase —
  the menu stays reachable with `0x90000000` blank — and that the device can be
  rebuilt to a working golden state entirely from source.
- **Blocker:** heavy (tens of minutes: a Retro-Go build + ~60 MB reflashed over
  SWD), wears flash, and the restore runs `make` paths that collide with a
  concurrent `make clean` from the parallel session.
- **Prerequisites:** dedicated device; intact source tree; `build/` populated (or
  willing to rebuild). The light `fs/lfs_wipe_restore_test` already proves the
  invariant for a LittleFS-only wipe; this is the full-chip variant.
- **Run + acceptance:** `run_suite.py --ids full_erase_destructive_test
  --destructive`; provisions, erases extflash, restores via
  `provision.full_provision_docs()` (`make flash_all` + `flash_rg` + `push_modules`
  + `push_i18n`), then `envprobe` must show `chainloader alive`, modules scanned,
  19 i18n packs, and the invariant test must pass. Must NEVER run in `--adaptive`
  on an arbitrary bench device.

## 2. Crash-log hook

- [ ] `hooks/crash_log_test`
- **Validates:** a deliberate fault is captured in the D3-SRAM crash log and
  decodes correctly (`scripts/debug/crash_log.py`).
- **Blocker:** needs the `CRASH_TEST` firmware build (recompiles `src/`).
- **Prerequisites:** `make clean && make CRASH_TEST=1 -jN` flashed; sentinel symbol
  `crash_test_check` present (run_suite's BUILD_SENTINEL gating already keys on it).
- **Run + acceptance:** flash the CRASH_TEST build; `run_suite.py --ids
  crash_log_test`; the injected fault must appear in the decoded log with the
  expected fault type/PC, and the device must still reach the menu afterward.

## 3. Boot-bench hook

- [ ] `hooks/boot_bench_test`
- **Validates:** boot-to-menu timing stays within budget (no boot-path regression).
- **Blocker:** needs the `BOOT_BENCH` firmware build (recompiles `src/`).
- **Prerequisites:** `make BOOT_BENCH=1` flashed; `g_boot_bench` symbol present.
- **Run + acceptance:** `run_suite.py --ids boot_bench_test`; the measured
  boot-to-menu must be ≤ the recorded budget (`scripts/debug/boot_bench.py`); flag a
  regression if it grows.

## 4. Fast-boot RUN (not just the SWD state check)

- [ ] `boot/fast_boot_test` (the live boot path; the SWD settings-word check is done)
- **Validates:** with fast-boot enabled, a reset boots straight into Retro-Go, and
  the **physical START/PAUSE hold** still forces the chainloader menu (the only
  escape).
- **Blocker:** authored but not auto-run — enabling fast-boot makes *every* reset
  boot Retro-Go, so a failure with no physical button press can strand a shared
  device.
- **Prerequisites:** dedicated device; ability to assert/inspect across the warm
  reset (`wait_u32(reconnect=True)`).
- **Run + acceptance:** set the fast-boot bit; reset; confirm Retro-Go launched
  (reset-vector / `g_boot_target`); then assert the START-hold override returns to
  the menu; finally clear the bit and confirm normal boot. Must leave fast-boot
  OFF.

## 5. Build matrix — full compile sweep

- [ ] `build/build_matrix_test --full`
- **Validates:** every build variant still compiles (`REMOTE_INPUT`, `ABI_SELFTEST`,
  `BOOT_BENCH`, `CRASH_TEST`, `DUMMY_ABI={0,1,2}`, `RTL_TEST`, `FONT_STYLE`).
- **Blocker:** recompiles `src/` repeatedly — collides with a concurrent source edit
  (and is slow).
- **Prerequisites:** exclusive use of the build tree (no parallel `make`).
- **Run + acceptance:** `run_suite.py --ids build_matrix_test --full`; every variant
  links and the default variant stays ≤ the 40 KiB ceiling. (The default-variant
  subset already runs in L1.)

## 6. L4 environment matrix sweep

- [ ] `run_suite.py --matrix ENV-DOCS,ENV-RG,ENV-BARE,ENV-OFW-RESIDENT,ENV-NO-EXTFLASH,ENV-STALE-ABI,ENV-CORRUPT`
- **Validates:** the suite provisions each named environment from
  `scripts/tests/env/` and sweeps the applicable tests, proving the invariant in
  every environment.
- **Blocker:** provisioning the full environments uses the heavy rebuild path (item
  1) and needs an uncontended device; several envs tear down SWD (bank swap) and
  need `wait_u32(reconnect=True)`.
- **Prerequisites:** dedicated device; the destructive/restore path validated (item
  1) first.
- **Run + acceptance:** each env provisions, `envprobe` confirms the expected state,
  the per-env invariant passes, and the device is restored between envs.

## 7. OCR-nav file-operation & diagnostics tests

- [ ] `partition/secure_delete_test`, `diag/diagnostics_test`,
  `fs/lfs_rw_test`, `fs/fat_browse_test`, `fs/recursive_copy_test`,
  `fs/space_cancel_test`, SD-content browse
- **Validates:** the File Browser / Tools file operations end-to-end (copy, delete,
  secure-delete, cancel via `g_op_cancelled`, RW-driver round-trips with host
  read-back), and the diagnostics screen.
- **Blocker:** OCR-nav + file-op heavy. **Now unblocked technically** by the
  hardened `ocrnav` (read-row matching + `^X$` anchors) — these were failing on the
  rigid matcher — but they still need a dedicated device session to author the
  step-by-step nav and tune timing, and several need specific SD/FS contents staged
  beforehand (directories can't be created on the SD card mid-test).
- **Prerequisites:** dedicated device; `build/i18n` cooked (`make i18n`) for OCR
  fonts; SD card with the required directories pre-created.
- **Run + acceptance:** each operation drives the browser via `ocrnav`, performs the
  action, and verifies the result by SWD (`g_op_cancelled`, driver-loaded flags,
  `pool_used` leak check) and/or host filesystem read-back — SWD-observable-first,
  OCR only for the navigation.

---

## Done (moved out of deferred)

- **`make qa-report`** — implemented; re-renders `qa-report.html` from
  `build/qa_results.json` on demand (host-only).
- **OCR-nav hardening** — `ocrnav`/`detect_language` read-row matching + `^X$`
  anchors; `ocr_nav_test` and `theme_lang_test` green on hardware.
- **Bank-swap / OFW round-trip** — `boot_selector`, `retrogo_return` now run
  headless (the OFW power-off-on-switch fix landed in gnwmanager).

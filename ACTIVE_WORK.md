# ACTIVE_WORK.md

This file tracks the active working session: the remaining checklist of **Overarching Goals** and granular **Tasks** grouped under them, plus the active **Debugging** log and process notes. Completed fixes and tasks are logged in [CHANGELOG.md](CHANGELOG.md), not here.

---

## Overarching Goals

- [ ] **Goal 17: Clean file-operation merges**
  *   **Status:** Genuinely incomplete.
  *   **Requirements:**
      *   *Conflict Resolution UI:* Prompt the user (Replace, Skip, Apply-to-All) in `ui_file_browser.c` when a destination filename already exists during a paste operation, instead of blindly overwriting or failing.
      *   *Recursive Directory Merging:* Update the folder copy-paste logic in `fileops.c` to traverse and merge files folder-by-folder instead of failing directory creation if a directory already exists at the destination.
- [x] **Goal 18: Introduce CI Pipeline**
  *   **Status:** Completed and host-verified.
  *   **Requirements:**
      *   *CI Pipeline:* GitHub Actions workflow to build/push Docker build image and compile/test firmware on push, PR, and tags.

---

## Tasks

### Goal 18 - Introduce CI Pipeline
- [x] **CI Configuration:** Create `.github/workflows/ci.yml` and verify the pipeline compiles and passes `make qa-host-only` on the host.

### Goal 17 - Clean file-operation merges
- [ ] **Conflict-resolution UI during copy-paste:** Implement overwrite/skip prompts and directory merging logic in the file browser when file name collisions occur during paste.

### Goal 14 - Internationalized UI (Polish & Verification)
- [ ] **Translation-quality + untranslated-leak verification:** Verify German/non-English UI refinements and translation leaks (untranslated Partition Viewer labels/dividers) on physical hardware.

### QA Test Suite (branch `qa-test-suite` - remaining coverage)
- [ ] **Harness language checks update:** Update tests that read BKP3R settings registers to respect the new language persistence `/i18n/.active` on LittleFS.
- [ ] **SWD / HIL test suite expansion:** Implement automated tests for Partition Viewer details, secure-erase, and fast-boot RUN behavior.

---

## Debugging Log

- **FatFs file browser: stale "Empty folder" when stepping back out of a subdirectory (cosmetic, not a data issue; deferred).** Entering a folder (e.g. a `testö` of UTF-8-named files on the SD card) then pressing back to the parent renders "Empty folder"; backing out to the FS list and re-opening the FatFS volume restores it. Pre-existing and SD-specific. Deferred as out of scope.
- **i18n OCR non-Latin matching (#15) is not a font-metric problem (on hold).** Cyrillic/Greek/CJK template matching fails in templates; metric difference is handled consistently on both sides and is not the cause. Diagnosis needs a device screenshot experiment to inspect the match score.
- **Follow-ups (not done):**
  - The OCR tests that read `g_current`/`g_langs` over SWD (`i18n_strings.py active_code`, used by `ocr_nav_test`/`theme_lang_test`/`i18n_switch`) will fail because those symbols moved into the language module; repoint tests to OCR-only text verification.
  - Finish remaining `DESIGN.md` FrogFS-as-browsable mentions (§7 feature list, §11 backend table).
  - Update `README.md` mentions that imply in-core i18n.
- **Probe contention from concurrent gnwmanager commands (process note).** Issuing a second probe command while a `gnwmanager push` is still running wedges OpenOCD. Recovery requires `pkill -f gnwmanager; pkill -f openocd`, then `trace.py reset-halt`. Always serialize probe access.

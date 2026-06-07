# Startup Module Probe & Theme-Load Speedup

Granular technical checklist + benchmark log for making module/theme recognition at
boot effectively instantaneous. Overarching view lives in [ACTIVE_WORK.md](../ACTIVE_WORK.md);
this doc owns the detail. Branch: `fast-module-probe`.

---

## Problem (root cause, confirmed)

The bundled theme module is loaded only once the **entire** partition scan reaches
`PARTITION_SCAN_COMPLETE`:

- `menu.c:84` — `if (!themes_loaded && partition_scan_get_state() == PARTITION_SCAN_COMPLETE) { theme_modules_init(); }`
- `partition.c:311` (`partition_scan_update`) walks **INT1 → INT2 → SD → full external-flash sweep**
  before reaching `STATE_COMPLETE`. The external sweep covers `0x90000000` across the
  whole part (16–64 MB) at strides 1 MiB → 64 KiB.

The cost is **not** raw OSPI read time — the scan is deliberately *frame-paced*: 32 probes
per `menu_main_update()`, one call per rendered frame, each frame also doing a full
`gui_refresh()`. So time-to-theme ≈ `(effective checkpoints / 32)` frames. That is the
observed **500–1000 ms** (worse on a near-empty chip, which the `covered_end()` jump can't
skip past).

**Symptom:** the menu list paints immediately, but the module theme's
background/selector/footer sprites snap in ~1 s later — a jarring late "pop".

## Key realization

Loading the theme needs exactly **two** sources, not the whole partition map:
1. the SD card's module dir (if a card is present), and
2. the one LittleFS partition on external flash.

It does **not** need FrogFS, the OFW backups, the asset blocks, firmware footprint sizing,
or free-space gaps — those exist only for the Partition Viewer / File Browser, which the
user opens manually long after boot. So: **decouple "find the module sources" (fast, O(1)
reads at known offsets) from "enumerate every partition" (slow, exhaustive sweep).**

The version-hierarchy selection is **already** implemented in `vfs_read_module()` (vfs.c:186):
two-pass, SD-first, highest-version wins across whatever partitions are registered. We don't
add new selection logic — we just register the two module-source partitions *fast* and fire
the theme load at that earlier checkpoint.

## Goals / non-goals

- **Goal:** theme (colors + sprites) present within ~1–2 frames of the menu loop starting.
- **Non-goal / hard rule — no color flashing.** Do NOT apply a placeholder/console-default
  color theme before the real theme is ready. Screen stays mostly black; the correct theme
  arrives in one clean transition. (User aesthetic requirement.)
- **Off the boot-critical path.** This is cosmetic theming with an existing graceful
  fallback; the menu is already structurally reachable by this point. A slow/failed probe
  can only delay the first *render*, never block reaching the menu. Stability invariant intact.

## Design

1. **Decouple theme load from the full scan.** Reorder the scanner so module sources are
   found first, and add an earlier "modules ready" checkpoint that the menu triggers
   `theme_modules_init()` on (instead of `PARTITION_SCAN_COMPLETE`):
   - `STATE_PROBE_SD` — `sdcard_detect()` + `sd_probe()` (what `STATE_SCAN_SD` already does).
   - `STATE_PROBE_LFS` — validate the `littlefs` superblock magic at a **known base offset**
     (one ~32-byte read); `add_partition()` it.
   - Mark modules-ready here. The exhaustive `STATE_SCAN_INT1 … STATE_SCAN_EXT … COMPLETE`
     then runs in the background for the Viewer; `is_covered()` already makes it skip
     re-adding the LFS region (guard the SD sentinel against a double-add).
2. **Build-time offset hint, kept dynamic.** Emit the LittleFS base as a per-variant
   constant (SD build = 8 MiB `0x90800000`; flash-only build = `FrogFS-end → end-of-flash`).
   Validate the superblock magic at each candidate; a wrong guess falls through to the slow
   find. Mirrors the existing `DEFAULT_*` "never assumed correct" philosophy in
   `memory_map.h`. → best of both: O(1) when standard, still correct when not.
3. **Tiny loading bar (safety + honest UX).** Reuse `gui_draw_progress_bar`, extended to a
   variable size, for a **20–50 px wide, centered** bar on the neutral/fallback look against
   the (already) black screen. No color theme applied. Doubles as a "not stuck" indicator.
   If the probe is sub-frame-fast the bar just flashes to 100%.
4. **Secondary — speed the background full-scan** (mostly for a near-empty chip / the
   Viewer): bigger per-frame batch and/or trimmed stride-refinement, since `covered_end()`
   already jumps the big partitions. Lower priority; does not affect time-to-theme.

## Benchmark methodology

Opt-in firmware instrumentation, modeled on `REMOTE_INPUT` (zero bytes in the golden build):

- Build flag `BOOT_BENCH=1` → `-DBOOT_BENCH`. Off by default.
- `src/chainloader/system/bench.h` exposes `BENCH_MARK(i)` → stamps `HAL_GetTick()` (1 ms
  resolution) into `volatile uint32_t g_boot_bench[]` (defined in `main.c`, `__attribute__((used))`).
- Milestones:

  | idx | milestone | captured at |
  |----|-----------|-------------|
  | 0 | committed to launcher | `main.c`, after magic scrub, before `board_init()` |
  | 1 | entering menu loop | first line of `menu_run()` (after OSPI/LCD/asset init) |
  | 2 | first menu frame drawn | after first `ui_draw()` in `menu_run` loop (once) |
  | 3 | module sources ready | theme-load trigger in `menu_main_update` |
  | 4 | theme fully applied | after `theme_modules_init()` returns |

- Derived metrics: heavy init `1−0`; first-paint `2−1`; **theme-recognition delay `3−2`
  (the target)**; theme-load cost `4−3`; total-to-theme `4−1`.

**Read-back (no new script required for baseline):**
```
arm-none-eabi-nm build/app/app.elf | grep g_boot_bench        # resolve SRAM address (0x240xxxxx)
# reset + let it run to the menu, wait ~2-3 s, then:
python3 scripts/debug/memory.py read <addr> 32                # dump 8 words, compute deltas
```
(A thin `scripts/debug/boot_bench.py` that resets, waits, reads, and prints the deltas is a
nice-to-have follow-up; the baseline can be captured with `nm` + `memory.py` as above.)

## Results

Measured on hardware (this unit: SPI1 SD mod, 64 MB extflash). All values ms.
`—` = not yet measured.

| build | init 1−0 | paint 2−1 | **recog 3−2** | load 4−3 | total 4−1 | notes |
|-------|---------:|----------:|--------------:|---------:|----------:|-------|
| baseline (pre-change) | 298 | 15 | **1392** | 9 | 1416 | full `== COMPLETE` gating; 64 MB chip, SD present |
| after decouple+probe  | 298 | 99 | **0** | 9 | 108 | theme at first paint; verified themed (Mario) on HW |
| + empty-chip case     | — | — | — | — | — | near-empty extflash (still TODO) |

**Result (verified on hardware, this unit):** theme applied **460 ms vs 1768 ms** baseline;
recognition delay **1392 → 0 ms** — the theme loads on the same frame the menu first paints.
First-paint moved 15 → 99 ms because the SD mount (~99 ms) now runs in the first frame's
*update* before the first draw; that's the perceived-latency the loading bar will cover next.

**Key finding — the LittleFS is INVERTED.** Its `littlefs` superblock lives in the *last*
two blocks of flash (verified: magic at `0x93FFF008` / `0x93FFE008`), and the nominal start
offsets (8 MiB / 56 MiB) read erased `0xFF`. So the boot probe must look at the **end of
flash** (last 64 KiB), where `check_address()`'s inverted-layout detection finds the
superblock and back-computes the real start. The 8/56 MiB start offsets are kept only as
fallbacks for a hypothetical non-inverted image.

**Bonus — the background scan got faster too.** Front-loading the LFS find means the
exhaustive `SCAN_EXT` no longer grinds down to 64 KiB stride to reach the end-superblock —
the whole LFS region is already registered, so `covered_end()` skips it. `SCAN_EXT` dropped
**1006 → 117 ms**; total background scan **1424 → 552 ms**.

**Baseline scan-stage breakdown** (`boot_bench.py`, this unit, 64 MB chip + SD):

| stage | dur (ms) |
|-------|---------:|
| SCAN_INT1 | 133 |
| GAP_INT1 | 16 |
| SCAN_INT2 | 135 |
| GAP_INT2 | 16 |
| SCAN_SD | 101 |
| **SCAN_EXT** | **1006** |
| GAP_EXT | 17 |
| total scan | 1424 |

Confirms the hypothesis empirically: the external-flash sweep alone is **~1006 ms** of
the 1424 ms scan, and the theme load itself is only **9 ms**. The entire 1.4 s
"recognition delay" is waiting for the exhaustive scan (dominated by SCAN_EXT) to finish
before the trivial theme load is even attempted. Decoupling the load from the full scan —
firing it right after SD + a targeted LFS probe — should collapse `recog` from ~1392 ms to
roughly the SD-detect + LFS-mount cost.

## Implementation checklist

- [x] Benchmark instrumentation (`bench.h` + `BOOT_BENCH` Makefile gate + marks + per-scan-stage timing).
- [x] `scripts/debug/boot_bench.py` (reset → settle → read → print deltas; reuses harness helpers).
- [x] Capture **baseline** numbers on hardware; fill the table. *(recog = 1392 ms; SCAN_EXT = 1006 ms)*
- [x] `STATE_PROBE_SD` / `STATE_PROBE_LFS` reorder + `partition_modules_ready()` checkpoint. *(verified: theme at 460 ms)*
- [x] LittleFS probe at the known offset — the **end of flash** (inverted superblock), magic-validated; start offsets kept as fallbacks. *(verified)*
- [x] Re-point `menu.c` theme trigger from `COMPLETE` to `modules_ready`. (SD added once in `STATE_PROBE_SD` — no double-add.)
- [x] Selection-aware no-flash gate (`ui_manager.c`), module-slot only, with `g_modules_init_done` reachability fallback (reuses `gui_draw_progress_bar`); 20–50 px centered boot bar on neutral look (no color theme).
- [x] Re-measure: timing confirmed (recog 1392 to 0 ms); no-flash eyeball pending. Theme arrives in ~1–2 frames.
- [x] Cooperative SD/scan + LCD overlap: `gui_init`'s LCD settle delays pump `partition_scan_update()` via the `gui_settle_hook` (`boot_idle_pump` in `main.c`), so the SD probe (~97 ms) AND the full partition scan run DURING the panel bring-up instead of sequentially after it. **Boot-to-themed 469 -> 415 ms; the black/flicker gap 106 -> ~22 ms (user-confirmed gone on hardware).** The whole scan finishes by ~217 ms (Partition Viewer instantly ready). NO LCD timing trimmed — the 1V8 settle window just grew 50 -> ~97 ms (more settling, safe). Storage/scan now start before `gui_init` (`vfs_init`/`partition_scan_start` reordered).
  - *(Pending vigorous cold + warm power-cycle verification, since the SD probe now runs during the LCD 1V8 settle — single-hardware caveat.)*
- [x] No-card fast-bail (`sdcard.c`): `spi1_power_on()` now reports whether the card answered CMD0; `spi1_init()` bails in ~200 ms if it didn't, instead of stacking the 200 ms CMD0 + 750 ms ready-wait timeouts. CMD0 is the presence gate (a present card answers in <86 ms; the long ACMD41 wait still applies once a card is there). **No-card boot-to-themed 1260 -> 510 ms (SD detect 959 -> 209 ms).** *(verify detection still reliable WITH and WITHOUT a card, cold + warm.)*
- [ ] Verify on hardware (cold boot, SD present, SD absent, near-empty chip). Update CHANGELOG.
- [x] Menu restructuring *(implemented; main menu verified on HW; submenu/toggle/gray-out pending eyeball)*: `ui_list` gained an `is_enabled` predicate (greyed, non-selectable rows). Main menu = BOOT RETRO-GO / BOOT OFW: < X > / TOOLS / SETTINGS / POWER OFF; boot targets grey out via `board_is_valid_app`, and BOOT OFW greys when neither console is detected. The selector only offers detected consoles; LEFT/RIGHT flashes the other one into Bank 2 (blocking, with progress) and A boots the active OFW. SETTINGS is its own top-level menu (THEME, FAST-BOOT, RESET DEFAULTS — zeros BKP3R, confirm-gated); TOOLS now holds just FILE BROWSER + PARTITION VIEWER.

**Related changes landed alongside (this branch):** backup-register (TAMP->BKP3R) theme encoding moved to per-OFW nibbles — Zelda `[15:12]`, Mario `[11:8]`, fast-boot `[0]`; 6 module slots/console (`MAX_THEME_MODULES`=6); `REMOTE_INPUT` default-on (opt out `REMOTE_INPUT=0`); `BKP3R` zeroed on-device for a clean baseline.

## Risks / failure modes (must stay benign)

- Wrong/absent LFS offset → magic check fails → fall through (slow find or default colors). Benign.
- SD mount slow/absent → bar shows; menu still reachable; default colors if no theme found. Benign.
- Background scan double-adds SD sentinel → guard in `STATE_SCAN_SD`. Cosmetic if missed.
- Theme module references OFW tileset/palette → still gated on `assets_loaded`; unchanged.

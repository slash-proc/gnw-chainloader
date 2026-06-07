# UI Service Refactor (in-core unified UI)

Living roadmap + checklist for unifying the chainloader UI into one in-core
service. Written so a fresh session can continue without re-deriving the design.

## Goal

One in-core UI "service": a ticked, retained-mode subsystem that BOTH the core
and modules drive through a single descriptor-based seam, instead of the
piece-meal `gui_api` draw endpoints. All UI stays in-core (File Browser and
Partition Viewer remain core functionality, NOT modularized). The win is capping
the module API cost (one seam, not N growing endpoints) and unifying rendering
(one selector spacing, one footer, one divider, one chrome, by construction).

## Model (owner's vision)

The window stack in `ui_manager.c` is already a ticked UI service. The core menus,
File Browser, and Partition Viewer already use the shared `ui_list` directly (they
are core code, so they do NOT need the seam). Only MODULES were the exception:
they blocked in their own `run()` and hand-rolled drawing through fine-grained
`gui_api` calls, which is the piece-meal cost. The seam closes that gap.

## AS BUILT (commit 7f53b79)

- `ui_view_desc_t` (in `src/chainloader/system/feature.h`): `{ uint8_t kind
  (UI_VIEW_MENU | UI_VIEW_DUALPANE); const char *title; const char *footer;
  uint16_t header_color; int count; const char *(*get_label)(int);
  void (*on_select)(int); void (*draw_right_pane)(int sel, uint32_t tick); }`.
- Appended to `feature_host_t` (APPEND ONLY, ABI is fine since all modules rebuild
  together): `void (*run_view)(const ui_view_desc_t *desc)` and
  `void (*pane_row)(int slot, const char *label, const char *value, bool marquee,
  uint32_t tick)`.
- Core impl in `feature.c`: `feat_run_view()` is a self-contained per-frame loop
  reusing the existing `ui_list` (`ui_list_init` -> `ui_list_set_split` for
  dual-pane -> on_back flag), drawing chrome via `ui_draw_header_solid` +
  `ui_list_draw(0,22,320,196)` + `ui_draw_footer_chrome` + `gui_refresh`. Geometry
  matches the Partition Viewer page. `pane_row` just calls `ui_list_pane_row`.
- r9: module callbacks are called directly, NO trampoline. r9 is callee-saved and
  the app is `-ffixed-r9`, so the module GOT persists from `run()` through the core
  back into `get_label`/`draw_right_pane` (confirmed vs loader.c `mod_invoke_r9`).
- Module Overview + example migrated: they build a descriptor and call
  `host->run_view`, dropping all hand-rolled nav/draw/selector. Module Overview's
  right pane uses `host->pane_row` for SOURCE/POOL RAM/STATE/MODEL.

Cost: core seam +148 B (1120 -> 972 free). modview.bin -40 B. Net ~neutral; the
payoff is structural (the API stops growing per-operation; modules inherit
divider/selector/scroll/wrap/footer/chrome by construction).

## Gotchas to revisit (from the build)

- `header_color`: `feat_run_view` honors `desc->header_color` by temporarily
  overriding `gui_accent_color` around the header draw then restoring it. Both
  migrated modules pass 0 (theme accent), so the non-zero path is UNTESTED. Verify
  when a module first sets a color.
- B vs START: the shared `ui_list_update` exits on both B and START; old Module
  Overview exited only on B. Minor, consistent with unified nav.
- Footer-divider clipping relies on `ui_footer_top()` returning 218 because the
  launching Tools menu is app-active with a footer. True for current launch sites;
  a launch from a non-app-active context would draw the divider full-height
  (cosmetic only).
- `system/input.h` include in the example module is now unused (harmless).

## Checklist

- [x] Phase 1: dual-pane selector glyph/X, Partition Viewer footer, per-window
      header_color, single chrome path (commit 013202b).
- [x] Corrections: homogeneous indicator-to-text gap; pop-up keeps own title, app
      owns header title (commit 6316295).
- [x] UI service: descriptor seam (`run_view` + `pane_row`) (commit 7f53b79).
- [x] Migrate Module Overview module onto the seam (7f53b79).
- [x] Migrate the example module (7f53b79).
- [ ] HARDWARE-VERIFY (owner): Module Overview + example render through the service
      (divider, themed selector, smooth scroll + wrap, footer, app-owned header
      title) in every theme + RTL; Retro-Go return round trip still works.
- [ ] Migrate the remaining modules (see below).
- [ ] Full documentation (DESIGN.md UI section + finalize this doc).

## Remaining modules + their complexity

- **installer** (`src/modules/features/installer/`): list-based (scan results +
  confirm) -> fits `UI_VIEW_MENU` / a dialog cleanly. Do this one first.
- **mp3** (`src/modules/features/mp3/`): has a CUSTOM player UI (transport, time,
  not a menu). Use `run_view` only for its file picker / add-track menu; the player
  screen stays custom drawing through `gui_api` (the descriptor does not model a
  player, and shouldn't). Same shape for **picture** (custom viewer).
- The core screens (menus, File Browser, Partition Viewer) already use `ui_list`
  directly and need NO change; they are not modules.

## Constraints

- In-core, 40K stub ceiling (972 B free after the seam). Measure the Makefile
  Free-space line every step.
- Verify on hardware each step (the owner tests), including the Retro-Go
  return-to-menu round trip (boot-path law, CLAUDE.md), even though this work does
  not touch the stub/boot-magic path.
- Deferred: the main menu shedding the "GnW Chainloader" header title (later).

## Commit trail

013202b Phase 1 -> 6316295 corrections + this roadmap -> 7f53b79 service seam +
Module Overview + example.

# Module Menu Registration (Feature Modules)

Status: planned, not implemented. Granular checklist for letting PIE modules add
their own Tools/Settings entries and file-type handlers without the core knowing them
by name; complements [ACTIVE_WORK.md](../ACTIVE_WORK.md) and defers memory-map detail
to [DESIGN.md](../DESIGN.md). Engineering invariant: STABILITY IS LAW. Discovery and
dispatch run only after the menu is already reachable; any missing or corrupt feature
module is skipped, never blocks the menu, and never enters the boot path.

## Context and background

The core owns three reusable pieces this builds on:
- The menu framework (`src/chainloader/menu.c`, `ui/ui_list.h`, `ui/ui_manager.c`).
  Each submenu is a `ui_list_t` whose `num_items` is mutable and whose label/action/
  adjust/enable are index-keyed callbacks. Tools is built in `menu_tools_enter()`
  (`File Browser`, `Partition Viewer`; count 2; `TOOLS_ACTIONS[]`,
  `get_tools_label`, `on_tools_action`). Settings is built in
  `menu_settings_enter()` (`SET_IDX_LANGUAGE` 0, `SET_IDX_THEME` 1,
  `SET_IDX_FASTBOOT` 2, `SET_IDX_RESET` 3; `get_settings_label`,
  `on_settings_action`, `on_settings_adjust`).
- The module loader and pool (`src/chainloader/system/loader.c`,
  `system/module.h`): `mod_load*`, and `mod_pool_mark()` / `mod_pool_reset()` for
  transient load-run-free, as the installer already uses in `menu.c`
  (`menu_do_install`).
- Header peeking (`storage/vfs.c`): the core already reads a `module_header_t` from a
  candidate without loading the body.

What is missing: a way for a module to say "list me under Tools as X" or "I handle
`.mp3`" that the core can honor generically.

## Decisions

1. Listing metadata lives in the module header, so the core can read it with the
   existing header-peek (no module code runs, nothing is loaded into RAM to show the
   entry). New fields are appended to `module_header_t` (existing field offsets
   unchanged) and `MODULE_ABI_VERSION` bumps 1 -> 2. All in-repo modules rebuild via
   the `MODULE_HEADER` macro; non-feature modules default to "no menu, no extension".
2. Behavior is provided by the loaded module, not the header. Selecting a feature
   entry runs the transient load-run-free cycle: `mod_pool_mark()` -> load the module
   -> call its `run()` -> `mod_pool_reset()`. The entry persists because the core
   owns the registry (label and path copied into core RAM); the module does not.
3. Discovery scans a dedicated directory `/modules/features/` so the scan is bounded
   and the existing core modules (theme/language/installer/filesystems) are untouched.
   Each `*.bin` there whose header declares a menu id and/or an extension is
   registered.
4. Two generic integration points, neither naming any module:
   - Tools/Settings entries, inserted at fixed positions by menu id.
   - File-browser dispatch by registered extension.
5. Interactive feature modules receive a rich host vtable `feature_host_t`
   (`system/feature.h`): tick, VFS streaming, framebuffer + draw primitives, input
   polling, a `pick_file()` that reuses the core file browser as a modal picker, and
   the existing `mod_ui` confirm/error. Audio is NOT in the host (the core has none);
   a module that needs audio brings it up itself.
6. Placement rules: Tools feature entries go after `Partition Viewer`; Settings
   feature entries go between `Fast-Boot` and `Reset Defaults`, and `Reset Defaults`
   is computed to remain last.
7. Feature menu entries are launchers (A activates `run()`); LEFT/RIGHT adjust is not
   offered for them in v1 (they are not in-place selectors).
8. Labels are module-owned and baked English in the header (the core cannot have a
   translation id for an unknown module). This matches the existing precedent that
   the installer prompt is English. No `STRINGS_ABI` involvement.

## Architecture touchpoints

Header manifest, appended to `module_header_t` in `src/chainloader/system/module.h`
(after `flags`, so existing offsets are stable):

```c
/* Feature-module manifest (ABI 2). Zeroed/empty on non-feature modules. */
uint8_t  menu_id;        /* MODULE_MENU_NONE=0, _TOOLS=1, _SETTINGS=2 */
uint8_t  _pad[3];
char     menu_label[24]; /* English label shown in the menu; "" if none */
char     file_ext[8];    /* lowercase extension this module handles, e.g. "mp3"; "" if none */
```

The `MODULE_HEADER` macro gains `-DMODULE_MENU_ID`, `-DMODULE_MENU_LABEL`,
`-DMODULE_FILE_EXT` inputs (defaulting to 0 / "" / ""). Header-peek sites must read
the full `sizeof(module_header_t)` (the installer's fixed 32-byte peek is widened or
switched to the struct size).

Feature interface `src/chainloader/system/feature.h` (modeled on
`system/installer.h` and the theme host API):

```c
#define MODULE_MENU_NONE     0
#define MODULE_MENU_TOOLS    1
#define MODULE_MENU_SETTINGS 2

typedef struct {
    uint32_t (*get_tick)(void);
    /* VFS streaming over the active mounted driver. 0 = ok. */
    int  (*open)(const char *path, void **ctx);
    int  (*read)(void *ctx, void *buf, uint32_t n, uint32_t *got);
    int  (*seek)(void *ctx, uint32_t pos);
    void (*close)(void *ctx);
    int  (*file_size)(const char *path, uint32_t *out);
    int  (*list_dir)(const char *dir, char *names, int stride, int max);
    /* Reuse the core file browser as a modal picker; returns 0 and fills out_path
     * on selection, nonzero if the user backs out. ext may be "" for any file. */
    int  (*pick_file)(const char *ext, char *out_path, int cap);
    /* Drawing (point at gui_* core primitives). */
    uint16_t *(*framebuffer)(void);
    void (*draw_text)(int x, int y, const char *s, uint16_t color);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*progress_bar)(int x, int y, int w, int h, int pct, uint16_t b, uint16_t f);
    void (*present)(void);
    /* Input polling. */
    void (*input_update)(void);
    int  (*just_pressed)(int button);
    int  (*is_pressed)(int button);
    /* Simple dialogs (the existing mod_ui seam). */
    const struct mod_ui *ui;
} feature_host_t;

typedef struct {
    /* path is the file selected in the browser, or NULL when launched from a menu
     * entry (the module then uses host->pick_file or its own list). Blocks until the
     * user exits. */
    void (*run)(const feature_host_t *host, const char *path);
} feature_api_t;

bool mod_load_feature(const char *path, const feature_host_t *host, feature_api_t *out);
```

Core registry (new `src/chainloader/system/feature.c`, registered like
`theme_register`):

```c
typedef struct { uint8_t menu_id; char label[24]; char ext[8]; char path[64]; } feature_entry_t;
/* small fixed array, e.g. MAX_FEATURES = 8 */
int  feature_count(int menu_id);                 /* entries for MODULE_MENU_TOOLS/_SETTINGS */
const char *feature_label(int menu_id, int n);   /* nth label within that menu */
void feature_launch(int menu_id, int n);         /* transient run, path = NULL */
const feature_entry_t *feature_by_ext(const char *ext); /* file-browser dispatch */
void feature_launch_path(const feature_entry_t *e, const char *path);
```

## Task 1: header manifest + ABI bump

- [ ] Append `menu_id`, `_pad`, `menu_label[24]`, `file_ext[8]` to `module_header_t`
      in `system/module.h`; extend the `MODULE_HEADER` macro with the three new
      `-D` inputs (defaults 0/""/""); define `MODULE_MENU_NONE/_TOOLS/_SETTINGS`.
- [ ] Bump `MODULE_ABI_VERSION` 1 -> 2. The loader's `module_abi_ok` gate then
      rejects any stale ABI-1 artifact; the installer gates with the core's ABI, so
      this stays consistent (see the abi notes in [DESIGN.md](../DESIGN.md)).
- [ ] Widen every header-peek to read `sizeof(module_header_t)` (installer's 32-byte
      peek, `vfs` header reads).
- [ ] Rebuild all in-repo modules (theme, language, installer, filesystems) so their
      headers carry ABI 2 with an empty manifest. Mechanical: they pass no menu `-D`s.

## Task 2: feature discovery + registry

- [ ] New `system/feature.c` / `system/feature.h`: the registry array and the
      accessors above, plus `feature_register(menu_id, label, ext, path)` (mirrors
      `theme_register`).
- [ ] Discovery in `menu.c` `menu_main_update()`, inside the existing
      `partition_modules_ready()` one-shot (right after `theme_modules_init()` /
      `i18n_init()`): enumerate `/modules/features/*.bin` (VFS readdir, the same
      enumeration used for `/modules`), peek each header, and for each with
      `menu_id != NONE` or `file_ext[0]` set, call `feature_register(...)` copying the
      label/ext/path into the core. Skip any that fail magic/ABI (stability).

## Task 3: splice the registry into Tools and Settings

- [ ] Tools (`menu.c`): keep static idx 0 `File Browser`, idx 1 `Partition Viewer`.
      `get_tools_label(idx)`: idx < 2 static, else
      `feature_label(MODULE_MENU_TOOLS, idx-2)`. `on_tools_action(idx)`: idx < 2 ->
      `TOOLS_ACTIONS[idx]()`, else `feature_launch(MODULE_MENU_TOOLS, idx-2)`. In
      `menu_tools_update`, set `g_list_tools.num_items = 2 + feature_count(TOOLS)`
      each frame.
- [ ] Settings (`menu.c`): keep idx 0..2 static (Language, Theme, Fast-Boot). Replace
      the fixed `SET_IDX_RESET 3` with a computed `settings_reset_idx() = 3 +
      feature_count(SETTINGS)`. `get_settings_label`/`on_settings_action`: idx 0..2
      static; idx in [3, reset) -> `feature_label/launch(SETTINGS, idx-3)`; idx ==
      reset -> Reset Defaults. `on_settings_adjust` ignores feature indices. In
      `menu_settings_update`, set `num_items = 4 + feature_count(SETTINGS)` each frame.
- [ ] No change to `ui_list.h/.c`; the widget already honors a runtime `num_items`.

## Task 4: transient dispatch + host vtable

- [ ] `mod_load_feature` in `system/loader.c` (one function, copy of
      `mod_load_installer`).
- [ ] `feature_launch` / `feature_launch_path` (in `feature.c` or `menu.c`):
      `mod_pool_mark()` -> `mod_load_feature(entry->path, &g_feature_host, &api)` ->
      `api.run(&g_feature_host, path_or_NULL)` (blocks) -> `mod_pool_reset(mark)`.
- [ ] Fill `g_feature_host` with core primitives: `get_tick = HAL_GetTick`;
      `framebuffer = gui_current_framebuffer`; `draw_text/fill_rect/progress_bar` =
      the `gui_*` calls; `present` = the core frame flush; input forwards; VFS
      open/read/seek/close/list_dir/file_size as shims over the active
      `vfs_driver_t` (same lookup the file browser uses for copy/delete); `ui =
      mod_ui()`; `pick_file` per Task 5.

## Task 5: file-browser picker mode + extension dispatch

- [ ] In `ui/ui_file_browser.c`, add an "open as picker" mode: a result callback +
      optional extension filter; on file select it invokes the callback with the full
      path and pops. `pick_file(ext, out, cap)` in the host runs this modally and
      returns the selection. Reusable by any feature module.
- [ ] In the browser's file action: extract the lowercase extension, call
      `feature_by_ext(ext)`; if a handler is registered, `feature_launch_path(e,
      fullpath)` (built with the existing `join_path`); otherwise fall through to the
      current context menu. The core names no extension.

## Task 6: build + deploy

- [ ] Makefile: a `/modules/features/` build convention; a feature module passes the
      menu `-D`s (e.g. `-DMODULE_MENU_ID=MODULE_MENU_TOOLS
      -DMODULE_MENU_LABEL='"MP3 Player"' -DMODULE_FILE_EXT='"mp3"'`) plus
      `-DMODULE_FLAGS=MOD_FLAG_TRANSIENT`.
- [ ] Add `/modules/features/<name>.bin` to the installer's `MODULE_PATHS[]` and the
      flash/deploy chain that ships `/modules/*.bin`.

## Verification

1. `make clean && make -j16`; confirm all modules rebuild at ABI 2 and the core size
   is essentially unchanged (the framework is small; no decoder/audio in core).
2. Boot path intact with NO feature modules present: menu reaches normally; run
   `scripts/tests/retrogo_return_test.py` (no boot-magic change).
3. With a dummy feature module installed (a trivial one that just draws "hello" and
   waits for B), OCR-nav (`scripts/common/ocrnav.py`) to confirm: it appears in Tools
   after Partition Viewer, or in Settings between Fast-Boot and Reset Defaults with
   Reset still last; selecting it loads, runs, and on B returns; entering it twice
   still works (pool slot reclaimed).
4. Corrupt/absent feature module: confirm it is skipped and the menu is unaffected.
5. File-browser dispatch: a file with the dummy's extension launches it; other files
   still open the normal context menu.

## Risks and blockers

- ABI bump touches every module header; verify the loader gate and installer gate
  both move to ABI 2 together so nothing stale is loaded or installed.
- Late discovery: features register after the first paint (same as theme/language).
  Recomputing `num_items` per frame in the update handlers absorbs the growth without
  a visible glitch; verify the cursor position stays valid when the count grows.
- `pick_file` modal reentry: opening the browser from inside a transient module's
  `run()` must not corrupt the window stack; confirm push/pop balance and that the
  feature module is still resident (not yet `mod_pool_reset`) while its `pick_file`
  callback runs.
- Header size growth: confirm no peek site assumes the old header size.

## Future work (out of scope here; add to ACTIVE_WORK.md when this lands)

- A "Module Manager" entry under Tools to enable/disable installed modules (would
  read the same registry and an enable flag; a disabled module is skipped at
  discovery). Noted at the user's request; not implemented in this pass.

## Documentation updates (same pass)

- `ACTIVE_WORK.md`: a task group for the framework plus the Module Manager follow-up.
- `DESIGN.md`: a subsystem section (the header manifest, the registry, the transient
  dispatch, the two integration points).
- `README.md`: one plain line ("installed modules can add their own menu entries").

## Critical files

Create: `system/feature.h`, `system/feature.c`, `docs/module-menu-registration.md`.
Modify: `system/module.h` (manifest + ABI bump), `system/loader.c`
(`mod_load_feature`), `menu.c` (discovery + Tools/Settings splice + `g_feature_host` +
`feature_launch`), `ui/ui_file_browser.c` (picker mode + extension dispatch),
installer `module_entry.c` (header peek width + feature path list), `Makefile`,
plus rebuild all module headers at ABI 2.

# Module-facing API (feature modules)

Everything a transient feature module (a PIE module loaded by `feature_launch`)
can call. A module links NO core globals (it is position-independent and built
separately); it reaches the core ONLY through two vtables the core hands it at
launch: `feature_host_t` (the host services, the module's main API) and
`gui_api_t` (the themed draw toolkit, reached as `host->gui`). Both are
APPEND-ONLY (see the ABI note at the bottom).

For how a module declares its menu placement / handled file extension, see
`docs/module-menu-registration.md`. For the unified-UI design, see
`docs/ui-service-refactor.md`.

## Preferred: the high-level UI seam

`host->run_view(const ui_view_desc_t *desc)` renders a whole screen through the
in-core `ui_list` + chrome and runs its own input loop until B/back, calling the
descriptor's callbacks each frame. Use it for any screen that is a list or a
list + detail; the module then inherits the themed selector, smooth scroll +
wrap, divider, and consistent header/footer for free (identical to the core
File Browser / Partition Viewer).

`ui_view_desc_t`:
- `kind`: `UI_VIEW_MENU` (list only) or `UI_VIEW_DUALPANE` (left list + right detail).
- `title`: header title. The running app owns it; the service draws it.
- `footer`: footer legend, NULL = none.
- `header_color`: 0 = theme accent, else a custom RGB565 header.
- `count` + `get_label(i)`: the rows.
- `on_select(i)`: A pressed on row i. NULL = a back-only (read-only) view.
- `draw_right_pane(sel, tick)`: the dual-pane detail; NULL for a plain menu. Draw
  label/value rows with `host->pane_row(...)`.

`host->pane_row(slot, label, value, marquee, tick)`: one right-pane label/value
row (slot 0..3 -> y 40/75/110/145), used inside `draw_right_pane`.

## feature_host_t (host services)

Lifecycle / launch
- `run(host, path)`: the module's entry (in `feature_api_t`); `path` is the
  launched file or NULL for a menu launch. Blocks until back.
- `is_first_launch()`: true only on the first feature launch since power-on.

Input
- `input_update()`, `just_pressed(btn)`, `is_pressed(btn)` (INPUT_* from input.h).

Frame
- `get_tick()`, `framebuffer()` (320x240 RGB565 back buffer), `present()` (flush + swap).

Chrome
- `draw_header(title)`, `draw_footer(hint)`: the themed header/footer bars.

Convenience draw (also available via `gui`)
- `draw_text`, `fill_rect`, `progress_bar`.

Modals
- `ui` (`mod_ui_t`): confirm/error dialogs. (Also `gui->confirm/error/notice/context_menu`.)

Themed toolkit
- `gui` (`gui_api_t *`): the full draw toolkit, see below.

File picker
- `pick_file(ext, out, cap, &size, &is_dir)`: run the in-core file browser as a
  modal picker (filtered to `ext`, "" = all); blocks until a pick or back.

Launched-file streaming (the file the module was launched on)
- `file_open(path)` / `file_read(f, buf, n)` / `file_seek(f, off)` / `file_close(f)`.
- `list_dir(dir, cb, user)`: enumerate a directory on the launched file's partition.

Persistent state
- `state_save(blob, len)` / `state_load(blob, cap)`: a per-module blob at
  `/modules/state/<name>`, survives power-off, degrades gracefully without RW LFS.

File source
- `source_id()` / `set_source(id)`: opaque token for the current file-source
  partition (persist with state, re-select on restore).
- `set_source_sd()`: point the source at the SD card if present.

Working memory
- `scratch_get(size)`: borrow up to 64 KB of idle D2 SRAM (CPU-only, NOT
  DMA-reachable; exclusive to the running module, no free needed).

Loader registry (what Module Overview reads)
- `mod_count()` / `mod_get(i, &mod_view_t)`: enumerate loaded modules
  (name, XIP-or-RAM, pool bytes, flags, resident).

UI service seam (appended this session)
- `run_view(desc)`, `pane_row(...)`: see the seam section above.

## gui_api_t (themed draw toolkit, via host->gui)

Direction / measure
- `is_rtl()`, `mirror_x(x, elem_w, box_x, box_w)`, `text_width(str)`.

Text (all RTL-aware)
- `draw_text`, `draw_text_aligned`, `draw_text_marquee` (scrolls long text),
  `draw_char(cp)` (a Unicode codepoint), `draw_selector`.

Primitives
- `draw_rect`, `fill_rect`, `blend_rect` (dim a region), `draw_progress_bar`,
  `draw_sprite`.

Theme colors (track the active theme)
- `color_bg`, `color_fg`, `color_accent`, `color_border`, `color_footer`.

Frame
- `framebuffer()`, `refresh()`.

Modals (async: push a window and return; the callback is resident module code)
- `confirm(msg, on_yes)`, `error(msg)`, `notice(msg)`,
  `context_menu(title, options, count, on_select)`.

i18n
- `tr(id)`: the core's active translations for shared words.
- `lang_code()`: the active locale (e.g. "de_DE", "en_US").

## What a module can build

- Any list menu or list + detail (dual-pane) screen via `run_view`, identical to
  the core UI.
- Confirm / error / notice dialogs and context menus via the modals.
- A file-pick flow via `pick_file`.
- A fully custom screen (a player transport, a picture canvas, a waveform) by
  drawing with the `gui` primitives + its own input loop + `present`, still in the
  active theme with RTL handled automatically.

## Limitations

- `run_view` models lists and dual-panes only. An arbitrary custom layout is still
  hand-drawn with the `gui` primitives (it matches the theme and gets RTL for
  free, but it is not "free" like a list). mp3's player and picture's viewer are
  exactly this case: they use `run_view` for their menus/pickers and draw their
  unique screen themselves.
- The descriptor is text rows + an optional detail pane. It does not yet surface
  the richer `ui_list` features the core uses internally: value-adjust (LEFT/RIGHT
  cycling, the Settings "< Theme >" item), per-row enable/disable, or the
  GAME-button hook. Adding them is an append-only descriptor extension.
- A module cannot yet register a named sub-menu under Tools (a future seam; the
  registry already lists flat Tools entries).
- Non-UI services are deliberately narrow: only the launched file + the picker +
  `list_dir` reach the filesystem; there is no arbitrary path access, no raw
  flash/partition access, no network. Anything new is a new appended host function.

## ABI rule (why old modules keep working)

Both vtables are APPEND-ONLY: add new entries at the END, never reorder, remove,
or change the type/meaning of an existing one. The core hands the module a pointer
to the struct and the module reads each field by offset, so appending leaves every
existing field where it was. An old module binary reads the old fields at the same
offsets and never touches the new ones, so it keeps running on a newer core without
a rebuild. A genuinely incompatible change requires bumping `MODULE_ABI_VERSION`,
which makes the core REJECT old modules (a loud refusal, not silent garbage) and
forces a rebuild.

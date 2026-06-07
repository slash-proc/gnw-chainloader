# ACTIVE_WORK.md

This file tracks the current working session: two tiers of checklist — **Overarching Goals** and the granular **Tasks** grouped under them — plus a **Debugging** log and long-lived reference notes. The overarching goals mirror the Project Goals in [DESIGN.md](DESIGN.md); keep them in sync. Completed fixes and tasks are logged in [CHANGELOG.md](CHANGELOG.md), not here.

---

## Overarching Goals

- [x] **0. External Flash (OSPI) Chip Detection** *(completed)* — Resolved hardware probing failure by mirroring working bootloader initialization; correctly identifies chip size.
- [x] **1. Headless, button-driven boot** *(completed)* — instant display-off boot; 1-2-3 priority (Buttons > Magic > Launcher).
- [x] **2. Dual-boot via bank swap** *(completed)* — Run the stock OFW (Bank 2) and Retro-Go (Bank 1) side-by-side via the `SWAP_BANK` option bytes. Once swapped into the OFW, operation is clean (§6).
- [x] **3. Recovery hook** *(completed)* — START at boot / LEFT+GAME in-game returns to the launcher.
- [x] **4. Themed menu with authentic game art** *(completed)* — Mario & Zelda art extracted from the real ROMs.
- [x] **5. On-screen diagnostics & system info** *(completed)* — battery, bank use, FPS, magic cells in a toggleable submenu.
- [x] **6. Read-only File Browser** *(completed)* — Browse filesystems (LittleFS/FrogFS/FAT) directly from the launcher.
- [x] **7. Power management** *(completed)* — Unified power button handling and 30s inactivity auto-hide mode.
- [x] **8. Flash size reduction** *(completed)* — Dieted RAM-boot + 128KB LZMA dictionary freed ~10KB total.
- [x] **9. Boot resilience hardening** *(completed)* — Protected Retro-Go warm resets via DTCM Safe Zone and direct stub jumps.
- [x] **10. Refactor and Modularize Codebase** *(completed)* — Clean architectural separation of engine, storage, and UI.
- [x] **11. Filesystem Integration & Dynamic Theming** *(completed)* — Enabled Read-Write LittleFS, full exFAT/LFN, and RTC timestamps.
- [x] **12. Codebase Bloat Cleanup** — Reduce technical debt and binary size by removing dead code and consolidating duplicates (see `docs/codebase-report.md`).

## Tasks

### Goal 1 — Headless, button-driven boot
- [x] Implement Toggle Fast-Boot setting via backup register 3 with PAUSE/SET god-mode override. *(verified on hardware)*

### Goal 12 — Codebase Bloat Cleanup
- [x] **Safe Deletions** (Items 1, 5, 6, 8, 16, 17, 28)
    - [x] Delete `src/patch/mx_ospi_init.c` (Item 1).
    - [x] Remove `ui_draw_battery` and `ui_draw_simple_background` (Items 5, 6).
    - [x] Remove `asset_loader_init` (Item 8).
    - [x] Remove dead declarations: `gui_draw_theme_background`, `menu_flash_firmware`, `lfs_test_mount` (Items 16, 17, 28).
- [x] **Feature Toggles**
    - [x] Introduce build-time flags for `int_to_str_w` and its hidden dependencies (Item 9, 14).
- [x] **Asset Generation Optimization**
    - [x] Fix `cook_assets.py` to only bake referenced assets and remove hardcoded count (Item 21).
- [x] **Structural Consolidation**
    - [x] Merge `update_progress` and `update_flash_ui` in `partition.c` (Item 11).
    - [x] Parameterize bank-swap helpers in `banks.h` (Item 12).
- [x] **Refactoring & Cleanup**
    - [x] Fix hardcoded asset count and minor API inconsistencies (Items 15, 18, 23, 26).
- [x] **Phase 2: Boot & Structural Deduplication**
    - [x] Consolidate boot-magic logic and delete `stub_logic.c` (Items 2, 3).
    - [x] Consolidate shared interrupt handlers and spin delays (Items 4, 29).
    - [x] Optimize partition sorting in `partition.c` (Items 13, 14).
    - [x] Minor cleanups: `LZMA_HEAP_RESET` macro and GPIO init comments (Items 24, 27).
- [x] **Phase 3: GUI Font Optimization**
    - [x] Replaced 512-byte RAM `dynamic_font` with direct-indexed flash `unscii-8` subset (ASCII + 8 common diacritics).
    - [x] Added minimal UTF-8 decoder to `gui_draw_text` for combining diacritics support.
- [x] **Phase 4: Byte-focused DRY pass** *(verified on hardware via framebuffer capture)*
    - [x] Collapsed `gui_draw_text` into `gui_draw_text_marquee` (removed the duplicate UTF-8 decoder + draw loop).
    - [x] Added `gui_clip_rect` helper; merged `gui_draw_dim_rect`/`gui_draw_frost_rect` into `gui_draw_blend_rect(...,frost)`.
    - [x] Removed redundant extension-uppercasing in the file browser (`gui_draw_char_clipped` already uppercases).
    - [x] Table-drove GPIO setup (`MX_GPIO_Init`, `HAL_OSPI_MspInit`, `HAL_LTDC_MspInit`) via a shared `gpio_init_table`.
    - Result: app `.text` 39,672 → 39,128 B; flashed payload 27,928 → 27,562 B (−366 B). Note: with `-flto`+`--gc-sections`, unreferenced functions are already stripped, so removing dead code (intentionally skipped this pass) yields ~0 bytes — wins came from consolidating *linked* code.
- [x] **Phase 5: Low-Level Register & Boot Optimization** *(verified on hardware)*
    - [x] Truncate the stub's interrupt vector table in `startup.s` to exclude unused external interrupts (-620 B).
    - [x] Replace all HAL GPIO write/read calls with bare-metal `BSRR`/`IDR` register writes/reads across `gui.c` and `board.c`, allowing the compiler to strip `HAL_GPIO_Init`.
    - [x] Replace `HAL_PWR_EnableBkUpAccess()` with direct register writes to `PWR->CR1` to enable compiler stripping of this HAL routine.
    - [x] Simplify `stub_lzma_alloc` by removing runtime reset logic and bounds checking.
    - [x] Optimize `board_is_valid_app` using high-byte checks before checking sub-boundaries to minimize instruction counts.
    - Result: final binary size 32,752 → 31,984 B; free space 16 → 784 B (+768 B).
- [x] **Phase 6: Retro-Go SD Card Makefile Targets**
    - [x] Renamed `build_retro_go`/`flash_retro_go` to `build_rg`/`flash_rg`.
    - [x] Implemented dynamic `RG_FILESYSTEM_SIZE` calculation based on `RG_SD_CARD` state.
    - [x] Updated `flash_rg` to conditionally skip `frogfs.bin` and shift `littlefs.bin` to 8MB when building for SD card variants.
    - [x] Added `build_rg_sd` and `flash_rg_sd` targets.
### Goal 4 — Themed menu with authentic game art
- [x] Implement Universal Asset System: 'Cooker' pipeline, LZMA Super-Blob, and unified `gui_draw_asset` API.
- [x] Fix Mario font glyph corruption via palette buffer expansion (128 entries) and cache alignment.
- [x] Fix jumbled multi-tile sprites via grid-aware horizontal flipping.
- [x] Implement authentic Zelda II Link walking sprites extracted from the ROM.
- [x] **Usage-Based Dynamic Asset Cooking**
    - [x] **Pipeline Intelligence**: Refactor `cook_assets.py` to recursively scan `src/chainloader/` for `ASSET_` symbols.
    - [x] **Dynamic Filtering**: Only bake assets into `assets_gen.c` if they are actually referenced in the source code.
    - [x] **Font Standardization**: Abandon OFW font extraction; standardize on the clean 8x8 `fallback_font` for all themes.
    - [x] **Space Optimization**: Drastically reduced binary bloat; Mario assets now use compact tile indices.
- [x] Build richer / less-janky Zelda sprite scenes. (Gilded Forest theme + Moss Green).
- [x] Fix the weird Triforce in the top-center of the Zelda menu. (Removed crude rectangles).
- [x] Remove "active OFW" text from themed menus while preserving layout spacing.
- [x] Consolidate debug scripts (Mario+Zelda+NES) onto the shared `scripts/common/` library + 7 parameterized tools.
- [x] Flash and verify the visual menu output on-screen.

### Goal 7 — Flash size reduction
- [x] Implement RAM-boot architecture: Flash stub decompresses main launcher into AXI-SRAM.
- [x] **Optimized Space-Saving Architecture**
    - [x] Moved boot hierarchy (Button God-Mode, SRAM Magic) into the compressed app.
    - [x] Minimized Flash Stub: Bare-metal clock/power setup, POR standby check, and immediate LZMA inflation.
    - [x] Maximized LZMA Dictionary: 128 KiB dictionary provides massive compression for the exFAT-enabled app.
- [x] Drop heavy standard library usage: `sprintf` replaced by lean custom formatting.
- [x] Aggressive LTO and optimization flags tuning.

### Goal 11 — Filesystem Integration & Dynamic Theming
- [x] **LittleFS write support**
    - [x] Remove `LFS_READONLY` from `lfs_config.h` (via Makefile CFLAGS).
    - [x] Implement `lfs_flash_prog` and `lfs_flash_erase` in `lfs_wrapper.c` using OSPI APIs (with mode switching).
    - [x] Widen cache/page buffers to 256 bytes.
    - [x] Expose `lfs_mount_at(addr, block_count)` for dynamic mount from the partition scanner.
- [x] **Lightweight FrogFS reader**
    - [x] Create `src/chainloader/storage/frogfs_reader.h` and `frogfs_reader.c`.
    - [x] Implement `frogfs_open_dir`, `frogfs_read_dir`, `frogfs_close_dir` using direct header parsing (no decompression, no malloc).
- [x] **FatFS with exFAT (Dynamic Driver Loading)**
    - [x] Exclude FatFs from main launcher to fit within 32,767 bytes limits (Final: 30,780 bytes).
    - [x] Create Virtual File System (VFS) abstraction (`vfs.c`/`vfs.h`) with on-demand RAM loading.
    - [x] Create dynamic `fatfs.bin` compiled to target SRAM origin `0x240A0000`.
    - [x] Implement dynamic driver load from `/drivers/fs/fatfs.bin` on SD card access.
    - [x] Deploy chainloader and driver binaries to device.
    - [x] Verify dynamic filesystem mounting and read-write/delete operations on-device (waiting for SD card mod hardware installation; dynamic driver loading, mounting, and deployment automated via Makefile).
- [x] **Dynamic LittleFS RW Driver**
    - [x] Extract LittleFS RW logic into dynamic driver `lfs_rw.bin` compiled to target SRAM origin `0x240C0000`.
    - [x] Implement dynamic loading of `lfs_rw.bin` in AXI SRAM when mounting a LittleFS partition.
    - [x] Update `vfs_get_driver` to search registered drivers in reverse order, allowing dynamic driver override.
    - [x] Implement the `install` Makefile target for both `fatfs.bin` and `lfs_rw.bin`.
    - [x] Polish File Browser / VFS copy-paste, error modals, Partition Viewer, and extract Zelda tileset.

## Debugging Log
- **BOOT REGRESSION (128KB DICT):** Decompression hung because `props` in `stub_main.c` were hardcoded for 16KB. Fixed by synchronizing properties to `{0x5D, 0x00, 0x00, 0x02, 0x00}`.
- **BOOT REGRESSION (STUB):** Stub failed to boot because AXI-SRAM clocks weren't enabled. Fixed by moving the LZMA heap back to DTCM (80KB) and keeping the app decompression target in AXI-SRAM (Oneshot mode).
- **RETRO-GO RETURN:** Retro-Go warm resets were failing because the stub was overwriting game state in AXI-SRAM. Fixed by implementing a 4KB DTCM Safe Zone and stub-level direct-jumps for `CORE`/`RESET` magic words.
- **MARIO COLOR SHIFT:** Yoshi colors were garbage due to a 1-pixel offset in the NES sub-palette mapping (81 instead of 80). Corrected in `assets.c`.
- **FONT ORIENTATION:** Bitmaps for `<`, `>`, `/`, and `\` were inverted because lower bits are on the left. Corrected in `font_utils.c`.
- **STUB OVERFLOW (32 BYTES):** Resolved by removing HAL_PWR dependency from the stub and consolidating PD1/PD4 power-on to a single BSRR register write.
- **OFW THEME RELOAD:** Flashing a new OFW didn't trigger a theme switch. Fixed by ensuring `board_detect_console_type()` is called after the flash operation.
- **File Browser filename auto-scroll resolved.** Widened filename caches from 32 to 64 bytes and corrected `max_w` clipping boundaries in `ui_list_draw` to align with the actual screen width (320px) minus the starting offset.
- **LittleFS mount failure (-12) resolved.** Fixed by always declaring `prog_buffer` and setting `prog_size` to `LFS_CACHE_SIZE`.
- **"White Screen" after OSPI fix.** Consolidated power-on to `board_early_init`, matched the proven reset sequence.
- **THEME & UI REFINEMENTS:** Fixed `ui_current_theme` tracking, corrected Mario clouds animation trigger, removed glitchy walking Link, added animated fairy menu selector for Zelda, and fixed vertical spacing in context menu modals. Added standalone drivers Makefile with an `install` target to automate driver deployment.
- **DYNAMIC LFS RW DRIVER & UI POLISH:** Created dynamic LittleFS RW driver (`lfs_rw.bin` loaded at `0x240C0000` concurrently with `fatfs.bin` at `0x240A0000`). Changed tab formats in file browser to `1) SELECT FS >` / `< 2) SELECT FS`, removed copy/paste completion popup confirmations (only keep delete confirm), increased modal background blend opacity to 75%, and updated `ui_list_init` to skip default selection on dividers.
- **VFS COPY-PASTE AUTO-MOUNTING & UI FINISHING:** Implemented auto-mounting/driver-loading of the source partition in `perform_paste()` when copying files across tabs. Added `fs_is_rw` detection for the LittleFS partition label (`LITTLEFS RW` vs `LITTLEFS`). Created a dedicated press-any-button `ERROR` modal (`ui_show_error`). Fixed the Partition Viewer `-INTFLASH-` divider scrolling issue by initializing `scroll_y = 0`. Relocated `build_zelda` to `build/zelda` and copied the newly generated labeled Zelda tileset to `backup/`.
- **LFS WRITE FREEZE & FROGFS COPY ERROR:** Identified stack-use-after-return memory corruption in `lfs_vfs_open` across both host (`lfs_wrapper.c`) and dynamic (`driver_entry.c`) LFS drivers due to temporary stack allocation of `struct lfs_file_config` (which the LittleFS file structures persist pointers to). Fixed by allocating the configs in static arrays. Added automatic unmounting of host LFS during dynamic driver loading (`vfs.c`) to prevent dirty mount states. Added compression rejection for FrogFS file access in `frogfs_reader.c` to fail gracefully on compressed files.
- **FROGFS FILENAME BUG & PARTITION SPACE PANE:** Fixed a critical bug in `frogfs_get_file_data` where `strcmp` was used on memory-mapped FrogFS filenames that are not null-terminated, causing file open failures; resolved by using segment-size-aware `memcmp`. Gated the context actions `COPY` on file accessibility so natively compressed files are safely ignored. Added `statfs` capability to the VFS and all drivers (host LFS, dynamic LFS RW, dynamic FAT, memory-mapped FrogFS), displaying Free and Used space in a new side pane on the partition select screen. Removed the `VIEW` action and prefix tab markers to save space, and refactored `format_size` using a DRY `format_unit` helper to format sizes with 1 decimal place (e.g. 1.5MB) when not exact multiples (resulting in 36 bytes of free space).
- **ZELDA THEME COLOR & GOLD BORDERS:** Adjusted Zelda background green to a lighter, tunic-inspired green (#1E6030) and brightened the gold accent (#E5B83B). Refactored window chrome, split panel separators, progress bars, and footer backgrounds to dynamically display gold accents/borders in the Zelda theme by introducing a global `gui_border_color` variable and `COLOR_BORDER` macro. This saved flash space, resulting in 44 bytes free (gained 8 bytes over the starting 36 bytes).
- **ZELDA SELECTION SPRITE:** Swapped the incorrect NES metasprite fairy for the correct Game & Watch clock fairy animation selection sprites (tile 190.0/190.1 left/right facing) as the item select indicator, centered at coordinate `x + 6`.
- **MENU ITEM VERTICAL CENTERING:** Dynamically centered menu items vertically in the Main and Tools menus when they contain fewer lines than the visible limits (using a mathematically exact centering offset calculation: `(h - count * 20 + 12) / 2`). Split pane lists (e.g. File Browser, Partition Viewer) are explicitly excluded from vertical centering, remaining top-aligned starting at `y + 5`. Reverted horizontal alignment back to left-aligned. Optimized list update logic, leaving 16 bytes free.
- **File Browser ".." Navigation:** Added ".." parent directory link in the file browser when inside subdirectories, blocking DELETE and COPY options on the parent link, and restoring directory highlight on back navigation. Verified build and flash.




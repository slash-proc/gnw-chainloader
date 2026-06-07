# Chainloader Function Catalog & Code Review
_Comprehensive analysis of all project source files (excluding deps/Drivers). Every function listed with true purpose derived from code. Bloat/redundancy has been largely eliminated._

---

## stub_main.c

| Function | Purpose |
|---|---|
| `stub_lzma_alloc` | Bump-pointer allocator backed by 80 KB static arena. Uses `LZMA_HEAP_RESET` macro to reset. 4-byte aligned. |
| `stub_lzma_free` | Intentional no-op. Required to satisfy `ISzAlloc` vtable. |
| `main` | Stub entry point. Calls `board_early_init()`, checks for POR → enters Standby, checks Retro-Go warm-reset magic via shared `boot_magic_check()` → jumps to `RETROGO_BASE`, then decompresses LZMA launcher into AXI-SRAM and jumps to it. |

---

## stub_logic.c
**DELETED.** Dead boot-decision engine removed; logic consolidated into `stub_main.c` and shared helpers.

---

## main.c

| Function | Purpose |
|---|---|
| `app_early_logic` (static) | Pre-menu boot-decision for the full RAM launcher. Uses shared `boot_magic_check()` helper to evaluate SRAM/RTC magic cells. |
| `main` | Full launcher entry point (runs from RAM). Orchestrates hardware init, early logic, and launches the themed menu. |

---

## board.c

| Function | Purpose |
|---|---|
| `start_app` (static, naked) | Two-instruction assembly trampoline for launching target firmware. |
| `Error_Handler` / `wdog_refresh` / `SysTick_Handler` | **Shared System Services.** Consolidated here to serve both the stub and the full application. |
| `board_early_init` | Idempotent hardware setup: clocks, GPIO, caches, power rails, RTC. Uses named constants for spin delays. |
| `board_detect_console_type` | Identifies active OFW in Bank 2 (Mario vs Zelda). |
| `board_init` | Full hardware initialization suite. |
| `board_ospi_init` | Chip-level OSPI driver initialization. |
| `board_rtc_init` | Enables backup domain and RTC clock. |
| `board_rtc_get_fattime` | Returns current time in FAT 32-bit format. |
| `board_battery_update` | Stateful battery monitor with periodic refresh. |
| `board_rtc_read_backup` / `board_rtc_write_backup` | Simplified wrappers for TAMP backup register access. |
| `board_is_valid_app` | Validation suite for jump targets (alignment, memory region, vector table). |
| `board_jump_to_app` | Executes target jump, handling hardware bank swaps via `banks.h`. |
| `board_flash_erase` | Erases Bank 2 sectors. Returns `false` on failure. |
| `board_flash_write` | Writes flashwords to internal flash. **Now returns `bool` status** for consistent error handling. |
| `board_lcd_gpios_init` | Specialized display GPIO config. (Overlap with `MX_GPIO_Init` is documented as intentional for display-centric state control). |

---

## gui.c

| Function | Purpose |
|---|---|
| `gui_spi2_init` / `gui_ltdc_init` | **Renamed.** Properly prefixed and internal to `gui.c`. |
| `gui_init` / `gui_deinit` | Full LCD lifecycle management. |
| `gui_draw_sprite` | Blits paletted 8bpp sprites with transparency and clipping. |
| `gui_draw_char_clipped` | Core 8x8 font rasteriser using `dynamic_font`. |
| `gui_draw_rect` | Draws rectangle outlines. **Now uses upfront clipping** for consistency and efficiency. |
| `gui_draw_fill_rect` | Optimized solid rectangle draw. |
| `gui_backlight_on` | **Renamed API.** Accurately reflects binary (On/Off) behavior. |
| `gui_refresh` | Flushes framebuffer via VBR-synced double-buffering. |

---

## ui/ui_manager.c

| Function | Purpose |
|---|---|
| `ui_update_theme` | Manages global theme colors based on active OFW. |
| `ui_draw_background` / `ui_draw_animations` | Themed background rendering (Zelda bar, Mario clouds/Yoshi). |
| `ui_draw_header` | Consolidated header: title (with marquee) and battery status. |
| `ui_draw_scan_progress` | Shared full-screen scanning UI. |

---

## storage/partition.c

| Function | Purpose |
|---|---|
| `sort_partitions` | Bubble-sort for partition list. **Optimized:** redundant calls removed; sorting is now surgical. |
| `find_free_space` | Identifies and labels unallocated flash regions. |
| `partition_scan_update` | Non-blocking background flash scanner state machine. |
| `update_progress_ui` | **Consolidated.** Single source of truth for full-screen operation progress (erase/flash). |
| `partition_flash_ofw` | High-level firmware flashing logic with robust error checking of `board_flash_write`. |

---

## assets.c

| Function | Purpose |
|---|---|
| `lzma_alloc` | Bump allocator. Uses `LZMA_HEAP_RESET` macro for resets. |
| `build_nes_palette` | **Shared Helper.** Deduplicated NES sub-palette construction for Mario/Zelda themes. |
| `board_load_dynamic_assets` | Runtime extraction of palettes and tilesets from internal/external flash. |

---

## assets_gen.c / assets_gen.h
**Optimized.** `cook_assets.py` now performs usage-based symbol filtering, drastically reducing binary size. `asset_list_count` is now dynamically calculated via `sizeof`.

---

## storage/lfs_wrapper.c

| Function | Purpose |
|---|---|
| `dcache_before_write` / `dcache_after_write` | **Shared Helpers.** Consolidated DCache toggle sequence to eliminate duplication in prog/erase paths. |
| `lfs_mount_at` | Dynamic LittleFS mount logic using runtime partition metadata. |

---

# Consolidated Bloat / Redundancy Register

| # | Issue | Location | Severity |
|---|---|---|---|
| *(none)* | All identified high and medium severity bloat items have been resolved. | - | Resolved |

---

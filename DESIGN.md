# gnw-chainloader Design and Architecture

This document describes the design, priorities, memory architecture, and API surfaces of **gnw-chainloader**, detailing how high-level specifications map to the actual codebase.

---

## 1. Project Priorities & Core Invariants

The architecture is built around two primary engineering priorities, which govern all other feature implementations:

### Priority 1: Inviolable Boot Path (Stability is Law)
The chainloader must always reach the launcher menu regardless of the state of external flash, bank configurations, or corrupt secondary firmware slots. To enforce this:
* **Flash Stub as Anchor of Trust:** Every byte in `stub_main.c` is uncompressed and statically linked. The logic is kept minimal to reserve space for the compressed application image.
* **Warm-Reset Return-to-Menu:** When a guest application (like Retro-Go) exits via a warm reset, it signals its intent by writing to the **RG magic cell** (`RG_MAGIC_ADDR` = `0x20000000` at the start of DTCM). 
  - `BOOT_MAGIC_RESET` (`0x1FA1AFE1`) is consumed by the stub (`stub_main.c`).
  - `BOOT_MAGIC_RETROGO` (`"CORE"` / `0x434F5245`) is consumed early in the main app (`main.c` `app_early_logic()`).
  - Both signals **must unconditionally bypass the chainloader menu and re-launch Retro-Go** by jumping directly to `RETROGO_BASE` (`0x0800A000`). This prevents loops or accidental drops into the menu during warm transitions. The only escape is holding **START/PAUSE** at boot, which overrides these checks and forces the menu.
* **OFW Integrity Gates:** Before bank-swapping into the Official Firmware (OFW) in Bank 2, the chainloader uses the hardware CRC unit to verify the integrity of the Bank 2 backup image and the external assets block. A mismatch prevents erasing or swapping, falling forward to the menu.

### Priority 2: Memory & Storage Management with Extensible Plugin Framework
The primary goal of the project is to serve as a highly capable memory and storage oversight, inspection, and management tool. Rather than locking the firmware into a rigid monolithic structure, it utilizes a highly extensible dynamic module plugin system (relocatable PIE binaries). 

Features such as SD card access, LittleFS/FAT partition writing, file browsers, audio players, and visual theme assets are implemented as dynamic, optional plugins layered on top of this extensible foundation. This keeps the core lightweight, modular, and boot-resilient.

*Note: The project is pivoted towards units with expanded memory storage configurations; standard non-modded internal flash is currently unsupported due to the space requirements of running the Retro-Go launcher alongside the diagnostic and module frameworks.*

---

## 2. Memory Partitioning Strategy

### Internal & External Flash Map
To allow side-by-side execution of the OFW and Retro-Go, the internal flash is split into two banks. Bank 1 hosts the chainloader stub and the Retro-Go launcher. Bank 2 hosts the relocated stock OFW (statically linked for `0x08000000`, brought online via a hardware bank swap).

#### Internal Flash Layout (Bank 1 & Bank 2)

| Region | Address | Size | Contents |
| :--- | :--- | :--- | :--- |
| **Chainloader Stub** | `0x08000000` | 40 KiB | Uncompressed flash stub + LZMA-compressed RAM-app payload (Bank 1) |
| **Retro-Go Launcher** | `0x0800A000` | 216 KiB | Retro-Go Launcher payload (Bank 1) |
| **OFW Internal** | `0x08100000` | 128 KiB | Relocated patched Mario/Zelda OFW (Bank 2) |
| **Free Flash** | `0x08120000` | 128 KiB | Unused space (Bank 2) |

#### External SPI Flash Partition Map
External SPI flash is memory-mapped starting at `0x90000000`. The offsets are contiguously aligned:

| Offset | SPI Address | Size | Description |
| :--- | :--- | :--- | :--- |
| `0x00000000` | `0x90000000` | 4 MiB | Zelda Edition Block (External Assets) (`DEFAULT_ZELDA_EXT_BLOCK_OFFSET`) |
| `0x00400000` | `0x90400000` | 1 MiB | Mario Edition Block (External Assets) (`DEFAULT_MARIO_EXT_BLOCK_OFFSET`) |
| `0x00500000` | `0x90500000` | 128 KiB | Zelda Patched OFW Internal Flash Backup (`ZELDA_SPI_OFFSET`) |
| `0x00520000` | `0x90520000` | 128 KiB | Mario Patched OFW Internal Flash Backup (`MARIO_SPI_OFFSET`) |
| `0x00540000` | `0x90540000` | ~4.75 MiB | FAT Module Store (`MODULE_FAT_OFFSET`) |
| `0x00A00000` | `0x90A00000` | 10 MiB | LittleFS (Themes, Configs, Language Packs) (`MODULE_LFS_OFFSET_SD`) |
| `0x01400000` | `0x91400000` | (rest of chip) | Retro-Go ROM Cache (`RETROGO_CACHE_OFFSET`) |

*Note: The Retro-Go ROM cache allocator is configured to keep its writes above `RETROGO_CACHE_OFFSET` (`0x01400000`) to protect the LittleFS, FAT store, and OFW backups from corruption.*

---

### Runtime RAM Map (SRAM)

The STM32H7B0 contains ~1.4 MB of RAM across distinct regions. Memory placement is governed by two hardware constraints:
1. **DMA Access:** Peripherals (like the LTDC display driver and SAI audio DMA) must reference AXI-SRAM (D1). They cannot reach the TCM regions.
2. **SWD Cache Coherency:** Handshake cells accessed by the SWD debugger must reside in DTCM, bypassing the L1 D-Cache to remain coherent with the host probe.

```
=============================================================================
PHYSICAL RAM BANKS (1.4 MB TOTAL)
=============================================================================

[ ITCM: 64 KB ] @ 0x00000000  ===============================================
  * Unused (Instruction Tightly-Coupled Memory)

[ DTCM: 128 KB ] @ 0x20000000 ===============================================
  0x20000000 +---------------------------------------------------+
             | DTCM Safe Zone (Retro-Go persistent state)        | 4 KB
  0x20001000 +---------------------------------------------------+
             | Stub Heap / Stack Area                            | ~123.7 KB
  0x2001FF00 +---------------------------------------------------+
             | Fastcap Handshake Cells                           | 16 B
  0x2001FFF4 +---------------------------------------------------+
             | SWD Remote Input Shadow Cell                      | 4 B
  0x2001FFF8 +---------------------------------------------------+
             | SRAM Boot Magic & Target Address Registers        | 8 B
             +---------------------------------------------------+

[ AXI-SRAM: 1024 KB ] @ 0x24000000 ===========================================
  0x24000000 +---------------------------------------------------+
             | Main Application (.text, .data, .bss)             | ~521 KB
             | Includes: Dual 320x240x2 Framebuffers (300 KB)    |
  0x24082480 +---------------------------------------------------+
             | Decompression Headroom / Free Gap                 | ~55 KB
  0x24090000 +---------------------------------------------------+
             | SRAM Module Pool (relocated PIE modules)          | 384 KB
  0x240F0000 +---------------------------------------------------+
             | Stack & Stack Guard Band                          | 64 KB
             +---------------------------------------------------+ (Top: 0x24100000)

[ D2 SRAM: 128 KB ] @ 0x30000000 ============================================
  0x30000000 +---------------------------------------------------+
             | AHB-SRAM1: Fastcap workspace, JPEGs, and tables   | 64 KB
  0x30010000 +---------------------------------------------------+
             | AHB-SRAM2: OFW patch workspace (idle in menu)     | 64 KB
             | Borrowable by transient modules (scratch_get())   |
             +---------------------------------------------------+

[ D3 SRAM: 32 KB ] @ 0x38000000 =============================================
  0x38000000 +---------------------------------------------------+
             | SRD-SRAM: Battery-backed crash log                | 32 KB
             +---------------------------------------------------+
```

---

## 3. RAM-Boot & LZMA Compression Scheme

To maximize physical flash space, the chainloader uses a **dieted RAM-boot architecture**. The uncompressed loader stub residing in flash initializes clocks/power, checks warm-reset magics, and immediately inflates the compressed main app into AXI-SRAM.

### Distinct Compression Strategies
To balance decompression speed, memory footprints, and space savings, the project utilizes two distinct LZMA compression parameters:

1. **Main Application Image:**
   * **Compression:** Raw LZMA1 pre-filtered with an ARMTHUMB BCJ pre-filter (`xz --format=raw --armthumb --lzma1=dict=128KiB,lc=1,lp=1,pb=1`).
   * **Dictionary Size:** **128 KiB** (requires a larger decompressor heap, which fits comfortably in SRAM).
   * **Decompression Props:** The stub passes `0x37` (lc=1, lp=1, pb=1) and a dict size of `0x00020000` to `LzmaDecode()`. After inflation, `armthumb_unfilter()` reverses the BCJ filter in place in AXI-SRAM.
2. **OFW Patched Assets:**
   * **Compression:** Standard LZMA without BCJ.
   * **Dictionary Size:** **16 KiB** (tuned to conserve the very limited RAM workspace available when guest OFWs are running).
   * **Decompression Props:** Decompressed by `assets.c` or the OFW patch payload (`src/patch/main.c`) using props `{0x5D, 0x00, 0x40, 0x00, 0x00}` (lc=3, lp=0, pb=2, dict size `0x00004000` = 16 KiB).
   * **Optimization:** The chainloader builds the asset decoder in **one-shot mode** (`LZMA_ONESHOT`), stripping the streaming and lookahead machinery (`LzmaDec_TryDummy`) to save ~1.3 KB of flash.

---

## 4. Bank Swapping & OFW Recovery Hooks

Because Nintendo's stock firmware binaries are statically linked for `0x08000000` and cannot execute from Bank 2 (`0x08100000`), the chainloader swaps physical banks in hardware to boot the stock firmware.

### The Swap Sequence
1. **Program Option Bytes:** The chainloader unlocks the flash option bytes, toggles the `SWAP_BANK` bit in `FLASH_OPTSR_PRG`, sets `FLASH_OPTCR_OPTSTART`, and waits for `FLASH_OPTSR_OPT_BUSY` to clear.
2. **Target Register Persistence:** The destination boot target is written to the persistent SRAM magic cells (`0x2001FFF8` and `0x2001FFFC`). These cells are located at the top of DTCMRAM, which is explicitly reserved in the stub's linker script to prevent stack corruption across resets.
3. **Reset:** The system triggers a reset via the SCB `AIRCR` register (`nvic_system_reset()`), causing Bank 2 to map to `0x08000000`.

### Injected Recovery Hooks
Once Bank 2 is active, the chainloader is dormant. To ensure the user can escape the OFW and return to the launcher, hooks are injected into the OFW binary at build time by the patch pipeline:
* **Startup Escape:** The OFW reset vector is redirected to an injected `bootloader()` function. It configures the physical **START** button pin (GPIOC Pin 11) with a pull-up. If the button is held, it invokes `ensure_unswapped_banks()`, swapping the chainloader back to Bank 1.
* **In-Game Escape:** The OFW's button reading routine (`read_buttons()`) is hooked. If **LEFT + GAME** are held simultaneously during gameplay, the hook writes `BOOT_MAGIC_FORCE` (`0x45435246`) to `SRAM_MAGIC_ADDR`, unswaps the banks, and triggers a system reset.

---

## 5. Virtual File System (VFS) & Relocatable Loader

The chainloader decouples physical storage interfaces from application code using a lightweight Virtual File System (VFS) layer and on-demand PIE (Position Independent Executable) driver modules.

### Filesystem Driver Architecture
To conserve the 40 KiB flash ceiling, the core launcher contains only read-only directory enumeration code. All write and erase capabilities are deferred to dynamic modules loaded into AXI-SRAM:

| Filesystem | Core Driver | Dynamic RW Driver (On-Device Path) | Purpose |
| :--- | :--- | :--- | :--- |
| **LittleFS** | Read-Only (via `-DLFS_READONLY`) | `/fs/lfs.bin` (built from `lfs_rw.bin`) | Manages user configs, themes, and language packs on external flash. |
| **FatFS** | Read-Only Bootstrap (LFN off, CP437) | `/fs/fat.bin` (built from `fatfs.bin`) | Manages FAT/exFAT volumes on the SD card with full LFN support. |
| **FrogFS** | Sizing/Scan only | *(None)* | Recognized in partition viewer; not browsable. |

*Note: The dynamic driver `/fs/lfs.bin` disables OSPI memory-mapped mode to perform NOR page-programs and sector-erases, then restores memory-mapped mode.*

### Relocatable PIE Loader & Execution Models
Dynamic modules (`/modules/`) are built as relocatable position-independent executables linked with a shared `module.ld` script. The loader (`system/loader.c`) supports two execution modes:

1. **Execute-in-Place (XIP) Model:**
   * **Prerequisites:** Modules must be compiled with `-msingle-pic-base -mno-pic-data-is-text-relative` (preserving `r9` as the GOT pointer).
   * **Execution:** The loader maps `.text` and `.rodata` directly to their contiguous memory-mapped external flash locations. Only the `.data`, `.got`, and `.bss` sections are copied into the SRAM module pool.
   * **Invocation:** The core calls the module entry point via a naked trampoline (`call_feat_r9`) that loads the module's RAM GOT base into `r9` and restores the core's `r9` upon return.
2. **Full RAM-Copy Model:**
   * Used for `-fPIC` modules (which rely on PC-relative GOT mappings and cannot execute from non-contiguous text/data spaces) or if XIP is explicitly disabled. The entire binary is copied into the module pool and relocated.

---

## 6. Window/List UI Engine & API Seams

The user interface is built on a clean event-driven window stack and structured lists, exposing core rendering services to loaded modules.

### The Window Manager (`ui_window_t`)
Screens are modeled as a stack of windows managed in `ui_manager.c`. The active window intercepts button inputs and drives the LTDC refresh loop:

```c
typedef struct ui_window {
    const char *title;
    int16_t x, y, w, h;
    uint8_t is_modal : 1;
    uint8_t show_footer : 1;
    uint8_t allow_idle_hide : 1;
    uint16_t header_color; // 0 = default theme header color

    void (*enter)(struct ui_window *self);
    void (*draw_content)(struct ui_window *self);
    void (*update_content)(struct ui_window *self);
    void (*exit)(struct ui_window *self);

    void *user_data;
} ui_window_t;
```

### The List Widget (`ui_list_t`)
Menus, settings, and file lists use a unified list widget. It supports scrolling, greyed-out divider rows, horizontal value-selection, and split-pane detail views:

```c
typedef struct {
    const char *title;
    int num_items;
    int selected;
    int scroll_y;
    int visible_lines;
    uint32_t selected_tick;
    uint32_t scroll_tick;

    const char* (*get_label)(int index);
    void (*on_action)(int index);
    void (*on_back)(void);
    void (*on_adjust)(int index, int dir); // LEFT/RIGHT value adjusting
    void (*on_game)(int index);           // GAME button hook
    bool (*is_enabled)(int index);        // Grey-out/skip divider predicate

    bool is_split;                        // Two-pane split mode
    void (*draw_right_pane)(int selected_idx, uint32_t selected_tick);
} ui_list_t;
```

### The GUI API Seam (`gui_api_t`)
To prevent code duplication, the core publishes its drawing primitives, layout helpers, theme colors, and translation interfaces to loadable modules via a vtable structure defined in `system/gui_api.h`:

```c
typedef struct gui_api {
    /* direction + measurement */
    bool (*is_rtl)(void);
    int  (*mirror_x)(int x, int elem_w, int box_x, int box_w);
    int  (*text_width)(const char *str);
    /* text */
    void (*draw_text)(int x, int y, const char *str, uint16_t color);
    void (*draw_text_aligned)(int x, int y, int w, const char *str, uint16_t color, bool is_active, uint32_t tick);
    void (*draw_text_marquee)(int x, int y, int max_w, const char *str, uint16_t color, bool is_active, uint32_t tick);
    void (*draw_char)(int x, int y, uint32_t cp, uint16_t color);
    void (*draw_selector)(int x, int y, uint16_t color);
    /* primitives */
    void (*draw_rect)(int x, int y, int w, int h, uint16_t color);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*blend_rect)(int x, int y, int w, int h);
    void (*draw_progress_bar)(int x, int y, int w, int h, int percent, uint16_t border, uint16_t fill);
    void (*draw_sprite)(int x, int y, int w, int h, const uint8_t *data, bool transparent, bool flip, int pitch);
    /* theme colors */
    uint16_t (*color_bg)(void);
    uint16_t (*color_fg)(void);
    uint16_t (*color_accent)(void);
    uint16_t (*color_border)(void);
    uint16_t (*color_footer)(void);
    /* frame */
    uint16_t *(*framebuffer)(void);
    void (*refresh)(void);
    /* modals */
    void (*confirm)(const char *message, void (*on_yes)(void));
    void (*error)(const char *message);
    void (*notice)(const char *message);
    void (*context_menu)(const char *title, const char **options, int count, void (*on_select)(int index));
    /* i18n */
    const char *(*tr)(int id);
    const char *(*lang_code)(void);
} gui_api_t;
```

---

## 7. Internationalization (i18n) & UTF-8 Font Streaming

The i18n system is designed to load resources dynamically, protecting the core from memory exhaustion.

### Language Discovery & Fallbacks
* **Baked Core English:** The core contains only the English string table and a basic 12px ASCII monospaced font.
* **Dynamic `.lang` Packs:** Other languages reside in self-describing `.lang` packs (`/i18n/<code>.lang`) on LittleFS, loaded on demand. If a pack or character is missing, the UI falls back gracefully to the baked English representation.
* **Strings ABI Protection:** Language packs are gated by `STRINGS_ABI_VERSION` to ensure they match the structural string ID mapping in the running firmware.

### CJK Font Paging (Glyph-on-Demand)
Chinese, Japanese, and Korean (CJK) fonts are too large to fit in RAM. 
* **Streaming Handle:** The CJK `.fnt` files are kept open on LittleFS. The font renderer binary-searches the codepoints index by seeking directly on flash, retrieving only the required 1bpp glyph bitmaps on demand.
* **Paging LRU:** Glyphs are cached in a small Least-Recently-Used (LRU) buffer in AXI-SRAM. 
* **Fallback Search:** The renderer scans three font slots in order: active script font → BASE `latin.fnt` (which also contains controller glyphs) → AUX script font by Unicode range, ensuring CJK filenames render under an English UI.

### Right-to-Left (RTL) Layout Mirroring
Arabic and Farsi mirror the entire UI layout logical coordinates.
* **Offline Shaping:** Text shaping (joining characters) and bidirectional ordering (logical-to-visual conversion) are computed **offline at cook time** (using `arabic_reshaper` and `python-bidi`). The device draws the pre-shaped visual bytes left-to-right.
* **RTL Primitives:** The layout coordinates are reflected dynamically at draw time using the `gui_mirror_x` and `gui_draw_text_aligned` (right-anchored) seams in the vtable.

---

## 8. Diagnostics, Power, & SWD-Debugging Infrastructure

The chainloader embeds instrumentation to support diagnostics and automated host-side testing.

### On-Screen Diagnostics
* **ADC1 Short Sampling:** The battery sense voltage is read using a short 1.5-cycle sampling time. This avoids integration of display ripple and provides stable, noiseless readings.
* **Diagnostics Screen:** Displays active frame rates, Bank 2 flash status, active language code, and raw boot magic cells.

### Crash Logging (D3 SRD-SRAM)
If a hardware exception occurs, the `HardFault_Handler` captures execution states (PC, SP, LR, HFSR, CFSR, MMFAR, BFAR) and writes them to the battery-backed D3 SRAM (`0x38000000`). This memory survives system resets and power-offs, allowing host-side analysis via `scripts/debug/crash_log.py`.

### SWD Remote Input Shadow Cell
To enable autonomous hardware testing, the input subsystem (`system/input.c`) exposes a 32-bit remote-input shadow cell at `0x2001FFF4` (top of DTCM):
* A workstation script (`scripts/common/remote_input.py`) writes button bitmasks directly to this address over SWD.
* The firmware ORs the shadow mask with physical button states, allowing script-driven navigation with zero hardware modifications.

### High-Speed Framebuffer Capture (Fastcap)
To capture screenshots over SWD without impacting the CPU:
* A position-independent capture codec (`src/fastcap/`) is loaded into `D2 AHB-SRAM1` (`0x3000E000` to `0x30010000`), isolating it from AXI-SRAM.
* The screen is divided into a 32×16 grid. FNV-1a hashes are computed per tile.
* Only changed tiles are hardware-JPEG compressed and transmitted, reducing SWD bandwidth.

---

## 9. Test Automation & Quality Assurance Suite

The project features a structured, multi-tier test automation suite divided between host-side simulations and on-device hardware-in-the-loop (HIL) tests. The runner, discovery mechanisms, and environmental isolation strategies are orchestrated to validate both core stability invariants and modular filesystem extensions.

### 9.1 Test Suite Orchestration & Tiers

The test suite is driven by the centralized Python orchestrator [run_suite.py](scripts/tests/run_suite.py). 

#### Discovery & Metadata
Tests are discovered dynamically. Each python test file contains a `TEST_META` dictionary (or is mapped via `LEGACY_META` in the runner for backwards compatibility) specifying:
* **Tier:** The classification layer (L0 to L4, tour, or manual).
* **Subsystem:** The target area (e.g., `boot`, `fs`, `i18n`, `modules`, `sd`, `theme`).
* **Envs:** The hardware environment requirements (e.g., `ENV-RG` for Retro-Go, `ENV-DOCS` for stock games/configs).
* **Build:** The compilation flags required on-device (e.g., `REMOTE_INPUT=1`, `ABI_SELFTEST=1`).
* **Observable:** The verification method used to assert correctness (`host`, `swd`, `fs`, `ocr`, `manual`).

#### Environmental Provisioning
Rather than running tests on static hardware, the suite leverages provision profiles under [scripts/tests/env/](scripts/tests/env/). Before running a set of tests, [run_suite.py](scripts/tests/run_suite.py) executes environment setup scripts (e.g., `env_bare.py`, `env_corrupt.py`, `env_no_extflash.py`, `env_stale_abi.py`). These scripts prepare the physical device state (erasing specific SPI partitions, writing corrupt filesystems, or flashing older ABI modules) to ensure each test runs from a pristine, well-defined hardware state, preventing side effects from leaking across tests.

---

### 9.2 Host-Side Tests (Tier L0 / L1)

Host tests run natively on the development workstation, validating layouts, parsers, and binary structures without requiring a connected physical device:

* **UTF-8 Layout Validation ([test_gui_text.c](scripts/build/test_gui_text.c)):** Exercises the core's proportional text layout, marquee calculation, RTL mirroring, and UTF-8 decoder. To run this 32-bit firmware code on 64-bit hosts, it implements a mock seam for `vfs_map_file` using the glibc `MAP_32BIT` flag under `mmap()`, ensuring returned file pointers fit within 32-bit addresses without truncation.
* **ABI Gate Verification ([test_abi_gate.c](scripts/build/test_abi_gate.c)):** Validates the dynamic linker's version verification, ensuring that the firmware core rejects modules compiled with incompatible or stale ABI definitions.
* **Settings Encoding ([test_settings_word.py](scripts/tests/host/test_settings_word.py)):** Asserts the correctness of settings serialization. It verifies that active languages, themes, and fast-boot options map precisely into the bitfields of the battery-backed backup register `TAMP->BKP3R`.
* **Font Structure Check ([test_lang_fnt_parse.py](scripts/tests/host/test_lang_fnt_parse.py)):** Parses compiled binary `.fnt` files offline to verify internal index tables, width maps, and character ranges, protecting the device's streaming font engine from out-of-bounds reads or invalid pointer dereferences.
* **Boot Table Alignment ([test_boot_magic_table.py](scripts/tests/host/test_boot_magic_table.py)):** Checks that register offsets, magic boot codes, and memory address constants are perfectly aligned between C headers and Python automation scripts.
* **Build Gates ([determinism_test.py](scripts/tests/build/determinism_test.py) & [size_ceiling_test.py](scripts/tests/build/size_ceiling_test.py)):** Verifies that the compiler output is 100% byte-reproducible and asserts that binary sizes do not exceed strict partition boundaries (e.g., the 40 KiB flash ceiling for the stub).

---

### 9.3 On-Device SWD-Driven Tests (Tier L2 / L3 / L4)

On-device tests are executed on physical hardware connected via JTAG/SWD, using closed-loop automation to drive the UI, capture states, and assert register behaviors. They integrate three main methodologies:

#### 1. SWD Remote Input Injection
Automated menu navigation is achieved through the remote input subsystem without mechanical or hardware modifications:
* **Firmware Hook:** When compiled with `REMOTE_INPUT=1` (enabled by default), the input subsystem ([system/input.c](src/chainloader/system/input.c)) and patched OFW code read a 32-bit shadow memory cell at `0x2001FFF4` (`SRAM_REMOTE_INPUT_ADDR`) at the top of DTCMRAM. This shadow cell is OR'd directly with the physical button status.
* **Low Latency & Timing:** The host-side driver [remote_input.py](scripts/common/remote_input.py) communicates with the debug probe using a single, persistent OpenOCD socket connection. This preserves button-press timing (typically ~80 ms hold, ~120 ms release) so that simulated presses behave exactly like a real tactile controller, avoiding the multi-second latencies of spawning new debug connections (which previously triggered auto-repeat and skipped rows).
* **Test Verification:** [test_remote_input.py](scripts/tests/test_remote_input.py) asserts that injecting single and repeated button inputs moves the main menu selection by exactly the expected index count, confirming that the button-poll timing matches physical user expectations.

#### 2. OCR-Based Visual Verification
Rather than reading raw RAM coordinates to infer UI state, tests verify the actual rendered pixels on the screen:
* **High-Speed Capture:** Screen frames are grabbed over SWD using [fastcap.py](scripts/debug/fastcap.py). The capture codec runs isolated in `D2 AHB-SRAM1` to avoid CPU interference, reading changed tiles and sending them over SWD.
* **Font-Aware Recognition:** The recognizer [ocr.py](scripts/common/ocr.py) parses the frame using Tesseract OCR. It restricts character sets dynamically to match the active font range to optimize speed.
* **Navigation Helpers:** [ocrnav.py](scripts/common/ocrnav.py) provides a closed-loop API: `navigate(dev, target_text)` scrolls and moves the cursor until it locates the target string visually, and `enter(dev, target_text)` navigates and presses the `A` button.
* **Validation Tests:** [ocr_nav_test.py](scripts/tests/ocr_nav_test.py) and [verify_entry_translated_in_all_languages.py](scripts/tests/verify_entry_translated_in_all_languages.py) navigate the settings pages, change languages, and verify that translations render correctly on the display without any "?" fallback glyphs.

#### 3. Memory & Register State Debugging
To verify boot sequences and hardware bank transitions, tests inspect register states directly:
* **Boot Target Inspection:** [boot_selector_test.py](scripts/tests/boot_selector_test.py) reads the file-static symbol `g_boot_target` over SWD to verify that cycling the boot selector modifies the correct internal target pointer. It then boots each target (Retro-Go, Mario, Zelda) and asserts that the program counter (`pc`) and stack pointer (`sp`) jump to the expected address ranges.
* **Warm-Reset Verification:** [retrogo_return_test.py](scripts/tests/retrogo_return_test.py) tests the critical "Return to Main Menu" path (which warm-resets from Retro-Go back to the launcher). It halts the CPU, writes `BOOT_MAGIC_RESET` or `BOOT_MAGIC_RETROGO` directly to `RG_MAGIC_ADDR` (`0x20000000`), issues a reset, and verifies that the stub intercepts the magic code and boots back into Retro-Go rather than falling through to the menu.
* **Option Byte & Bank Swapping:** Tests verify that option registers are properly unlocked and that bank swap options match the targeted boot state, preventing accidental bricking.

---

### 9.4 Test Suite Expansion Guidelines

To add new tests or expand the QA capabilities of the project, use the following patterns:

#### Adding a Host-Side Test (L0 / L1)
1. Write a standalone C test file under `scripts/build/` or a Python test script under [scripts/tests/host/](scripts/tests/host/).
2. Define a `TEST_META` dictionary with `tier="L0"` or `tier="L1"` and `observable="host"`.
3. Integrate the test into the root `Makefile` host test target (`make test-host`) and list the executable in `run_suite.py` discovery rules.

#### Adding an On-Device Test (L2 / L3)
1. Create a Python test file under the appropriate subsystem subdirectory in [scripts/tests/](scripts/tests/).
2. Define `TEST_META` specifying the required `envs` (e.g., `["ENV-DOCS"]` or `["ANY"]`), required `build` configurations, and `tier` (typically `L2` for non-destructive menu checks, or `L3` for destructive flash/reset sequences).
3. Import [ocrnav.py](scripts/common/ocrnav.py) for screen-based navigation and `common.harness` for memory assertions.
4. Execute via the suite runner: `python3 scripts/tests/run_suite.py --adaptive` (runs all tests matching the currently flashed firmware) or specify the tier.

#### Avenues for Test Suite Expansion
* **Module Loader & Relocation Testing:** Add tests to inject intentionally malformed or corrupted PIE binaries (e.g., modules missing relocations or with mismatched relocation table headers) to verify the loader handles them gracefully.
* **High-Coverage Peripheral Mocking:** Expand host-side test coverage by mocking STM32 peripherals (such as SAI audio, LTDC display clocks, and OSPI flash memory controllers) to validate rendering and sound drivers in L0/L1 environments.
* **OCR Quality Tuning:** Improve character recognition for non-Latin scripts (Cyrillic, Greek, CJK). Although the fonts contain these glyphs, the template matching currently experiences recognition errors under non-Latin scripts. Training the OCR character library on exact rendered font grids will enable full closed-loop translation validation for all supported languages.
* **Power-Down & Fault Resilience:** Expand the tests to verify state persistence across sudden power outages. This includes simulating power failures (using JTAG reset) during LittleFS file commits or flash bank swaps to guarantee the backup recovery mechanisms restore a working bootloader menu.

---

## 10. Asset Extraction & OFW Data Rebasing

The themed launcher graphics (Mario/Zelda) are extracted from stock Nintendo firmware binaries to respect copyrights and conserve storage.

### The Asset Cooking Pipeline
At build time, `scripts/build/cook_assets.py` performs **usage-based dynamic cooking**:
1. **Discovers Symbols:** It scans the entire `src/` directory (including loadable modules like the theme driver) for `ASSET_` string literals.
2. **Filters & Packs:** Only referenced assets are extracted from the theme JSON configurations and compiled into `assets_gen.c`, keeping the physical footprint minimal.

### OFW Data Rebasing
Patched OFWs store asset offsets as absolute pointer addresses linked for `0x08000000`. When reading from unswapped Bank 2 (`0x08100000`) or an SPI backup (`0x90xxxxxx`), the chainloader rebase-offsets these pointers:
* **Mario Palette Lookup:** Table resides at OFW offset `0x180A4`. The target palette is retrieved from the compressed memory blob at offset `0xCD54`.
* **Mario Tileset Pointer:** Located at OFW offset `0x7350`. The absolute pointer is remapped dynamically:
  - Active OFW: `source_fw + 0x100000`
  - SPI Backup: `source_fw + (ptr - 0x08000000)`

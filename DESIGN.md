# gnw-chainloader Design and Architecture

This document describes the goals and architecture of **gnw-chainloader** and documents how each design concept maps to the shipped implementation. Each numbered section below pairs a brief **Overview** (the idea) with an **Implementation** (how the code actually does it).

---

## Project Goals

These overarching goals are the backbone shared by three documents: the [README](README.md) states them in plain language, this file pairs each with its architecture, and [ACTIVE_WORK.md](ACTIVE_WORK.md) tracks the live tasks and debugging under each. Status reflects the current state, not the final intent.

0. **External Flash (OSPI) Chip Detection** *(completed)* — Resolved hardware probing failure by matching working bootloader initialization; correctly identifies chip size.
1. **Headless, button-driven boot** *(completed)* — Boot target instantly with display off; optimized 128KB LZMA dictionary allows complex OFW switching logic to reside in the compressed app image while keeping cold boots near-instant (§5).
2. **Dual-boot via bank swap** *(completed)* — Run the stock OFW (Bank 2) and Retro-Go (Bank 1) side-by-side via the `SWAP_BANK` option bytes. Once swapped into the OFW, operation is clean (§6).
3. **Recovery hook** *(completed)* — Always escape a booted OFW back to the launcher: **START** at boot or **LEFT + GAME** in-game forces a bank unswap (§4).
4. **Themed menu with authentic game art** *(completed)* — Render Mario & Zelda menu themes from sprites and palettes extracted from the real game ROMs (§7). Standardizes on a clean 8x8 font for all themes to ensure stability and space efficiency. 
5. **On-screen diagnostics & system info** *(completed)* — SURFACES battery state, flash-bank utilization, FPS, and boot magic cells. Includes a full Partition Viewer and File Browser for deep system exploration.
6. **Power management** *(completed)* — Unified power button handling and a 30-second inactivity timeout that hides the main menu UI while keeping themed background animations active.
7. **Read-only File Browser** *(completed)* — Dynamically browse any detected filesystem partition (LittleFS, FrogFS, FAT/exFAT) from the launcher. Supports long filenames (255 chars) and exFAT SD cards (§11).
8. **Boot resilience hardening** *(completed)* — Launcher reaches the main menu regardless of flash state. Protected Retro-Go warm resets via a 4KB DTCM Safe Zone and stub-level direct-jumps (§9).
9. **Filesystem Integration & Dynamic Theming** *(completed)* — Full read-write LittleFS for user configs and themes, lightweight FrogFS directory reader for ROM browsing, and FatFS with exFAT/RTC for SD-card file listing (§11).

---

## 1. Memory Partitioning Strategy

### Overview
To enable side-by-side coexistence of the Official Firmware (OFW) and Retro-Go, the internal flash is divided into two banks. Bank 1 hosts the chainloader and Retro-Go launcher, while Bank 2 hosts the stock official firmware. Because the OFW expects to execute at the start of flash (`0x08000000`), a hardware bank swap is used to boot it, making Bank 2 appear at `0x08000000`.

### Implementation
The memory map is hardcoded in the linker scripts and source code. This table is the **authoritative memory map** for the project; other documents reference it rather than duplicating it.

| Region         | Address      | Size      | Contents                                  |
| :------------- | :----------- | :-------- | :---------------------------------------- |
| Chainloader    | `0x08000000` | 32 KiB    | This chainloader (Bank 1)                 |
| Retro-Go       | `0x08008000` | 224 KiB   | Retro-Go Launcher payload (Bank 1)        |
| OFW Internal   | `0x08100000` | 128 KiB   | Relocated Mario/Zelda OFW (Bank 2)        |
| Free Flash     | `0x08120000` | 128 KiB   | Unused space (Bank 2)                     |
| Ext. SPI Flash | `0x90000000` | ~16–64 MB | Backups, Retro-Go filesystems (see §2)    |

Bank structure:

*   **Bank 1 (`0x08000000` – `0x0803FFFF`):** chainloader (32 KiB, configured in `STM32H7B0VBTx.ld`) followed by the Retro-Go Launcher (224 KiB).
*   **Bank 2 (`0x08100000` – `0x0813FFFF`):** relocated stock OFW (128 KiB) followed by 128 KiB of free space.

---

## 2. External Flash Mapping (SPI)

### Overview
External SPI flash (mapped to memory space starting at `0x90000000`) stores large read-only stock assets, persistent backups of the patched OFW internal flash, and the Retro-Go ROM/Save filesystems. The layout must support external flash sizes from 16 MB up to 64 MB.

### Implementation
The flash partitioning is handled via offset constants. The chainloader reads backups from these offsets to flash them to the internal bank:

| Offset       | SPI Address  | Size         | Description                                                   |
| :----------- | :----------- | :----------- | :------------------------------------------------------------ |
| `0x00000000` | `0x90000000` | 4 MiB        | Zelda Edition Block (External Assets)                         |
| `0x00400000` | `0x90400000` | 1 MiB        | Mario Edition Block (External Assets)                         |
| `0x007C0000` | `0x907C0000` | 128 KiB      | Mario Patched OFW Internal Flash Backup (`MARIO_SPI_OFFSET`)  |
| `0x007E0000` | `0x907E0000` | 128 KiB      | Zelda Patched OFW Internal Flash Backup (`ZELDA_SPI_OFFSET`)  |
| `0x00800000` | `0x90800000` | 48 MiB       | Retro-Go FrogFS (Read-only ROMs)                              |
| `0x03800000` | `0x93800000` | 8 MiB        | Retro-Go LittleFS (Writable — Saves/Configs/Themes)           |

The offsets and the two FrogFS/LittleFS sizes above reflect the 64 MB default layout; on smaller parts (down to 16 MB) the build recomputes the FrogFS/LittleFS split.

The sizes that are fixed regardless of flash capacity are: Zelda External Asset Block (4 MiB, `ZELDA_EXT_BLOCK_SIZE`), Mario External Asset Block (1 MiB, `MARIO_EXT_BLOCK_SIZE`), and the two 128 KiB OFW backups (`MARIO_SPI_OFFSET` / `ZELDA_SPI_OFFSET`, each `OFW_INTERNAL_SIZE`). FrogFS and LittleFS sizes are discovered at runtime — FrogFS reports its own size via the `bin_sz` field of its header, and LittleFS occupies the remaining space from FrogFS end to the end of extflash.

The LittleFS partition serves as the chainloader's **read-write filesystem**: it holds user-defined themes (`/themes/*.bin`), persistent configuration, and any other chainloader-owned data. Theme binaries are described in §11.

The Partition Viewer architecture supports:
- **Non-Blocking Background Scanning:** A state machine performs incremental scan steps at startup without impacting menu responsiveness.
- **Anchored Signature Detection:** Partitions are identified via unique data patterns at known internal offsets (e.g. 'CORE' for Retro-Go, 'ZELDA' NES headers).
- **Inverted Layout Support:** Robustly detects LittleFS partitions stored backward from the end of flash by calculating the start address from anchored superblocks.
- **Dynamic Footprint Scanning:** Scans internal banks in 16KB increments to determine the actual binary size of installed applications (e.g. Retro-Go).
- **Partition Management:** Supports secure deletion of non-critical partitions with UI confirmation and progress tracking.

---

## Architectural Evolution: Modular UI and System Services

To support growing feature complexity (like the File Browser) while maintaining strict binary size limits, the project is transitioning to a modular architecture. This shift decouples low-level system services from high-level UI components.

Key architectural pillars:
- **Centralized System Services:** Global input tracking (`input.c`) and shared utility libraries (`utils.c`) eliminate boilerplate and logic duplication.
- **Page-Based UI Engine:** A "cookie-cutter" framework where screens are defined as data-driven `ui_page_t` structs, managed by a central event loop.
- **Service-Oriented Storage:** Flash management and partition discovery are isolated from the UI, providing clean APIs for filesystem operations.

See [refactoring_plan.md](docs/refactoring_plan.md) for the full transition roadmap.

---

## 3. OFW Patching & Asset Relocation

### Overview
Nintendo's stock firmware binaries are statically linked for the `0x08000000` base address, so they cannot simply run from the Bank 2 partition (`0x08100000`). Two approaches exist: *relocate* the image (rewrite every absolute pointer and the vector table for the new base), or leave it linked for `0x08000000` and use a hardware **bank swap** to map Bank 2 there at boot. This project takes the bank-swap route (see §6), so the build-time patches never shift the firmware's base — they only adapt the image to coexist with the chainloader, regain control, and free internal-flash space.

### Implementation
The OFW image is patched during the build phase by `gnwmanager`'s host-side engine (`make patch`, driven by [scripts/build/patch_firmware.py](scripts/build/patch_firmware.py)). The firmware's load base is **not** shifted — it stays linked for `0x08000000` and is brought online by the bank swap (§6), so no absolute-pointer or vector relocation is performed. The patch steps are:
1.  **Decryption:** The OTFDEC-encrypted stock external flash is decrypted (`device.crypt()`) so assets can be relocated and read back later.
2.  **Asset Relocation:** Bulky compressible assets (e.g. graphics and sleep images) are LZMA-compressed and moved out to external SPI flash, freeing internal-flash space for the injected hook. The external block's base is set by `offset_size` (`0x400000` for Mario, `0` for Zelda — matching the `flash ext --offset=` targets in `Makefile.common`), **not** an internal base shift.
3.  **Hook Injection:** The recovery hook (compiled from [src/patch/main.c](src/patch/main.c)) is appended into the unused internal-flash tail past `STOCK_ROM_END`.
4.  **Reset-Vector Redirect:** The reset vector is rewritten to point at the injected `chainloader()` hook (see §4) instead of the stock reset handler.

---

## 4. OFW Recovery Hook

### Overview
When the OFW is booted, Bank 2 is swapped to `0x08000000` and the chainloader becomes dormant in Bank 2. To prevent the user from being locked into the stock firmware, a safety hook must be embedded inside the OFW image. This hook should intercept the boot sequence to swap banks back to normal if a physical button (e.g., **START**) is held, or if a specific key combination is pressed during gameplay.

### Implementation
The recovery mechanism is compiled from [src/patch/main.c](src/patch/main.c) (using the linker script [src/patch/STM32H7B0VBTx_FLASH.ld](src/patch/STM32H7B0VBTx_FLASH.ld)) and injected into the OFW binary:
*   **Startup Check:** The OFW reset handler is redirected to the `chainloader()` function. It configures the START button pin (GPIOC Pin 11) with a pull-up, reads its state, and calls `ensure_unswapped_banks()` to swap the chainloader back to Bank 1 if the button is held.
*   **In-Game Reset Combo:** The patch overrides the stock button reading routine (`read_buttons()`). If it detects **LEFT + GAME** held simultaneously, it sets the `CHAINLOADER_MAGIC_FORCE` (`0x45435246`) flag, swaps banks, and triggers a system reset to return to the launcher.

---

## 5. Boot Flow and Module Map

The chainloader uses a **Dieted RAM-boot architecture** to fit rich features into the 32 KiB internal flash limit. The boot logic is distributed between the uncompressed stub and the compressed app to maximize space:

### 1. Flash Stub (Uncompressed Loader)
A minimal loader resides at `0x08000000`. It performs bare-metal hardware setup and immediate decompressor handoff:

1.  **Stage 1 — Safe Zone & direct-jump:** Checks for Retro-Go warm resets (`RESET` magic) at `0x20000000`. If found, it jumps directly to `RETROGO_BASE` to preserve AXI-SRAM state.
2.  **Stage 2 — Standby Check:** Performs the POR Standby check to prevent auto-booting on USB connection.
3.  **Stage 3 — Inflation:** Inflates the main app from flash into AXI-SRAM using a one-shot LZMA decoder.

### 2. RAM Application (Compressed Orchestrator)
The complex boot hierarchy lives in the compressed `main.c`, which is linked for **AXI-SRAM** (`0x24000000`). This ensures that advanced logic (OFW switching, magic words) consumes minimal physical flash.

1.  **Level 1 — Physical Override (God Mode):** Checked immediately upon app start.
    *   **LEFT / RIGHT:** Identify active OFW; if mismatching the button, start OSPI and flash the requested OFW before booting.
    *   **B:** Direct Retro-Go Boot Shortcut.
    *   **START / PAUSE:** Force Launcher Menu.
2.  **Level 2 — Software Intent (Magic Words):**
    *   Handles "Quit to Menu" (`CORE` magic) from Retro-Go.
    *   Checks for protocol-aware `BOOT` magic and target address in SRAM/RTC.
3.  **Level 3 — Launcher Default:**
    *   Proceeds to GUI initialization and the themed interactive menu.

### Boot Resilience Invariants

**Stability is the non-negotiable invariant of this project.** The chainloader must reach the launcher menu under any condition — empty Bank 2, absent external flash, stale SRAM magic, mid-operation reset.

1. **The stub is the anchor of trust.** Every byte in `stub_main.c` is uncompressed; logic here is minimized to preserve space for the compressed payload.
2. **AXI-SRAM is the volatile workspace.** Decompressing into AXI-SRAM allows for massive (128KB) LZMA dictionaries, which in turn enables large features like exFAT and LFN.
3. **DTCM Safe Zone (4KB):** The first 4KB of internal DTCM RAM is reserved to protect Retro-Go save-state metadata and magic words from being overwritten during boot.
4. **Target-Aware Logic:** To prevent unintended boots after a power cycle, the launcher only honors `BOOT` magic if the target address points to a valid execution region.
5. **Failure must fall forward.** If a button-triggered or magic-triggered boot target is invalid, the orchestrator must always return to the launcher menu.

### Module Map

| File | Responsibility |
|---|---|
| `stub_main.c` | Stage 1 orchestrator; handles hardware init and the 1-2-3 priority logic. |
| [main.c](src/chainloader/main.c) | Stage 2 orchestrator; starts directly at GUI initialization. |
| [board.c](src/chainloader/board.c) | Unified hardware drivers. `board_is_valid_app()` includes region-bounds and alignment validation. |
| [assets.c](src/chainloader/assets.c) | Sprite drawing engine using direct-pointer generated symbols. |
| [gui.c](src/chainloader/gui.c) | RGB565 LTDC display driver; double-buffered in AXI SRAM. |
| [menu.c](src/chainloader/menu.c) | Interactive menu; selects boot targets and triggers resets. |
| [startup.s](src/chainloader/startup.s) | Reset handler and vector table. |
| [system_stm32h7xx.c](src/chainloader/system_stm32h7xx.c) | `SystemInit` (runs before Stage 1/2); sets the `DBGMCU` keep-alive bits (§8). |
| `deps/` | STM32 HAL drivers and CMSIS (third-party, mostly unmodified). |

---

## 6. Bank Swapping Strategy

### Overview
To run the stock firmware without patching its vector references away from `0x08000000`, the MCU's `SWAP_BANK` option bytes must be modified, swapping the physical addressing of Bank 1 and Bank 2. The chainloader must handle this swap safely, using SRAM register persistence to keep track of the final boot target across the resulting hardware system resets.

### Implementation
The swap sequence is implemented using bare-metal register routines in [src/common/banks.h](src/common/banks.h) (called during `board_jump_to_app()`):
1.  **Check Mapping State:** Compares current bank configuration using `FLASH_OPTSR_CUR`.
2.  **Unlock & Program:** If a swap or unswap is required, the chainloader unlocks option bytes, writes the `SWAP_BANK` bit in `FLASH_OPTSR_PRG`, and writes `FLASH_OPTCR_OPTSTART` to execute.
3.  **Launch Reset:** Checks option byte busy status (`FLASH_OPTSR_OPT_BUSY`), locks the option bytes, and triggers a system reset via the SCB `AIRCR` register (`nvic_system_reset()`).
4.  **Target Persistence:** The target jump address is saved in SRAM magic cells (`0x2001FFF8` and `0x2001FFFC`) which survive the system reset. This location is explicitly reserved in the stub's linker script to prevent stack corruption, allowing the chainloader to complete the boot process once the banks are swapped.

---

## 7. Deep Implementation Details

### Asset Extraction & RWData Tables

In patched OFWs (Mario/Zelda), assets like tilesets and fonts are often relocated and compressed to save space. The firmware uses an `rwdata` table mechanism to manage these blocks.

**Table Structures:** Two related entry layouts appear, depending on which side walks the table.

*Patch payload (`rwdata_inflate`, [src/patch/main.c](src/patch/main.c)) — 12-byte / 3-field entry:*
| Offset | Type | Description |
|---|---|---|
| 0x0 | int32_t | **Relative Offset** from this entry to the compressed data start. |
| 0x4 | int32_t | **Compressed Length** of the data block. |
| 0x8 | uint32_t | **Target RAM Address** the data is decompressed/copied to. |

*Chainloader Mario-palette lookup ([asset_loader.c](src/chainloader/asset_loader.c), table at OFW offset `0x180A4`) — 16-byte / 4-field entry:*
| Offset | Type | Description |
|---|---|---|
| 0x0 | uint32_t | **Inflate function pointer** (present in the stock table; skipped by the chainloader). |
| 0x4 | int32_t | **Relative Offset** from this field to the compressed data start. |
| 0x8 | int32_t | **Compressed Length** of the data block. |
| 0xC | uint32_t | **Target RAM Address** (e.g. `0x240F2124`, the Mario FreeMemory base). |

**The Rebasing Challenge:**
The OFW stores asset pointers as absolute `0x080xxxxx` addresses — correct for when it runs at `0x08000000` after the bank swap (§6). The chainloader, however, reads the OFW image from wherever it currently sits *unswapped*: the active internal copy at `0x08100000` (Bank 2), or an SPI backup at `0x90xxxxxx`. It must therefore rebase such a pointer onto the image's current location before dereferencing it ([asset_loader.c](src/chainloader/asset_loader.c) handles the Mario tileset pointer at OFW offset `0x7350`):
- Source is the active OFW at `0x08100000`: add `0x100000` (e.g. `0x0800XXXX` → `0x0810XXXX`).
- Source is an SPI backup at `0x90xxxxxx`: map to `source_fw + (ptr - 0x08000000)`.

### LZMA Decompression

The assets are compressed using LZMA (16KB dictionary). The chainloader builds the
decoder in **one-shot mode** (`LZMA_ONESHOT`), which strips the streaming and
lookahead machinery (`LzmaDec_TryDummy`) to save ~1.3 KB of flash. This mode
requires the full compressed stream to be present in mapped memory and tolerates
 a small tail over-read by the range coder. The patch payload continues to use
 the full decoder. Decompression is performed into the back-buffer scratchpad
 during boot.

## 7. Asset and Sprite Management

### Overview
The chainloader utilizes a unified, data-driven asset pipeline to manage themed UI elements (palettes and sprites) across different consoles (Mario/Zelda). To maintain a strict 32 KiB internal flash limit, the pipeline uses **usage-based dynamic cooking**, ensuring only those assets actually referenced in the source code are baked into the binary.

### Implementation

**1. The Cooking Pipeline (`scripts/build/cook_assets.py`):**
- **Symbol Discovery:** The script recursively scans the `src/chainloader/` directory for `ASSET_` string literals to determine which assets are required.
- **Dynamic Compilation:** Only discovered assets are extracted from the theme JSONs (`mario_tiles.json`, `zelda_tiles_v3.json`) and processed into the generated binary blob (`assets_gen.c`).
- **Optimization:** Mario assets are cooked as compact tile indices rather than raw pixels, matching the Zelda implementation to save space.
- **Standardized Fonts:** The pipeline does **not** extract fonts from the OFW. Instead, the firmware uses a clean, built-in 8x8 standard font for all themes, ensuring consistent character coverage and eliminating ROM-specific character mapping bugs.

**2. Drawing Engine (`assets.c` / `gui.c`):**
- **Unified Symbols:** Assets are defined as direct C symbols (`const uint8_t ASSET_NAME[]`) in the generated `assets_gen.h`, providing a clean interface.
- **`gui_draw_asset(ASSET_POINTER, x, y)`:** Handles theme-agnostic drawing, supporting diverse layout strategies (grid/quadrant) transparently.
- **Dynamic Palette loading:** Theme-specific palettes are still extracted from the stock OFW images at runtime and loaded into RAM buffers.

### Technical References

**Zelda Asset Details**
- **Clock Tileset:** 64KB 8bpp quadrant-based data at EXTFLASH `0x20000`.
- **Palette:** Zelda Clock (BGRA) at `0x2E8B24`, Link NES colors at `0x2D8160`.
- **Link Metasprites:** Extracted from Zelda II CHR-ROM Page 3 at `0xA8000`.

**Mario Asset Details**
- **Clock Tileset:** 64KB 8bpp grid-based data, LZMA-compressed in internal flash.
- **Palette:** Day Palette at internal offset `0xCD54` (inside compressed_memory blob).
- **NES Metasprites:** Extracted from SMB1 CHR-ROM at `0x408010`.

## 8. OSPI Initialization and Stabilization

### Overview
The external OSPI flash chip (MX25U51245G) is sensitive to power sequencing, noise, and residual configuration states left by previous environments (e.g., debuggers or Retro-Go). The chainloader must ensure reliable communication from both a cold power-on and a warm reset without stalling the core boot process or causing BusFaults when reading fallback targets.

### Implementation
The OSPI initialization sequence in `board.c` and `flash.c` uses a verbatim copy of the proven Retro-Go flash driver and is meticulously aligned with the hardware-specific timings used in the `game-and-watch-bootloader` example:
1.  **Early Power-Up:** The 1.8V and 3.3V power rails (PD1 and PD4) are initialized early in `board_early_init`. PD1 (active-low) is set to RESET to enable the 1.8V rail for the flash and LCD, followed by a 50ms stabilization delay.
2.  **Hardware Alignment:** System clocks and OSPI peripheral settings (including OSPIM pin mappings and a 32-bit `DeviceSize`) are matched exactly to working examples to ensure stable communication.
3.  **QPI Recovery:** The driver performs a multi-stage reset sequence (1-line SPI reset followed by a 4-line QPI reset) to force the chip into a known 1-line state before probing begins.
4.  **Mapping Stability:** `board_ospi_init` ensures that the memory-mapped mode is always enabled after initialization. On probe success, the driver correctly identifies the Macronix 64 MiB chip (`C2 25 3A`) and sets the `flash.size` property, enabling reliable address-range calculation for the partition scanner.
5.  **Dynamic Partition Scanning:** The partition scanner uses the `b"littlefs"` magic signature combined with on-flash superblock parsing (version, block size, block count) to dynamically calculate filesystem sizes, allowing the chainloader to support variable-sized Retro-Go installations.
6.  **Persistent Debugging (`DBGMCU`):** `SystemInit` explicitly sets the `DBG_SLEEPCD`, `DBG_STOPCD`, `DBG_STANDBYCD`, `DBG_STOPSRD`, and `DBG_STANDBYSRD` bits in the `DBGMCU->CR` register. This ensures OpenOCD remains connected during low-power transitions.
7.  **Power-On Reset (POR) Handling:** If the chainloader detects a Power-On Reset (`RCC_FLAG_PORRST`), it immediately enters Standby mode to prevent the device from turning on automatically when plugged into USB; it will only wake when the user presses the Power button (`WKUP1`).

---

## 9. Boot Magic Values

Magic words shared by the chainloader and the OFW patch, written to the SRAM magic cell (`0x2001FFF8`, with the jump target at `0x2001FFFC`). Defined in [src/common/boot_magic.h](src/common/boot_magic.h).

| Value | Meaning | Target (`0x2001FFFC`) |
|---|---|---|
| `0x544F4F42` | "BOOT" — software jump request | Destination address |
| `0xFEDEBEDA` | Retro-Go standby resume | Defaults to `0x08008000` |
| `0x434F5245` | "CORE" — Retro-Go quit request | — |
| `0x1FA1AFE1` | "RESET" — Retro-Go warm reset signature | — |
| `0x5254524F` | "RTRO" — Toggle Fast-Boot magic | Defaults to `0x24000000` (Retro-Go) |
| `0x00000000` | No magic / cold boot | Defaults to the menu |

**Reliability & Persistence Features:**
- **DTCM Safe Zone:** Retro-Go stores its persistent state (including the `CORE` and `RESET` signatures and save-state metadata) at the start of DTCM RAM (`0x20000000`). The chainloader's flash stub reserves the first 4KB of RAM to prevent wiping this state during the boot process.
- **Protocol Deduction:** The chainloader deducess the return-to-menu intent by checking both the explicit `CORE` request and the implicit `RESET` trace left by Retro-Go's Bank 1 exit path.
- **Stack Safety:** The boot stub's stack is linked to start below `0x2001FFF8`, ensuring that jump targets are never corrupted during the early boot phase.
- **Target-Aware Logic:** To prevent unintended boots after a power cycle (where SRAM is randomized), the stub only honors the `BOOT` magic if the target address points to a valid execution region (Internal Flash, RAM, or SPI Flash).
- **Cache Coherency:** The main application calls `SCB_CleanDCache()` before triggering a system reset to ensure magic words reach physical RAM.
- **RTC Fallback:** The `BOOT` magic is also checked in the RTC backup register (`BKP0`) for protocol compatibility, though SRAM remains the primary high-speed signaling path.
- **Fast-Boot Register (`BKP3`):** If the `BKP3` register contains the magic `"RTRO"` (`0x5254524F`), the chainloader skips the launcher menu and boots directly to Retro-Go. The user can bypass this and force entry into the launcher menu by holding the **PAUSE/SET** button (or **START**) at boot.

---

## 10. Debugging & Inspection

Device control and inspection run through the scripts in `scripts/debug/`; the conventions for using them are in GEMINI.md's Engineering Rules.

**Key debug scripts:**
- [scripts/debug/memory.py](scripts/debug/memory.py) — read/write/dump/compare/search target memory by address (`read`, `write`, `dump`, `compare`, `search` subcommands; addresses accept hex math like `0x0811AE70+0x301A5`). The workhorse for inspecting magic cells, vectors, and flash contents.
- [scripts/debug/diagnostic.py](scripts/debug/diagnostic.py) — dumps the Program Counter (PC), LTDC register states, and a VRAM framebuffer snippet.
- [scripts/debug/bank.py](scripts/debug/bank.py) — reads/sets the option-byte bank-swap state (e.g. `python scripts/debug/bank.py unswap --halt`).
- [scripts/debug/trace.py](scripts/debug/trace.py) — CPU control. `reset-halt` forces a hardware reset (and auto-unswaps banks) to recover a hung MCU / OpenOCD connection. `halt` halts the CPU **without** resetting and prints PC/SP — use this to freeze a hang and inspect it *before* the state is lost. Also `resume`, `step`, `until <addr>`, `watch <addr>`.

**Reset / run after flashing:** `gnwmanager start bank1` starts the flashed chainloader; `python scripts/debug/bank.py unswap` unswaps the banks and resets. Prefer these over `gnwmanager info`, which is a last-resort connection-debug fallback.

### Live-state debugging playbook

`memory.py` reads halt→read→**resume** (and `trace.py halt` halts without reset), so you can peek a *running or just-booted* device without disturbing its boot state. This is what makes boot-bug forensics possible: get the device into a scenario, leave it parked, and read.

**Useful non-DTCM state registers (read with `memory.py read <addr> 32`):**

| Address | Register | What it tells you |
|---|---|---|
| `0x2001FFF8` / `0x2001FFFC` | SRAM magic / target | pending boot intent (see §9); note this sits at the stack top, so it reflects stale stack at idle |
| `0x20000000` | RG magic (DTCM) | Retro-Go quit/reset trace |
| `0x580244D0` | `RCC->RSR` | reset cause: PORRSTF (bit23), SFTRSTF (bit24), PINRSTF (bit22), BORRSTF (bit21) |
| `0x58024810` | `PWR->CPUCR` | SBF standby flag (bit6), STOPF (bit5) — distinguishes warm reset vs. standby wake |
| `0x58024824` | `PWR->WKUPFR` | wakeup-pin flag |
| `0xE000ED04` | SCB ICSR | `VECTACTIVE & 0x1FF == 3` ⇒ HardFault active |

**Recipe — compare boot scenarios (e.g. "works vs. fails"):** park the device at the same UI point in each scenario, read the magic cells + `RCC->RSR` + `PWR->CPUCR`, and diff. Reads are non-destructive, so the device stays in-state. (This session: a stranded `FRCE` at `0x2001FFF8` only appeared in the failing path — the magic word was identical at idle but differed at the moment the OFW read it.)

**Recipe — locate a hang / black screen:** `trace.py halt` (no reset) to freeze it and read PC, then map PC to source with `arm-none-eabi-addr2line -f -e build/app/app.elf <PC>`. The app runs from AXI-SRAM, so its `.elf` is linked at `0x24000000` and PC maps directly. (This session: PC sat on the `__WFI` in `gui_refresh` → display used before `gui_init()`.) If a plain reset cleanly recovers to the chainloader, the banks were still unswapped, so the hang preceded any bank swap — narrowing it to pre-jump code.

**Asset tooling:** the Mario/Zelda/NES graphics reverse-engineering scripts were consolidated onto the shared `scripts/common/` library as 7 parameterized tools:

- `assets.py` — the asset pipeline: decrypt stock flash, decompress the clock-graphics blob, extract palettes, emit `mario_assets.h`, and render the Zelda menu assets from `zelda_tiles_v3.json`.
- `extract.py` — extract visual PNGs from flash or a ROM: NES CHR pages, metasprites, and G&W menu tiles.
- `render.py` — labeled tile-grid sheets, full NES CHR-page renders, and sprite-layout experiments.
- `inspect_gfx.py` — dump tiles to the terminal as ASCII art or hex, and tally colour frequencies.
- `romcheck.py` — validate a ROM/flash image (iNES header, boot magic, tile stats) and search for sprite colours or a palette in patched memory.
- `findtiles.py` — locate tiles matching a feature (brick/mortar fill, letter shapes) within a tileset.
- `capture.py` — pull the live device framebuffer over OpenOCD as a screenshot PNG or a recorded animation/video (hardware required).

**Binary inspection of the chainloader ELF:**
```bash
arm-none-eabi-nm -S build/gnw_chainloader.elf | grep "<symbol>"
arm-none-eabi-addr2line -e build/gnw_chainloader.elf 0x0800XXXX
arm-none-eabi-objdump -d build/gnw_chainloader.elf | sed -n '/<func>:/,/^$/p'
```

---

## 11. Filesystem Integration & Dynamic Theming

### Overview

The chainloader needs to read and write three classes of external-flash data:

1. **User configuration and themes** — small, writable files owned entirely by the chainloader. Stored on the Retro-Go LittleFS partition (already present on the flash).
2. **ROM archives** — the large read-only Retro-Go FrogFS image. The chainloader only needs to list files here, never write to it.
3. **SD card files** — FAT/exFAT volumes on an optional SD card expansion. Retro-Go SD already mounts these; the chainloader needs read-only directory listing for its file browser.

All three are accessed through a common file-browser UI page, driven by a filesystem-type tag obtained from the partition scanner.

### Filesystem Roles

| Filesystem | Access | Use in chainloader                                  |
| :--------- | :----- | :-------------------------------------------------- |
| LittleFS   | R/W    | Themes (`/themes/*.bin`), user config, future saves |
| FrogFS     | R/O    | ROM/cover listing for file browser                  |
| FAT/exFAT  | R/O    | SD card directory listing for file browser          |

### Implementation

#### LittleFS (Read-Write)

The existing LittleFS library under `src/chainloader/storage/littlefs/` was initially compiled with `LFS_READONLY` to keep code size down. With the architecture settled, write support is enabled by removing that flag from `lfs_config.h`. The block device callbacks `lfs_flash_prog` and `lfs_flash_erase` in `lfs_wrapper.c` implement write and erase by temporarily exiting OSPI memory-mapped mode (`OSPI_DisableMemoryMappedMode()`), calling the OSPI program/erase APIs from `flash.c`, and immediately re-entering memory-mapped mode (`OSPI_EnableMemoryMappedMode()`). Cache and page sizes are set to 256 bytes to match the OSPI page-program granularity and avoid alignment faults.

The mount address is not hardcoded. Instead, `lfs_wrapper.c` exposes `lfs_test_mount_at(uint32_t addr, uint32_t block_count)` so the application can pass the address and size discovered by the partition scanner at runtime.

#### FrogFS (Read-Only Directory Listing)

The full FrogFS library (`retro-go-sd/Core/Src/porting/lib/frogfs/`) uses dynamic allocation (`malloc`/`free`) and pulls in decompression backends (heatshrink, miniz, zlib) that are unneeded for simple directory listing. Instead a small custom reader (`src/chainloader/storage/frogfs_reader.c`, ~150 lines) is implemented that:

- Accepts a base address pointing to a memory-mapped FrogFS image.
- Validates the `FROG` magic and version field directly from the `frogfs_head_t` header.
- Iterates over the hash table and entry offsets to enumerate filenames and types without decompressing file data.
- Provides `frogfs_open_dir`, `frogfs_read_dir`, and `frogfs_close_dir` equivalents compatible with the file browser's listing loop.

This approach costs roughly 1–2 KB of flash instead of the 10+ KB the full library would add.

#### FatFS with exFAT

The FatFS R0.15 source already present in `retro-go-sd/Core/Src/porting/lib/FatFs/` (including `ff.c`, `ffunicode.c`, `ffconf.h`) is included directly in the chainloader build. ExFAT support is enabled by setting `FF_FS_EXFAT 1` in `ffconf.h`. A minimal block-device driver (`src/chainloader/storage/fatfs_diskio.c`) maps FatFS disk I/O to the memory-mapped flash (and, in the future, to an SD card SPI driver). For now only read operations (`disk_read`) are implemented; write stubs return `RES_WRPRT`.

#### Dynamic Theming Engine

Custom themes are distributed as compiled binary packages (`theme.bin`) stored in `/themes/` on the LittleFS partition. Each file begins with a `theme_header_t` descriptor:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;          // 'THME' (0x454D4854)
    uint16_t version;        // Format version (current: 1)
    uint16_t bg_color;       // RGB565 background
    uint16_t fg_color;       // RGB565 foreground
    uint16_t accent_color;   // RGB565 accent / list highlight
    uint16_t bg_style;       // 0=solid  1=Mario clouds  2=Zelda triangles
    uint16_t sprite_count;   // Sprites in the LZMA payload
    uint32_t lzma_size;      // Compressed payload length (bytes)
    uint32_t raw_size;       // Uncompressed payload length (bytes)
} theme_header_t;
```

The sprite recipe payload (tile indices, layout coordinates, animation frames) is LZMA-compressed immediately after the header. At load time, `assets.c` reads the header, allocates a fixed-size decompression scratch buffer in SRAM, and streams the LZMA payload through the existing `LzmaDec` decoder already present in the build. This means themes consume **no internal flash** — the compressed data lives entirely on external flash and is decompressed on demand.

Themes that reference stock Nintendo graphics do so via *offset descriptors* that point into the OFW external asset blocks (Zelda Block `0x90000000`, Mario Block `0x90400000`). The theme binary never embeds the Nintendo pixel data itself, keeping the repository free of proprietary content.

If the LittleFS partition is absent, unreadable, or contains no `/themes/` directory, the launcher falls back to the **Default Text Mode**: a plain background using hard-coded grey/cyan colors and the built-in 8×8 bitmap font. All functional systems (navigation, file browser, diagnostics, boot targets) remain fully operational in this fallback state.

#### Host-Side Theme Compiler

`scripts/build/cook_theme.py` reads a JSON theme definition (colors, background style, sprite recipe list, OFW asset offset references) and writes a `theme.bin` file. The sprite payload is LZMA-compressed by the script before being appended. The resulting file is copied into the LittleFS image during the build process (or can be dropped onto the filesystem manually via gnwmanager).

#### File Browser Integration

The partition viewer (`ui/partition_viewer.c`) presents an action menu when the user presses `A` on a detected filesystem partition. The `PAGE_BROWSER` page (`ui/ui_file_browser.c`) is invoked with the partition's filesystem type tag, base address, and size. The browser dispatches directory listing to the appropriate backend (LittleFS, FrogFS reader, or FatFS) and renders the results with the standard `ui_list` widget, supporting scroll, back navigation, and eventual file-action hooks.

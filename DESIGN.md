# gnw-chainloader Design and Architecture

This document describes the goals and architecture of **gnw-chainloader** and documents how each design concept maps to the shipped implementation. Each numbered section below pairs a brief **Overview** (the idea) with an **Implementation** (how the code actually does it).

---

## Project Goals

These overarching goals are the backbone shared by three documents: the [README](README.md) states them in plain language, this file pairs each with its architecture, and [ACTIVE_WORK.md](ACTIVE_WORK.md) tracks the live tasks and debugging under each. Status reflects the current state, not the final intent.

0. **External Flash (OSPI) Chip Detection** *(completed)* — Resolved hardware probing failure by matching working bootloader initialization; correctly identifies chip size.
1. **Headless, button-driven boot** *(completed)* — Boot target instantly with display off; optimized 128KB LZMA dictionary allows complex OFW switching logic to reside in the compressed app image while keeping cold boots near-instant (§5).
2. **Dual-boot via bank swap** *(completed)* — Run the stock OFW (Bank 2) and Retro-Go (Bank 1) side-by-side via the `SWAP_BANK` option bytes. Once swapped into the OFW, operation is clean (§6).
3. **Recovery hook** *(completed)* — Always escape a booted OFW back to the launcher: **START** at boot or **LEFT + GAME** in-game forces a bank unswap (§4).
4. **Themed menu with authentic game art** *(completed)* — Render Mario & Zelda menu themes from sprites and palettes extracted from the real game ROMs (§7). Uses one shared mixed-case Fusion Pixel 12px monospaced font across all themes (see §12) for stability and space efficiency. 
5. **On-screen diagnostics & system info** *(completed)* — SURFACES battery state, flash-bank utilization, FPS, and boot magic cells. Includes a full Partition Viewer and File Browser for deep system exploration.
6. **Power management** *(completed)* — Unified power button handling and a 30-second inactivity timeout that hides the main menu UI while keeping themed background animations active.
7. **Read-only File Browser** *(completed)* — Dynamically browse any detected filesystem partition (LittleFS, FAT/exFAT) from the launcher; FrogFS partitions are recognized + sized but not browsable. Supports long filenames (255 chars) and exFAT SD cards (§11).
8. **Boot resilience hardening** *(completed)* — Launcher reaches the main menu regardless of flash state. Protected Retro-Go warm resets via a 4KB DTCM Safe Zone and stub-level direct-jumps (§9).
9. **Filesystem Integration & Dynamic Theming** *(completed)* — Full read-write LittleFS for user configs and themes, FrogFS partition recognition (size only, not browsable), and FatFS with exFAT/RTC for SD-card file listing (§11).
10. **Internationalized UI** *(completed)* — Every menu string is translatable, including non-Latin scripts. A mixed-case Fusion Pixel 12px monospaced font replaces the uppercase-only 8px font; translations (`.lang`) and per-script glyph fonts (`.fnt`) live on the device filesystem and are selected live under Settings, with English/ASCII as the always-present fallback (§12, [docs/i18n.md](docs/i18n.md)).

---

## 1. Memory Partitioning Strategy

### Overview
To enable side-by-side coexistence of the Official Firmware (OFW) and Retro-Go, the internal flash is divided into two banks. Bank 1 hosts the chainloader and Retro-Go launcher, while Bank 2 hosts the stock official firmware. Because the OFW expects to execute at the start of flash (`0x08000000`), a hardware bank swap is used to boot it, making Bank 2 appear at `0x08000000`.

### Implementation
The memory map is hardcoded in the linker scripts and source code. This table is the **authoritative memory map** for the project; other documents reference it rather than duplicating it.

| Region         | Address      | Size      | Contents                                  |
| :------------- | :----------- | :-------- | :---------------------------------------- |
| Chainloader    | `0x08000000` | 40 KiB    | This chainloader (Bank 1)                 |
| Retro-Go       | `0x0800A000` | 216 KiB   | Retro-Go Launcher payload (Bank 1)        |
| OFW Internal   | `0x08100000` | 128 KiB   | Relocated Mario/Zelda OFW (Bank 2)        |
| Free Flash     | `0x08120000` | 128 KiB   | Unused space (Bank 2)                     |
| Ext. SPI Flash | `0x90000000` | ~16–64 MB | Backups, Retro-Go filesystems (see §2)    |

The chainloader reservation was raised from 32 KiB to 40 KiB (`STM32H7B0_FLASH_STUB.ld`, `LENGTH = 40K`), so `RETROGO_BASE` moved to `0x0800A000` (`src/common/memory_map.h`) and the Retro-Go payload now spans 216 KiB (`RG_INTFLASH_ADDRESS` / `RG_FLASH_LENGTH` in `Makefile.common` match). The similarly named `STM32H7B0VBTx.ld` is the OFW patch linker script (§3), not the chainloader's.

The 40 KiB holds the uncompressed stub plus the LZMA-compressed RAM-app blob; the Makefile `Free space` line is the headroom metric. The app is compressed with an ARMTHUMB BCJ pre-filter then raw LZMA1 (`lc1/lp1/pb1`, 128 KiB dict), and `stub_main.c` reverses both at boot (`LzmaDecode` with props byte `0x37`, then `armthumb_unfilter()` in place). The encoder (Makefile xz call) and decoder (stub props + inverse BCJ) MUST stay in lock-step or the app will not decompress and the device will not boot. See [docs/size-optimization.md](docs/size-optimization.md) for the full pipeline, the LZMA dynamics, and the size-tuning methodology.

Bank structure:

*   **Bank 1 (`0x08000000` – `0x0803FFFF`):** chainloader (40 KiB, configured in `STM32H7B0_FLASH_STUB.ld`) followed by the Retro-Go Launcher (216 KiB).
*   **Bank 2 (`0x08100000` – `0x0813FFFF`):** relocated stock OFW (128 KiB) followed by 128 KiB of free space.

### Runtime RAM Map (SRAM)

The MCU has ~1.4 MB of RAM across five banks. **Two hard rules decide where a buffer
*must* go; everything else is free placement, arbitrated by lifetime** (buffers whose
lifetimes never overlap may share an address range — that is the lever for fitting more
into less).

1. **A peripheral's DMA reads it → it must live in AXI-SRAM (D1).** The LTDC scans the
   framebuffers out to the LCD over its own bus master, which reaches AXI, not the TCMs.
   So `fb_a`/`fb_b` are non-negotiably AXI.
2. **The SWD debugger must see it without a cache flush → DTCM.** DTCM bypasses the L1
   D-cache, so the fastcap / remote-input / boot-magic handshake words sit at the top of
   DTCM (`0x2001FFxx`) and stay coherent with the probe (see §9, §10.1, and the fastcap
   cells in `memory_map.h`).

| Bank | Base | Size | Access / use |
| :--- | :--- | :--- | :--- |
| ITCM | `0x00000000` | 64 KB | CPU instruction fetch; unused |
| DTCM | `0x20000000` | 128 KB | CPU-only, **uncached** → app stack + SWD handshake cells |
| AXI-SRAM (D1) | `0x24000000` | 1024 KB | CPU + DMA, cached → app, framebuffers, heaps, module pool |
| D2 AHB-SRAM1 | `0x30000000` | 64 KB | CPU + D2 peripherals; **free** (its clock is off by default) |
| D2 AHB-SRAM2 | `0x30010000` | 64 KB | used by the OFW *patch* (`PATCH_LDFLAGS`), idle while the menu runs |
| D3 SRD-SRAM | `0x38000000` | 32 KB | **battery-backed** (VBAT); persists across reset/power → **crash log** |

**AXI-SRAM (`0x24000000`–`0x24100000`) — three non-overlapping spans:**

```
0x24000000  app: code + data, then .bss ................ ~521 KB USED
              (the two 320x240x2 framebuffers alone are 300 KB of this)
0x24082480  ── free gap ──.............................  ~55 KB  app/lzma headroom
0x24090000  MODULE POOL  (bump-up from base; UI stack     384 KB  usable (−64 KB guard);
              descends from _estack)                        residents ~91 KB (language ~52,
                                                            fatfs ~20, lfs_rw ~17, theme ~2),
0x24100000  _estack (top)                                   ~293 KB for one live feature
```

**D2 AHB-SRAM1 (`0x30000000`–`0x30010000`, 64 KB) — fastcap owns the whole bank when live:**

```
0x30000000  tile-hash table  150 x u32 FNV-1a (per-tile change detect) ..  1 KB reserve
0x30000400  YCC scratch      one 32x16 tile = 2 MCU, RGB565->YCbCr 4:2:0 .. ~8 KB reserve
0x30002400  payload buffer   frame header + packed tile JPEGs ...........  47 KB cap
0x3000E000  fastcap code     .text/.data/.bss (binary ~3.7 KB) .........   8 KB
0x30010000  end of bank
```

Key facts that make this safe to reason about:

- The **module pool is a bump allocator** (`system/loader.c`, `MODULE_POOL_BASE`): it
  *reserves* the address range but only *consumes* what is loaded. Its size is driven by
  module **code** size, never by filesystem content — the file browser uses fixed app
  buffers (`fb_name_pool[32 KB]`, `file_entries[512]` in `ui_file_browser.c`), so a huge
  directory costs zero pool.
- **fastcap** (the live framebuffer-capture RAM app, §10) runs only while the menu is
  live, so it must not touch the framebuffers, the resident theme module, or the pool's
  growth path. It tiles the screen into 32×16 tiles, detects change with a per-tile
  FNV-1a hash (no full-frame buffers), and hardware-JPEG-encodes only changed tiles at a
  host-selectable quality (the `FASTCAP_QUALITY` cell, default 98; read at reinit). The
  whole codec is placed in **D2 AHB-SRAM1 (`0x30000000`)** — isolated
  from AXI by construction, so it can never re-collide. *(An earlier layout hardcoded
  fastcap into `0x240A0000`+ and overwrote loaded modules → bus fault; that is the
  cautionary tale this map exists to prevent.)*
- **Module pool is 384 KB usable** (`MODULE_POOL_BASE = 0x24090000`; lowered from `0x240C0000`
  (192 KB) into the free AXI gap below it so a feature module like the ~104 KB picture viewer
  loads past the residents — that overflow, "Out of module memory", is what drove the change).
  Modules are PIE relocated to the pool base, so it stays a one-constant change. With the resident
  modules (~91 KB: language ~52 / fatfs ~20 / lfs_rw ~17 / theme ~2) and a 64 KB stack guard,
  ~293 KB remains for the one live transient feature module. The 1 MB AXI-SRAM still has plenty
  of room below; growing the pool further is the same one-constant change. The full tiered
  module-memory design (this pool, the borrowed D2 scratch, and the planned OSPI XIP store) is in
  [docs/memory-architecture.md](docs/memory-architecture.md).
- **Crash log in D3 SRAM** (`0x38000000`, otherwise unused): the chainloader's `HardFault`
  handler (`system/crash_log.c`) records the fault-status registers (HFSR/CFSR/MMFAR/BFAR) and
  the stacked exception frame, then halts — so a fault can be diagnosed over SWD after the fact
  with `scripts/debug/crash_log.py` (decodes the cause; `addr2line` on the captured PC pins the
  source line). All configurable faults escalate to HardFault, so one handler catches them all.
  Only ever runs on a fault, so the boot path is untouched.
- Any new large buffer must be placed against this map — ideally derived from `_ebss` /
  `MODULE_POOL_BASE` so an overlap fails at **build** time, not as a runtime bus fault.

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

**SD layout: bounded LittleFS + the Retro-Go ROM cache.** `src/common/memory_map.h` is the single source of truth for these offsets. The SD-variant external flash is laid out, bottom to top:

| Offset | Region |
| :--- | :--- |
| `0x000000` | Zelda / Mario asset blocks |
| `0x500000` / `0x520000` | Zelda / Mario OFW backups (128 KiB each) |
| `0x540000` | FAT module store (`MODULE_FAT_OFFSET`) |
| `0xA00000` | **LittleFS** — `MODULE_LFS_SIZE` (10 MiB default, set by `Makefile.common` `LFS_SIZE`), butted against the FAT store (`MODULE_LFS_OFFSET_SD`) |
| `0x1400000` | **Retro-Go ROM cache** — raw ROM/save circular cache, fills to end of chip (`RETROGO_CACHE_OFFSET`) |

In the SD-card Retro-Go build the *firmware itself* never mounts the external-flash LittleFS (saves/config/screenshots go to the SD card); the chainloader owns it (themes, `/i18n` language packs + fonts, feature modules, the RW FS drivers). Launching a game copies the ROM from the SD card into the raw ROM-cache region via a round-robin allocator (`retro-go-sd/Core/Src/gw_flash_alloc.c`).

The cache must never write below `RETROGO_CACHE_OFFSET`, or it would erase the LittleFS, the OFW backups, and the FAT store (none of which Retro-Go knows about). The chainloader passes `RETROGO_CACHE_OFFSET` to the Retro-Go build as `EXTFLASH_OFFSET`; the allocator keeps its writes above the **larger** of that (exposed to C as `__EXTFLASH_OFFSET__`) and `get_ofw_extflash_size()` (the active OFW's own footprint). The LittleFS sits *inside* this reservation, so the cache needs only a lower bound — no upper bound (the upstreamable Retro-Go change is simply "respect `EXTFLASH_OFFSET`"). The LittleFS is stored inverted (superblock at the top of its region); the Partition Viewer validates it at the fixed `0xA00000..0x1400000` span, registers a labeled `Retro-Go Cache` region above it, and skips the cache during the sweep (raw ROM bytes can false-match FS magics). Host LFS tools reach the relocated LittleFS via `lfs_gnwmanager_offset()` (`scripts/common/__init__.py`), which converts `RETROGO_CACHE_OFFSET` to the end-anchored offset gnwmanager expects.

The Partition Viewer architecture supports:
- **Non-Blocking Background Scanning:** A state machine performs incremental scan steps at startup without impacting menu responsiveness.
- **Anchored Signature Detection:** Partitions are identified via unique data patterns at known internal offsets (e.g. 'CORE' for Retro-Go, 'ZELDA' NES headers).
- **Inverted Layout Support:** Robustly detects LittleFS partitions stored backward from the end of flash by calculating the start address from anchored superblocks.
- **Dynamic Footprint Scanning:** Scans internal banks in 16KB increments to determine the actual binary size of installed applications (e.g. Retro-Go).
- **Partition Management:** Supports secure deletion of non-critical partitions with UI confirmation and progress tracking.

---

## Architectural Evolution: Modular UI and System Services

To support growing feature complexity (like the File Browser) while keeping the 40 KiB core small, the project uses a modular architecture that decouples low-level system services from high-level UI components and pushes optional features into loadable PIE modules (`src/modules/`).

Key architectural pillars:
- **Centralized System Services:** Global input tracking (`input.c`) and shared utility libraries (`utils.c`) eliminate boilerplate and logic duplication.
- **Window/List UI Engine:** Screens are a stack of `ui_window_t` (`ui/ui.h`) drawn by a central event loop, with a shared `ui_list_t` widget (`ui/ui_list.h`) that provides split-pane lists, scrolling, value-selectors, and grey-out predicates.
- **Service-Oriented Storage:** Flash management and partition discovery are isolated from the UI behind a VFS (`storage/vfs.c`), providing clean APIs for filesystem operations.
- **Loadable PIE Modules:** Optional capability (filesystem drivers, theme sprites, language packs) ships as relocatable modules loaded on demand into a RAM pool, each with an in-core fallback so the menu is always reachable.

The PIE module loader and the loadable drivers are documented in §5 (module map), §11, and §12, and in [docs/pie-module-loader.md](docs/pie-module-loader.md).

---

## 3. OFW Patching & Asset Relocation

### Overview
Nintendo's stock firmware binaries are statically linked for the `0x08000000` base address, so they cannot simply run from the Bank 2 partition (`0x08100000`). Two approaches exist: *relocate* the image (rewrite every absolute pointer and the vector table for the new base), or leave it linked for `0x08000000` and use a hardware **bank swap** to map Bank 2 there at boot. This project takes the bank-swap route (see §6), so the build-time patches never shift the firmware's base — they only adapt the image to coexist with the chainloader, regain control, and free internal-flash space.

### Implementation
The OFW image is patched during the build phase by `gnwmanager`'s host-side engine (`make patch`, driven by [scripts/build/patch_firmware.py](scripts/build/patch_firmware.py)). The firmware's load base is **not** shifted — it stays linked for `0x08000000` and is brought online by the bank swap (§6), so no absolute-pointer or vector relocation is performed. The patch steps are:
1.  **Decryption:** The OTFDEC-encrypted stock external flash is decrypted (`device.crypt()`) so assets can be relocated and read back later.
2.  **Asset Relocation:** Bulky compressible assets (e.g. graphics and sleep images) are LZMA-compressed and moved out to external SPI flash, freeing internal-flash space for the injected hook. The external block's base is set by `offset_size` (`0x400000` for Mario, `0` for Zelda — matching the `flash ext --offset=` targets in `Makefile.common`), **not** an internal base shift.
3.  **Hook Injection:** The recovery hook (compiled from [src/patch/main.c](src/patch/main.c)) is appended into the unused internal-flash tail past `STOCK_ROM_END`.
4.  **Reset-Vector Redirect:** The reset vector is rewritten to point at the injected `bootloader()` hook (see §4) instead of the stock reset handler. (The entry symbol is named `bootloader` so the patch builds against unmodified `gnwmanager`, whose script does `replace(0x4, "bootloader")`; the resulting reset-vector value is unchanged — `0x08018101` Mario, `0x0801B3E1` Zelda.)
5.  **Warm-boot power-on patches:** Two byte-patches per firmware make the stock OFW boot normally after a *warm* reset (our bank swap into the OFW), not only a power-button power-on — see [docs/ofw-poweroff-on-warm-switch.md](docs/ofw-poweroff-on-warm-switch.md). They NOP the OFW's `PWR_CPUCR.SBF` boot-mode gate (so display init always runs) and turn the in-loop "state-6" standby branch into an unconditional skip. Without them, a warm switch left the screen dark until a manual power-button press.

### OFW Integrity Verification (CRC gate)

The chainloader copies an OFW backup from external flash into Bank 2 and boots it. A backup is *recognized* only by its reset vector (`0x08018101` Mario / `0x0801B3E1` Zelda), with no integrity check — so a wiped, corrupt, half-flashed, or merely *unpatched* image (stock, or built by a different/older patch) is indistinguishable from a good one and would bank-swap into a console that cannot boot or return to the launcher. To close that gap the patch pipeline bakes a CRC-32 signature of each image, and the chainloader re-checks it before every copy.

**Baked signatures.** [scripts/build/gen_ofw_crc.py](scripts/build/gen_ofw_crc.py) (`make ofw-crc`, after `make patch`) computes, from the deterministic patch outputs, a per-console record holding two CRCs and writes them to the committed header `src/common/ofw_crc.h`:
- **internal_crc** — the full 128 KiB backup image (`patched_internal_<game>.bin`), the bytes copied into Bank 2.
- **asset_crc** — the *static* window of the external asset blob (`patched_external_<game>.bin`), i.e. the immutable ROM/code/asset region with the runtime-mutable **save regions excluded** (otherwise playing a game would change the CRC). The windows are: Mario `[0, blob_len − 0x2000)` (the 8 KiB NVRAM is always the trailing slice of the shortened blob — see `mario.py`); Zelda `[0x20000, 0x3254A0)` (the stock encrypted span; saves live before `0x20000` and after `0x3E0000`, and Zelda is never shortened).

The two CRCs together form a **pairing**: a boot is allowed only when both the backup image and the asset blob it will reach for match the *same* baked record, so a mismatched internal/asset combination is also rejected. The values are device-invariant (stock ROMs are SHA1-pinned, saves excluded), so this is a once-per-patch commit, not part of the normal build.

**CRC convention.** The STM32H7 hardware CRC unit in its reset-default configuration (poly `0x04C11DB7`, init `0xFFFFFFFF`, 32-bit, no input/output reflection — CRC-32/MPEG-2), fed one little-endian 32-bit word at a time. `gen_ofw_crc.py` models that operation exactly, so host and device agree by construction (the specific polynomial is irrelevant — it is a shared integrity token).

**Device gate.** [src/chainloader/system/ofw_verify.c](src/chainloader/system/ofw_verify.c) drives the hardware CRC unit (`ofw_crc32`) over the mapped external-flash regions. `ofw_verify_by_spi()` is called at the top of `partition_flash_ofw()` *before* erasing Bank 2; on any mismatch (or a region past the detected flash, or an unknown offset) it refuses, leaves Bank 2 untouched, and the caller stays in the menu — both the menu A-press (`menu.c`) and the boot-time god-mode LEFT/RIGHT override (`main.c`) honor the refusal, falling through to the launcher rather than jumping to an unbootable Bank 2 (STABILITY IS LAW). `ofw_verify_addr()` lets the Partition Viewer mark a recognized-but-mismatched OFW/asset region as `UNKNOWN` (computed once per selection — the CRC sweep is too heavy for a per-frame draw). Retro-Go is not covered by this gate.

---

## 4. OFW Recovery Hook

### Overview
When the OFW is booted, Bank 2 is swapped to `0x08000000` and the chainloader becomes dormant in Bank 2. To prevent the user from being locked into the stock firmware, a safety hook must be embedded inside the OFW image. This hook should intercept the boot sequence to swap banks back to normal if a physical button (e.g., **START**) is held, or if a specific key combination is pressed during gameplay.

### Implementation
The recovery mechanism is compiled from [src/patch/main.c](src/patch/main.c) (using the linker script [src/patch/STM32H7B0VBTx_FLASH.ld](src/patch/STM32H7B0VBTx_FLASH.ld)) and injected into the OFW binary:
*   **Startup Check:** The OFW reset handler is redirected to the `bootloader()` function (the entry symbol our patch exports; see §3 step 4). It configures the START button pin (GPIOC Pin 11) with a pull-up, reads its state, and calls `ensure_unswapped_banks()` to swap the chainloader back to Bank 1 if the button is held.
*   **In-Game Reset Combo:** The patch overrides the stock button reading routine (`read_buttons()`). If it detects **LEFT + GAME** held simultaneously, it sets the `CHAINLOADER_MAGIC_FORCE` (`0x45435246`) flag, swaps banks, and triggers a system reset to return to the launcher.

---

## 5. Boot Flow and Module Map

The chainloader uses a **Dieted RAM-boot architecture** to fit rich features into the 40 KiB internal flash limit. The boot logic is distributed between the uncompressed stub and the compressed app to maximize space:

### 1. Flash Stub (Uncompressed Loader)
A minimal loader resides at `0x08000000`. It performs bare-metal hardware setup and immediate decompressor handoff:

1.  **Stage 1 — Safe Zone & direct-jump:** Checks the RG magic cell (`0x20000000`) for Retro-Go's warm-reset trace (`RESET`). If found, it jumps directly to `RETROGO_BASE` (always the define, never a literal — a stale `0x08008000` here once silently dropped to the menu) to **re-launch Retro-Go**, preserving its AXI-SRAM state. This is the "Return to Main Menu" path for a bank-1 Retro-Go, which bare-resets with only the `RESET` trace set.
2.  **Stage 2 — Standby Check:** Performs the POR Standby check to prevent auto-booting on USB connection.
3.  **Stage 3 — Inflation:** Inflates the main app from flash into AXI-SRAM using a one-shot LZMA decoder.

### 2. RAM Application (Compressed Orchestrator)
The complex boot hierarchy lives in the compressed `main.c`, which is linked for **AXI-SRAM** (`0x24000000`). This ensures that advanced logic (OFW switching, magic words) consumes minimal physical flash.

1.  **Level 1 — Physical Override (God Mode):** Checked immediately upon app start.
    *   **LEFT / RIGHT:** Identify active OFW; if mismatching the button, start OSPI and flash the requested OFW before booting.
    *   **B:** Direct Retro-Go Boot Shortcut.
    *   **START / PAUSE:** Force Launcher Menu.
2.  **Level 2 — Software Intent (Magic Words):**
    *   Handles Retro-Go's explicit "Return to Main Menu" marker (`CORE`) by **re-launching Retro-Go** (jump `RETROGO_BASE`); the `RESET` trace is caught earlier by the stub (Stage 1).
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
| `stub_main.c` | Uncompressed loader: Retro-Go `RESET` direct-jump, POR standby check, then LZMA inflation of the app into AXI-SRAM (the stub stages above). |
| [main.c](src/chainloader/main.c) | RAM-app orchestrator: runs the button god-mode + magic-word boot priority (`app_early_logic`, the app levels above), then GUI init and `menu_run`. |
| [board.c](src/chainloader/board.c) | Unified hardware drivers. `board_is_valid_app()` includes region-bounds and alignment validation. |
| [assets.c](src/chainloader/assets.c) | Sprite drawing engine using direct-pointer generated symbols. |
| [gui.c](src/chainloader/gui.c) | RGB565 LTDC display driver; double-buffered in AXI SRAM. |
| [menu.c](src/chainloader/menu.c) | Interactive menu. Its top item is the unified boot selector (`boot_target_t`): LEFT/RIGHT cycle the bootable targets (Retro-Go in Bank 1, plus whichever OFW backups exist), A boots the shown one (flashing that OFW into Bank 2 first if it is not already there). Renders as a translated `LAUNCH: < target >` value selector, matching the THEME/LANGUAGE rows. |
| [startup.s](src/chainloader/startup.s) | Reset handler and vector table. |
| [system_stm32h7xx.c](src/chainloader/system_stm32h7xx.c) | `SystemInit` (runs before Stage 1/2); sets the `DBGMCU` keep-alive bits (§8). |
| `deps/` | STM32 HAL drivers and CMSIS (third-party, mostly unmodified). |

### ABI Gating

Every dynamically loaded artifact is honored only when BOTH its magic and its ABI version match the running firmware. A mismatch in either direction means it was built for a different firmware and is rejected, falling back to safe defaults (a module is treated as absent; a language pack falls back to English), so a stale artifact left in the filesystem by a prior firmware cannot load and render garbage. Two independent contracts:

- **Module-framework ABI** (`MODULE_ABI_VERSION`, `system/module.h`): gates every PIE module. `module_header_t` carries an `abi` field (plus a `flags` load-model field, below); `mod_load_image` (`loader.c`) and the multi-filesystem `vfs_read_module` (`vfs.c`) both reject a module whose `abi` mismatches. Bump it whenever the module/host-API contract changes.
- **Strings ABI** (`STRINGS_ABI_VERSION`, `ui/strings.h`): gates `.lang` packs. The pack header carries its ABI, and the core's vfs lang functions enforce the **core's own** value (not one a module passes in), so a stale module and stale packs can no longer agree and load together.

Both gates are factored into one header, `src/common/abi.h` (`module_abi_ok` / `pack_abi_ok`), called by the firmware gates **and** a host unit test (`scripts/build/test_abi_gate.c`, `make test_host`), so the test exercises the real check rather than a copy. An on-device test (`scripts/tests/test_abi_reject.py`, 6/6 on hardware) proves both gates reject a wrong-ABI dummy module and a mutated-ABI pack.

**Load-model flag.** `module_header_t.flags` bit `MOD_FLAG_TRANSIENT` declares whether the core keeps a module **resident** in the RAM pool (it registered permanent callbacks, e.g. theme or language) or treats it as **transient** (load on demand, run, then reclaim its slot with `mod_pool_mark` / `mod_pool_reset`). Gating is recovery-critical and stays in-core. The **installer** (`/modules/installer.bin`) is the transient example: the core loads it on demand to install SD artifacts, then frees its slot. Its **install gate mirrors the loader's LOAD gate** (magic plus the running core's ABI for that artifact class, checked with the core's own ABI values), so it never commits an artifact the firmware could not then load.

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

*Chainloader Mario-palette lookup ([assets.c](src/chainloader/assets.c), table at OFW offset `0x180A4`) — 16-byte / 4-field entry:*
| Offset | Type | Description |
|---|---|---|
| 0x0 | uint32_t | **Inflate function pointer** (present in the stock table; skipped by the chainloader). |
| 0x4 | int32_t | **Relative Offset** from this field to the compressed data start. |
| 0x8 | int32_t | **Compressed Length** of the data block. |
| 0xC | uint32_t | **Target RAM Address** (e.g. `0x240F2124`, the Mario FreeMemory base). |

**The Rebasing Challenge:**
The OFW stores asset pointers as absolute `0x080xxxxx` addresses — correct for when it runs at `0x08000000` after the bank swap (§6). The chainloader, however, reads the OFW image from wherever it currently sits *unswapped*: the active internal copy at `0x08100000` (Bank 2), or an SPI backup at `0x90xxxxxx`. It must therefore rebase such a pointer onto the image's current location before dereferencing it ([assets.c](src/chainloader/assets.c) handles the Mario tileset pointer at OFW offset `0x7350`):
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

### Asset and Sprite Management

#### Overview
The chainloader utilizes a unified, data-driven asset pipeline to manage themed UI elements (palettes and sprites) across different consoles (Mario/Zelda). To maintain a strict 40 KiB internal flash limit, the pipeline uses **usage-based dynamic cooking**, ensuring only those assets actually referenced in the source code are baked into the binary.

#### Implementation

**1. The Cooking Pipeline (`scripts/build/cook_assets.py`):**
- **Symbol Discovery:** The script recursively scans the whole `src/` tree for `ASSET_` string literals to determine which assets are required. It scans all of `src/` (not just `src/chainloader/`) so that asset references in loadable modules compiled outside the core — notably the theme driver in `src/modules/theme/` — are seen; otherwise their sprites would be filtered out as "unused" and the module would fail to link.
- **Dynamic Compilation:** Only discovered assets are extracted from the theme JSONs (`mario_tiles.json`, `zelda_tiles_v3.json`) and processed into the generated binary blob (`assets_gen.c`).
- **Optimization:** Mario assets are cooked as compact tile indices rather than raw pixels, matching the Zelda implementation to save space.
- **Standardized Fonts:** The pipeline does **not** extract fonts from the OFW. Instead, the firmware uses a clean, built-in Fusion Pixel 12px monospaced font for all themes (see §12), ensuring consistent character coverage and eliminating ROM-specific character mapping bugs.

**2. Drawing Engine (`assets.c` / `gui.c`):**
- **Unified Symbols:** Assets are defined as direct C symbols (`const uint8_t ASSET_NAME[]`) in the generated `assets_gen.h`, providing a clean interface.
- **`gui_draw_asset(ASSET_POINTER, x, y)`:** Handles theme-agnostic drawing, supporting diverse layout strategies (grid/quadrant) transparently.
- **Dynamic Palette loading:** Theme-specific palettes are still extracted from the stock OFW images at runtime and loaded into RAM buffers.

#### Technical References

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

Magic words shared by the chainloader and the OFW patch. Most live in the SRAM magic cell (`0x2001FFF8`, jump target at `0x2001FFFC`); the two Retro-Go *return* signals (`CORE`, `RESET`) instead live in the **RG magic cell** (`0x20000000` = `RG_MAGIC_ADDR`), where Retro-Go leaves them, and both **re-launch Retro-Go** (jump `RETROGO_BASE`) rather than reaching the chainloader menu. Defined in [src/common/boot_magic.h](src/common/boot_magic.h).

| Value | Meaning | Cell / target |
|---|---|---|
| `0x544F4F42` | "BOOT" — software jump request | `0x2001FFF8`; target at `0x2001FFFC` |
| `0xFEDEBEDA` | Retro-Go standby resume | `0x2001FFF8`; jumps `RETROGO_BASE` (`0x0800A000`) |
| `0x434F5245` | "CORE" — Retro-Go "Return to Main Menu" → re-launch Retro-Go | RG cell `0x20000000`; jumps `RETROGO_BASE` |
| `0x1FA1AFE1` | "RESET" — Retro-Go warm-reset trace → re-launch Retro-Go | RG cell `0x20000000`; jumps `RETROGO_BASE` |
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
- [scripts/debug/boot_target_state.py](scripts/debug/boot_target_state.py) — dumps the unified boot selector's view of the world: the cached `board_console_type` against what Bank 2 and the external-flash OFW backups actually hold (reset vectors), and the resulting per-target `active_valid` (boot Bank 2 as-is vs re-flash the backup first). Explains boot-selector surprises and confirms both OFW backups are present and intact.
- [scripts/debug/trace.py](scripts/debug/trace.py) — CPU control. `reset-halt` forces a hardware reset (and auto-unswaps banks) to recover a hung MCU / OpenOCD connection. `halt` halts the CPU **without** resetting and prints PC/SP — use this to freeze a hang and inspect it *before* the state is lost. Also `resume`, `step`, `until <addr>`, `watch <addr>`.
- [scripts/debug/remote_control.py](scripts/debug/remote_control.py) — drive the on-device UI from your keyboard (or a real gamepad, via evdev) over the debug probe, CPU running. A thin front-end over the shared backend `scripts/common/remote_input.py`, which writes a button bitmask to the remote-input shadow cell (`0x2001FFF4`) that the firmware OR's into its live button state — so a keystroke is identical to a physical press (auto-repeat included). Lets a session navigate the menu, open the file browser, and trigger the Left+Game escape with no keyboard emulator or hardware mod. **Uses the `REMOTE_INPUT` hook, which is now compiled in by default** (opt out with `make REMOTE_INPUT=0`). See §10.1.

**Reset / run after flashing:** `gnwmanager start bank1` starts the flashed chainloader; `python scripts/debug/bank.py unswap` unswaps the banks and resets. Prefer these over `gnwmanager info`, which is a last-resort connection-debug fallback.

### 10.2 memory.py Usage & Reference

The `memory.py` script ([scripts/debug/memory.py](file:///home/doug/Nerd/git/gnw-chainloader/scripts/debug/memory.py)) is a versatile diagnostic utility for inspecting and modifying memory-mapped registers, SRAM, internal flash, and external flash (OSPI) on the STM32H7B0 target.

By default, the script halts the CPU before executing any subcommands and resumes it afterward. You can bypass this behavior with the `--no-halt` option to inspect a running target.

#### Address Math
All address arguments accept basic math expressions containing hex/decimal values and operators (`+`, `-`). This allows offsets to be specified directly relative to base addresses or symbols (e.g., resolving a symbol address from `arm-none-eabi-nm` and adding an offset):
```bash
python scripts/debug/memory.py read 0x0811AE70+0x301A5 8
```

#### Subcommands

##### 1. `read <address> <size> [count]`
Reads memory at the specified address.
- `size`: Access width in bits (`8`, `16`, `32`) or raw `bytes`.
- `count`: Number of elements to read (defaults to `1`).
- **Examples**:
  - Read a 32-bit CPU/MCU register (e.g., `RCC->RSR` reset status register at `0x580244D0`):
    ```bash
    python scripts/debug/memory.py read 0x580244D0 32
    ```
  - Read the SRAM magic boot intent cell at `0x2001FFF8`:
    ```bash
    python scripts/debug/memory.py read 0x2001FFF8 32
    ```
  - Read raw bytes (e.g., 16 bytes starting at `0x24000000`):
    ```bash
    python scripts/debug/memory.py read 0x24000000 bytes 16
    ```

##### 2. `write <address> <size> <value>`
Writes a value or byte pattern to the target memory or register.
- `size`: Access width in bits (`8`, `16`, `32`) or raw `bytes`.
- `value`: Hex/decimal integer value, or space-separated hex bytes string (when size is `bytes`).
- **Examples**:
  - Write a 32-bit button mask to the remote-input shadow cell at `0x2001FFF4`:
    ```bash
    python scripts/debug/memory.py write 0x2001FFF4 32 0x10
    ```
  - Write raw bytes:
    ```bash
    python scripts/debug/memory.py write 0x24000000 bytes "AA BB CC DD"
    ```

##### 3. `dump <address> <length> <file>`
Dumps a contiguous memory region of `length` bytes to a local file. Reads are executed in 64 KiB chunks to avoid OpenOCD timeouts.
- **Example**:
  - Dump the 1 MiB Bank 1 internal flash to a local file:
    ```bash
    python scripts/debug/memory.py dump 0x08000000 0x100000 build/bank1_dump.bin
    ```

##### 4. `compare <address> <file> [--length <length>]`
Compares the target's memory starting at `address` against the contents of a local file. Reports byte mismatches (up to the first 20 differences).
- **Example**:
  - Compare Bank 1 flash against a local compiled chainloader binary:
    ```bash
    python scripts/debug/memory.py compare 0x08000000 build/gnw_chainloader.bin
    ```

##### 5. `search <start> <end> [pattern] [--ospi]`
Searches a memory range (aligned to 4-byte boundaries) for either a specific byte pattern or external flash (OSPI) pointers.
- `pattern`: Hex value or bytes string to search for (e.g., `AABB` or `0x12345678`).
- `--ospi`: Scans for any 32-bit words that fall within the external flash memory-mapped range (`[0x90000000, 0x94000000)`).
- **Examples**:
  - Search for references to the SRAM magic cell:
    ```bash
    python scripts/debug/memory.py search 0x24000000 0x24040000 0x2001FFF8
    ```
  - Scan memory for OSPI pointers:
    ```bash
    python scripts/debug/memory.py search 0x24000000 0x24040000 --ospi
    ```

### Live-state debugging playbook

`memory.py` reads halt→read→**resume** (and `trace.py halt` halts without reset), so you can peek a *running or just-booted* device without disturbing its boot state. This is what makes boot-bug forensics possible: get the device into a scenario, leave it parked, and read.

**Useful non-DTCM state registers (read with `memory.py read <addr> 32`):**

| Address | Register | What it tells you |
|---|---|---|
| `0x2001FFF8` / `0x2001FFFC` | SRAM magic / target | pending boot intent (see §9); note this sits at the stack top, so it reflects stale stack at idle |
| `0x2001FFF4` | remote-input shadow | injected button bitmask (`REMOTE_INPUT`, now default-on — see §10.1); `input_button_t` bit order |
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
- `scripts/build/push_batched.py` — push files to the device LittleFS via gnwmanager's in-process bindings, **always draining openocd's stdout**: an undrained subprocess pipe-buffer deadlock (openocd blocks on `write()` once the ~64 KB buffer fills, on the slow first LittleFS-mount write) was the long-standing "multi-file push hangs" bug; setting `GNWMANAGER_OPENOCD_DEBUG=1` unconditionally keeps the pipe drained. Probe-driven tests bound every operation with `time_budget()` (SIGALRM, since the openocd TCL socket has no per-command timeout) and assert `chainloader_running()` (the chainloader's SysTick `uwTick` advancing) before trusting a read, because a flash or push leaves the gnwmanager RAM flasher resident, not the chainloader.

**Binary inspection of the chainloader ELF:**
```bash
arm-none-eabi-nm -S build/gnw_chainloader.elf | grep "<symbol>"
arm-none-eabi-addr2line -e build/gnw_chainloader.elf 0x0800XXXX
arm-none-eabi-objdump -d build/gnw_chainloader.elf | sed -n '/<func>:/,/^$/p'
```

### 10.1 Remote input over the debug probe

A test-only path that lets a workstation drive the on-device UI through the SWD/JTAG probe, with no keyboard emulator and no hardware modification. It exists to make a debug session able to *act* on the device (navigate menus, enter the file browser, trigger the launcher escape) instead of only observing it — the missing half of autonomous hardware testing alongside `capture.py` / `memory.py`.

**Mechanism — a shadow cell.** The hardware GPIO input registers (`IDR`) are read-only, so physical presses can't be forged there. Instead a single 32-bit "shadow" word lives at `SRAM_REMOTE_INPUT_ADDR` = `0x2001FFF4` (defined in `src/common/memory_map.h`), one word below the boot-magic cells and inside the same top-of-DTCMRAM region the OFW stack reservation already protects. The app runs from AXI-SRAM (`0x24000000`+), so this DTCM address never collides with its code, stack, or module pool. A debug-probe write to the cell is the entire host→device channel.

**Bit format — the "unified format."** Bit positions follow the chainloader's `input_button_t` enum (`src/chainloader/system/input.h`): UP=0, DOWN=1, LEFT=2, RIGHT=3, A=4, B=5, START=6, SELECT=7, PAUSE=8, GAME=9, TIME=10, PWR=11. The host always speaks this format; the OFW patch translates the bits it understands into the stock gamepad encoding.

**Two device-side consumers, both gated behind `-DREMOTE_INPUT`:**
- *Chainloader* (`system/input.c`): `input_update()` OR's the shadow word into each button's pressed state, so the existing just-pressed / auto-repeat logic treats a remote hold exactly like a finger hold. `input_init()` clears the cell so a cold boot with no host attached injects nothing.
- *OFW patch* (`src/patch/main.c`): `read_buttons()` already hooks the stock firmware to honor the Left+Game launcher-escape macro; with the flag it also maps the shadow cell's LEFT/GAME bits into the stock `gamepad` word, so the escape works from inside a running Mario/Zelda game too (verified on hardware — see the tests below). A full in-game remote would require reverse-mapping the entire stock gamepad layout and is intentionally out of scope.

**Build gating.** The hook is **on by default** — it ships as a feature (drive the UI over the probe), not just a debug aid — so the build adds `-DREMOTE_INPUT` to both `C_DEFS` (app) and `PATCH_CFLAGS` (patch) unless you opt out with `make REMOTE_INPUT=0`. The device-side cost is ~8 bytes (which golden now carries).

**Host side — three layers, one backend.** The mechanism lives once in `scripts/common/remote_input.py` and is shared by a manual front-end and the automated tests (so a test never depends on `scripts/debug/`):

- **`scripts/common/remote_input.py`** — the engine. Holds **one persistent OpenOCD connection** and exposes a `button_press()` API: `dev.button_press([BTN_DOWN])` is a clean tap, `repeat=N` taps N times, and `with dev.button_press([BTN_LEFT, BTN_GAME], hold=True): …` holds for the block. The persistent connection is the whole point — a tap is `set mask → ~80 ms → clear` over one open socket. (The original mistake was making a tap out of two separate `memory.py` invocations, each spinning up its own OpenOCD; press→release ended up *seconds* apart, so the firmware auto-repeat treated every "tap" as a long hold and scrolled the whole menu.) An `InputTransport` seam abstracts *how* a press is delivered — today `ShadowCellTransport` (SWD), tomorrow a Raspberry-Pi GPIO rig driving NRST / power / real button lines behind the same API. `reconnect()` re-opens the link after a device reset (a bank swap or the Left+Game escape resets the MCU and tears down the SWD session at the process level).
- **`scripts/common/harness.py`** — test helpers: resolve a firmware symbol via `nm` (tolerating LTO's `.lto_priv` suffixes; addresses move every rebuild, so never hardcode), read the live menu selection (`g_list_main.selected`, offset 8), closed-loop `navigate_to()` (reads selection and steps toward the target — robust against idle-hide, which blanks the menu after ~30 s so the first input only un-hides it), capture a frame over the same connection, `wait_u32()` (poll-with-reconnect across a reset), and a tiny pass/fail `TestRun`.
- **`scripts/debug/remote_control.py`** — manual control. Keyboard (raw-mode; terminals send key-down but not key-up, so a key holds for `--hold` ms refreshed by the terminal's own auto-repeat) or `--gamepad` (Linux evdev, which gives true press/release). Restores the terminal via try/finally + atexit + signal handlers and always clears the cell on exit.

**Tests** (`scripts/tests/`, depend only on `common/`):
- `test_remote_input.py` — asserts a single tap moves the selection by exactly one (the regression that proved the timing fix), and `repeat=3` by exactly three.
- `boot_selector_test.py [--target all|retrogo|mario|zelda]` — drives the unified LAUNCH selector over the probe and verifies it boots each target this unit offers. Selection is closed-loop on the firmware's own `g_boot_target` (read over SWD), never an OCR guess. An earlier OCR-driven version mis-read the selection and reported a Zelda boot bug that did not exist (MARIO was selected and Mario correctly booted). **RETRO-GO**: A resets via `board_request_jump` and the stub re-launches Retro-Go from Bank 1 (PC lands in the Retro-Go flash region, chainloader header gone). **MARIO/ZELDA**: A flashes the patched OFW backup into Bank 2 if it isn't already active, bank-swaps, and boots it with no manual power-on (the warm-switch fix), confirmed by the reset vector at `0x08000004` (Mario `0x08018101`, Zelda `0x0801B3E1`); then **Left+Game** FRCE-resets and unswaps back to the chainloader. The escape runs even if the wrong OFW boots, so a mis-boot never strands the banks swapped. Verified end-to-end on hardware for all three targets (6/6), re-flashing in both directions (Mario over a Zelda Bank 2 and vice-versa).

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
| FrogFS     | —      | recognized + sized in the partition viewer; not browsable |
| FAT/exFAT  | R/O    | SD card directory listing for file browser          |

### Implementation

#### LittleFS (Read-Write)

The LittleFS core (the `deps/littlefs/` submodule, wrapped by `src/chainloader/storage/lfs_wrapper.c`) is built read-write; the `LFS_READONLY` flag that once kept it read-only is no longer set. The block device callbacks `lfs_flash_prog` and `lfs_flash_erase` in `lfs_wrapper.c` implement write and erase by temporarily exiting OSPI memory-mapped mode (`OSPI_DisableMemoryMappedMode()`), calling the OSPI program/erase APIs from `flash.c`, and immediately re-entering memory-mapped mode (`OSPI_EnableMemoryMappedMode()`). Cache and page sizes are set to 256 bytes to match the OSPI page-program granularity and avoid alignment faults.

The mount address is not hardcoded. Instead, `lfs_wrapper.c` exposes `lfs_test_mount_at(uint32_t addr, uint32_t block_count)` so the application can pass the address and size discovered by the partition scanner at runtime.

#### FrogFS (Recognition Only)

FrogFS is **recognized and sized but not browsable.** The partition scanner
(`storage/partition.c`, `check_address`) reads the `FROG` magic + `bin_sz` + entry
count straight from the memory-mapped `frogfs_head_t` header, so a FrogFS partition
shows up in the **partition viewer** with its address/size/entry count. The content
reader/driver (the former `src/chainloader/storage/frogfs_reader.c` + its
`frogfs_vfs_*` driver) was **removed** to reclaim flash — there is no `FROGFS`
filesystem driver, so the file browser cannot enter a FrogFS partition (it degrades
gracefully: `mount_tab_partition` null-checks the driver and shows an empty list).
The VFS read path (`read_from_partition`, `vfs_module_available`) no longer has a
FrogFS branch. (If full FrogFS *listing* is ever wanted again, restore the reader as
a PIE module rather than in-core.)

#### FatFS with exFAT

The chainloader builds the FatFs source in two flavors from a staged `ffconf.h`: a tiny **in-core read-only bootstrap** (long filenames off, CP437) that is always present, and a **loadable `fatfs.bin` PIE module** that enables full read-write exFAT with 255-character long filenames. A minimal block-device driver (`src/chainloader/storage/fatfs_diskio.c`) routes FatFs `disk_read` to either the memory-mapped flash or the SD card block device (`src/chainloader/storage/sdcard.c`), selected by the volume base address; in-core write stubs return `RES_WRPRT` (the RW module performs the writes).

#### Theme System (slot model + PIE theme module)

The theme system is a **slot model**. The 40 KiB core ships only **color-only Default / Fallback themes**: a `theme_driver_t` (`ui/theme.h`) is a name plus five RGB565 colors (background, foreground, accent, and border/footer shades) with optional `draw_selector` / `draw_footer` / `draw_background` callbacks. The **sprite themes** (Zelda `FAIRY` animated cursor; Mario `COIN` and the original `YOSHI` brick-floor look) live in a loadable **PIE theme module** (`src/modules/theme/module_entry.c`, built to `theme.bin`), so their blitter and tile recipes cost the 40 KiB core no flash.

The PIE loader brings the module in (`mod_load_theme`); its `init_module()` registers the OFW-appropriate `theme_driver_t`(s) through the host vtable `theme_host_api_t`. The module carries its own sprite blitter (the core's `gui_draw_asset` was stripped) and reads the live framebuffer plus the OFW tileset/palette through that vtable, so sprites that reference stock Nintendo graphics are read at runtime from the OFW external asset blocks (Zelda block at `0x90000000`, Mario block at `0x90400000`) and never embedded in the repository.

The active per-OFW theme slot is persisted in `TAMP->BKP3R` nibbles; a `< THEME >` value-selector in Settings cycles the registered themes live, and switching the OFW re-registers the module for the new console. If the module is absent, the launcher falls back to the in-core color-only theme: a plain background with hard-coded colors and the built-in Fusion Pixel 12px monospaced font. All functional systems (navigation, file browser, diagnostics, boot targets) remain fully operational in that fallback.

#### File Browser Integration

The partition viewer (`ui/partition_viewer.c`) presents an action menu when the user presses `A` on a detected filesystem partition. The `PAGE_BROWSER` page (`ui/ui_file_browser.c`) is invoked with the partition's filesystem type tag, base address, and size. The browser dispatches directory listing to the appropriate backend (LittleFS or FatFS; FrogFS partitions have no driver, so entering one yields an empty list) and renders the results with the standard `ui_list` widget, supporting scroll, back navigation, and eventual file-action hooks.

#### SD Installer (transient module) and the shared streaming copy

Installing artifacts from an SD card is done by a **transient PIE module**,
`/modules/installer.bin` (`MOD_FLAG_TRANSIENT`): the core loads it on demand, runs
it, then reclaims its pool slot (`mod_pool_mark` / `mod_pool_reset`). One generic
descriptor-driven path installs **two artifact classes**: language packs
(`/i18n/<code>.lang` plus their `/i18n/fonts/<script>.fnt`) and PIE modules (the fixed
`/modules/*.bin` list); header-only reads peek each artifact's version. The install
gate mirrors the loader's LOAD gate (magic plus the running core's ABI for that class,
using the core's own ABI values; see §5 ABI Gating), so nothing the firmware could not
load is ever committed.

At boot the core shows a **per-class confirm pop-up** ("Install N language(s) from
SD?" or "Install N module(s) from SD?", or both combined). On accept it commits:
**languages apply live** (the core re-runs i18n re-discovery via the language module's
`rediscover()` and re-applies), while **installed modules apply on the next boot**.

The actual copy **streams** through one shared in-core function `vfs_copy_open_file`
(4 KiB buffer), the same copy loop the file browser uses; modules reach it via
`vfs_copy_sd_to_lfs`. There is no whole-file buffer.

---

## 12. Internationalized UI

### Overview

The menu UI is translatable into any language, including non-Latin scripts. The
guiding constraint is the project's stability invariant: a missing or corrupt
translation/font asset can never block the boot path, so the design layers three
independent fallbacks — in-core English text, English per-string, and in-core
ASCII glyphs — each degrading to the one below. Only ~1.5 KB of the 40 KB binary
is spent on the feature; the translations and the fonts they need live on the
device filesystem, fetched at runtime. Right-to-left languages (Arabic, Farsi)
**mirror the layout** rather than reorder glyphs, their text reshaped and bidi-ordered
**offline at cook time** (no runtime shaper); and a non-Latin font beyond the active
language **loads on demand** by Unicode range, so e.g. a CJK filename renders under an
English UI. See [docs/i18n.md](docs/i18n.md) for the layer detail, formats, the RTL
mirror, the `gui_api` module surface, and the on-demand font slots.

### Implementation

Three layers, each overriding the one beneath it (the same "active overrides
core, else fall back" pattern as the theme system):

- **In-core (always present):** a baked Fusion Pixel **12px monospaced** ASCII
  font (`ui/gui_font.c`, mixed-case, generated from the OTF by
  `scripts/build/cook_font.py`) and the English string table (`ui/strings.c`,
  every label behind a `string_id_t` + `tr()`). The renderer was rewritten for
  UTF-8 decoding and proportional widths; the HAL-free core of it (`gui_text.c`:
  `gui_utf8_next` / `gui_glyph` / `gui_text_width`) is host-unit-tested.
> **Lean-core note:** everything below — `font_ext`, discovery, pack +
> script-font loading, and switching — now lives in the **PIE language module**
> (`src/modules/language/` → `/modules/language.bin`, the former `ui/i18n.c` +
> `ui/font_ext.c`), NOT the firmware. SD install is no longer one of its jobs (it
> was de-installed); the module now exposes a `rediscover()` the core calls after an
> install. The core keeps only the baked ASCII font +
> English `strings.c` + a thin `ui/i18n.c` shim that loads the module when present
> and delegates (English-only defaults when absent). `gui_glyph` resolves non-ASCII
> through a registerable hook (`gui_set_ext_glyph`) the module fills, so the module
> owns rendering too. The module loads at boot whenever present (so UTF-8 works even
> with English active) and stays resident. Contract: `src/chainloader/system/language.h`.

- **Shared script fonts:** per-script `.fnt` blobs on the filesystem
  (`/i18n/fonts/<script>.fnt`) supply non-ASCII glyphs (accented Latin, CJK). The
  module's `font_ext.c` reads them **LFS-only** (fonts are installed to LittleFS; the
  SD is delivery-only, so a stale SD copy can't shadow them); each blob carries a
  `ref_top` so its glyphs share the in-core ASCII baseline exactly. A small font is read
  whole into RAM and binary-searched; a **complete CJK** font is too big, so it is held
  open on LittleFS and **paged glyph-by-glyph** (binary-search `codepoints[]` by seek,
  bitmap on demand, small LRU) through the handle-based `vfs_stream_t` primitive the
  feature/MP3 file path also uses. Three slots are tried in order — the active language's
  **script** font, an always-on **Latin base** (`latin.fnt`), then an on-demand **AUX**
  font by Unicode range — so accented Latin renders in any language (e.g. `Scheiße.txt`)
  and an arbitrary CJK filename renders under any UI. `latin.fnt` also carries
  controller/logo icon glyphs the `{TOKEN}` text replacer (`gui_text.c` `gui_text_next`)
  draws from a `{NAME}` shortcode. Detail: [docs/i18n.md](docs/i18n.md).
- **Language packs (discovered, not registered):** per-language `.lang` files
  (`/i18n/<code>.lang`, magic `'LNG2'`) hold the translated strings and
  **self-describe** — each 76-byte header carries its own locale code + endonym +
  script. **Only English is baked into the firmware;** at boot the language module
  builds the list by *scanning* `/i18n/<code>.lang` on LittleFS (reading each
  header), so a language never compiled in appears once its pack is present. Packs
  are ABI-checked against `STRINGS_ABI_VERSION`; the core's `tr()` points at the
  active table the module installs (English fallback per id). Sorted by code, English first.

Runtime reads are **LittleFS-only** (no per-switch SD scan). The active language
is persisted **by code** in `/i18n/.active` (written on Settings exit, read at
boot); until the filesystems are scanned (and whenever an asset is absent) the UI
is in-core English. SD distribution runs through the **transient installer module**
(`/modules/installer.bin`, `MOD_FLAG_TRANSIENT`): if the SD has new/newer
`<code>.lang` packs (plus the fonts they declare), the core shows a per-class
confirm pop-up at boot ("Install N language(s) from SD?") and on accept commits them
into LittleFS's `/i18n` data folder (deleting each SD source after a successful install,
since the SD is delivery-only), never a bank. Installed languages apply live
(the core calls the language module's `rediscover()`, then re-applies). So a user
drops `build/i18n/` onto the SD's `/i18n/` (a 1:1 mirror, no renaming). Assets are
cooked by `make i18n` (`make push_i18n` is the dev shortcut). Full reference, file
formats, and "adding a language" steps: [docs/i18n.md](docs/i18n.md).

**Module translations** are self-contained in each module binary (no device-side
files): a module's per-language strings live in `i18n/modules/<module>/<lang>.json`,
cooked by `scripts/build/cook_modstrings.py` (also run by `make i18n`) into an
`[id][lang]` matrix compiled into the module; the module resolves them via `ms(MS_*)`
after picking its column from `gui->lang_code()` (the `gui_api_t` seam exposes `tr()` +
`lang_code()`), English fallback. RTL module text is pre-shaped at cook time
(token-safe footers, no mirror); a feature module's **menu entry** is localized by
packing its title per language into the module header (`menu_label_xlat`), which the
core resolves live at discovery without loading the module. The MP3 player is the first
consumer. Detail: [docs/i18n.md](docs/i18n.md).

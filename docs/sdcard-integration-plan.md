# SD Card Integration Plan

**Status:** Planning. No code changes yet.
**Budget:** ~600 bytes of free space in `gnw_chainloader.bin` (current: 32,128 / 32,768 ≈ 640 B free).
**Goal:** The launcher recognises an SD card mod when present, mounts FAT/exFAT through the existing dynamic `fatfs.bin`, and remains 100% functional on consoles with no SD mod installed. The stub is never aware of SD.

**Read/write split:** Read-only SD support is mandatory and lives in the host (chainloader binary). Read-write support is delivered by a new dynamic driver `sd_rw.bin`, loaded on demand from LittleFS exactly like `lfs_rw.bin` and `fatfs.bin`. If `sd_rw.bin` is not installed on the device, the SD card cleanly degrades to read-only — copy/paste/delete simply aren't offered for the SDCARD tab.

The plan is split into phases so each can be built, flashed, and verified on hardware in isolation before the next is started. Every phase is sized so the running binary stays under the 32,767-byte stub limit.

---

## 0. Reference Design and Project Constraints

### 0.1 Where the SD card lives on the hardware

The standard Game & Watch SD mod taps the four OSPI bus pins, splicing the card onto pins already in use by the external NOR flash. The example project at `example projects/game-and-watch-bootloader/` shows two supported wiring variants:

| Wiring                  | Pins                                                    | Driver used in example       |
| ----------------------- | ------------------------------------------------------- | ---------------------------- |
| OSPI pin-tap (common)   | PE11 (CS), PB1 (MOSI), PB2 (CLK), PD12 (MISO)           | `softspi.c` + `user_diskio_softspi.c` |
| Separate-pin SPI1 PCB   | PA15 (VCC), PB9 (CS) + SPI1 alt-function pins           | HAL SPI + `user_diskio_spi.c` |

The pin-tap path is mutually exclusive with OSPI memory-mapped access — when SD is alive the external flash is dead, and vice-versa. The example handles this via `switch_ospi_gpio()`.

**This plan targets the OSPI pin-tap variant only.** Reasons:

1. It is the dominant mod in the wild.
2. Bit-banged SoftSPI is roughly an order of magnitude smaller than pulling in `stm32h7xx_hal_spi.c` and friends, which is the only way to stay within the 600-byte budget.
3. A separate-pin SPI1 build can be added later behind a `-DSDCARD_SPI1=1` compile flag without disturbing the host/driver split designed here.

### 0.2 Existing space-saving patterns we must honour

The chainloader's flash budget is preserved by an unusually disciplined set of conventions. Any new code added in this work must follow them, not invent parallel mechanisms:

* **Two-stage RAM-boot** — 32 KB flash stub LZMA-inflates the app to AXI-SRAM (`0x24000000`, 1 MB). The app never executes from flash. All SD code lives in the app.
* **Dynamic drivers** — `fatfs.bin` (11.6 KB) and `lfs_rw.bin` (13.5 KB) are stored as files inside a LittleFS partition on external flash. They are loaded on demand into fixed AXI-SRAM slots (`0x240A0000` for FAT, `0x240C0000` for LFS-RW) and entered via the `init_driver(vfs_driver_t*, const host_api_t*)` symbol pinned at offset 0 by `ENTRY(init_driver)` plus a `KEEP(*(.text.init_driver))` first in the driver linker script. **SD support reuses `fatfs.bin` exactly — no new driver binary.**
* **Host-API callback contract** — Dynamic drivers contain no HAL. They route all hardware access through `host_api_t` callbacks. SD I/O will be added the same way.
* **Bare-metal register writes** — `GPIOx->BSRR`, `RCC->CR`, `PWR->CR1` directly rather than `HAL_GPIO_Init`/`HAL_PWR_*`. The new SD GPIO multiplex follows suit.
* **Table-driven GPIO setup** — `gpio_cfg_t` arrays consumed by `gpio_init_table()` already exist in `board.c`. SD's GPIO pin maps go in the same table style, not as fresh `GPIO_InitStruct` boilerplate.
* **No `printf`/`sprintf`** — `int_to_str`, `hex_to_str`, `format_size` exist; reuse them.
* **Type detection by first-letter** — `type[0] == 'L'`, `type[0] == 'F' && type[1] == 'A'` etc. SD partitions are also `"FAT"` typed, so the existing fast-path branch already covers them.

### 0.3 Linker scripts — no changes needed

The three active linker scripts and the two driver scripts already give us what we need:

* `STM32H7B0_FLASH_STUB.ld` — 32 KB stub. **No SD code here.** Untouched.
* `STM32H7B0_RAM_APP.ld` — 1 MB AXI-SRAM app. SD host code lives in here; no script change.
* `src/drivers/fs/fatfs/STM32H7B0_DRIVER.ld` — Driver region `0x240A0000`, 128 KB. The current `fatfs.bin` uses only ~11.6 KB; adding pdrv=1 dispatch lands well within the slot.
* `src/drivers/fs/lfs_rw/STM32H7B0_DRIVER.ld` — Untouched.
* **NEW** `src/drivers/sd_rw/STM32H7B0_DRIVER.ld` — Driver region `0x240E0000`, 128 KB. Cloned from the existing driver linker scripts. `sd_rw.bin` is expected to be well under 2 KB; the rest of the slot is unused.

`STM32H7B0VBTx.ld` is a stale legacy file unused by any build target; leave it alone.

---

## 1. Phase 1 — Host-side SD I/O Layer (read-only)

### 1.1 Goal

Stand up a minimal, read-only, hardware-only SD-over-SoftSPI implementation in the app. No file system, no integration with `vfs` or the file browser yet. End state: a single function `sdcard_read_sector(sector, buf)` works on a device that has the mod, and is a no-op (returns failure) on one that doesn't. Writes are deferred to Phase 2.5's dynamic driver.

### 1.2 New files

* `src/chainloader/storage/sdcard.c`
* `src/chainloader/storage/sdcard.h`

### 1.3 What goes in `sdcard.c`

Written demoscene-tight: every function `static` unless exposed, no helper indirection unless it actually shrinks code, magic numbers inline.

* **`switch_ospi_to_sd()` / `switch_sd_to_ospi()`** — Mirrors `switch_ospi_gpio()` from the example. Reconfigures PE11/PB1/PB2/PD12 between OSPI alt-function and GPIO mode for SoftSPI. Implemented as two `gpio_cfg_t` tables fed to `gpio_init_table()` (reused from `board.c` — promote it from `static` if needed). The OSPI side must also call `HAL_OSPI_DeInit()` / `HAL_OSPI_Init()` to keep the peripheral state consistent. Cache cleans around any OSPI memory-mapped enable/disable, as `lfs_wrapper.c` and `vfs.c` already do.
* **SoftSPI byte exchange** — One function `sd_xchg(uint8_t byte) -> uint8_t`. Direct `BSRR`/`IDR` reads, never HAL. **Made non-static and exposed via the header** so the RW driver in Phase 2.5 can reuse it through `host_api_t.sd_xchg`. This is the only function in `sdcard.c` that the driver calls into directly.
* **SD card command primitive** — `sd_cmd(cmd, arg) -> response_byte`. Single combined send + wait-for-response. CRC values are inline compile-time constants (`0x95` for CMD0, `0x87` for CMD8, `0x01` for everything else); no runtime CRC generation.
* **Init state machine** — `sdcard_init(void) -> bool`. CMD0 → CMD8 → ACMD41 (with HCS) → CMD58 read OCR. Stores `card_type` (block-addressed vs byte-addressed) in a single `uint8_t` static; both `sdcard_read_sector` and the future RW driver consult it via host_api.
* **Read sector** — `sdcard_read_sector(uint32_t sector, uint8_t *buf) -> int`. CMD17 + 0xFE token, 512-byte transfer, two CRC discard bytes. **Single-sector only**; FatFs calls us once per sector.

**Not in host (moved to `sd_rw.bin` in Phase 2.5):**
* `sdcard_write_sector` — CMD24 + 0xFC token + verify. ~70 B saved.
* `sdcard_get_sector_count` (CSD parse) — ~60 B saved. Capacity is instead derived from FatFs cluster metadata after mount, where `fat_vfs_statfs` already does it.

The header exposes: `sdcard_init`, `sdcard_read_sector`, `sd_xchg`, plus `card_type` accessor (`sdcard_is_block_addressed()`), plus `switch_ospi_to_sd`, `switch_sd_to_ospi`. Internal helpers stay `static`.

### 1.4 Mutex with OSPI

Any function that touches the SD pins must be bracketed:

```
switch_ospi_to_sd();  // also calls OSPI_DisableMemoryMappedMode + HAL_OSPI_DeInit
... SD work ...
switch_sd_to_ospi();  // re-init OSPI, re-enable mem-mapped mode
```

The host wrapper (Phase 2) owns this bracketing so the driver and partition layers never reach across it.

### 1.5 Make / build

Add `src/chainloader/storage/sdcard.c` to `APP_SOURCES` in the `Makefile`. **Do not add it to `STUB_C_SOURCES`** — the stub must never link SD code.

### 1.6 Size impact (estimated)

* OSPI ↔ SD GPIO retabulation: ~80 B (two small tables, share existing `gpio_init_table`)
* SoftSPI byte exchange (`sd_xchg`): ~40 B
* `sd_cmd` primitive: ~60 B
* Init state machine: ~140 B
* `sdcard_read_sector`: ~50 B
* Two `host_api_t` wrappers with OSPI/SD bracketing: ~40 B

**Total: ~410 B** of `.text`, leaving ~190 B headroom inside the 600 B budget for Phases 3, 4, and 6. `sdcard_write_sector` and `sdcard_get_sector_count` are explicitly excluded from the host — they live in `sd_rw.bin` (Phase 2.5).

Pre-flight: build with stubbed (`return -1`) SD ops to confirm scaffolding alone fits, then add functions one at a time and watch the size delta. If actual measured size exceeds the budget mid-phase, see §7.3 for triage.

### 1.7 Verification

* Build the chainloader; confirm under 32,767 B and that the device with NO SD card still boots cleanly. This is the headless-boot invariant from CLAUDE.md, Engineering Rules.
* Add a temporary "SD ping" entry in the Tools menu (debug-only, removed before Phase 4) that calls `sdcard_init()` then `sdcard_read_sector(0, buf)` and prints the OCR / first 16 bytes via an existing modal. **Verify on hardware on a console with the SD mod**: confirm the card initialises and sector 0 reads back the MBR boot signature.
* On a device without SD: confirm `sdcard_init()` returns false cleanly and the OSPI flash is left intact afterwards (file browser still works, LittleFS partitions still mount).

---

## 2. Phase 2 — Host-API Extension and Driver Routing

### 2.1 Goal

The existing `fatfs.bin` driver gets one new code path: when mounted in "SD mode", `disk_read`/`disk_write`/`disk_initialize`/`disk_ioctl` route through new `host_api_t` callbacks instead of `flash_read`/`flash_write`. The same driver binary handles both modes; no second driver, no second linker script.

### 2.2 `host_api_t` additions (`src/chainloader/storage/vfs.h`)

Four new callbacks, all nullable so older drivers/host versions stay binary-compatible:

```
int     (*sd_init)(void);
int     (*sd_read)(uint32_t sector, void *buf, uint32_t count);
int     (*sd_write)(uint32_t sector, const void *buf, uint32_t count);  /* NULL until sd_rw.bin loaded */
uint8_t (*sd_xchg)(uint8_t byte);                                       /* exposed for sd_rw.bin */
```

`count` is always 1 in this phase (single-sector). The host wrappers (`sd_init`, `sd_read`) internally take the OSPI/SD switch on entry and restore on return — the driver sees only "read N sectors from disk."

`vfs_init()` in `vfs.c` populates `sd_init`, `sd_read`, and `sd_xchg` with thin wrappers into `sdcard.c`. `sd_write` is set to `NULL` here and patched by `sd_rw.bin`'s loader in Phase 2.5. `fatfs.bin`'s `disk_write` for SD mode checks `g_host->sd_write` for `NULL` and returns `RES_WRPRT` when absent — so cards mount read-only when the RW driver is missing.

The OSPI/SD bracketing belongs in `vfs.c`'s host wrappers, **not** in the driver. The driver sees a flat read/write API; the host owns peripheral state.

### 2.3 Driver mount-mode signalling

`vfs_driver_t.mount(uint32_t base_addr, uint32_t size)` stays. We overload the existing signature using a sentinel that flash addresses cannot collide with:

* `mount(0, 0)` → SD-card mode.
* Anything else → OSPI partition at that physical address as today.

Inside `driver_entry.c`:

* Add a single `static uint8_t g_drive_mode;` — 0 = OSPI (current behaviour), 1 = SD.
* `fat_vfs_mount` switches mode based on the sentinel, then calls `f_mount` as before. FatFs's `FF_VOLUMES=1` stays at 1; we never have both mounted simultaneously.
* `disk_initialize(pdrv=0)`: if `g_drive_mode == 1`, call `g_host->sd_init()`; else return 0 (OSPI is always ready).
* `disk_read(pdrv=0, ...)`: branch on `g_drive_mode`. SD mode calls `g_host->sd_read(sector, buff, count)`; OSPI mode keeps the current memory-mapped read path.
* `disk_write(pdrv=0, ...)`: same branch. SD mode calls `g_host->sd_write(...)`; OSPI mode keeps the 4 KB-page erase/program cycle.
* `disk_ioctl(CTRL_SYNC)`: SD mode is a no-op (every write already flushes). OSPI mode keeps the flush logic.
* `fat_vfs_unmount`: on SD mode, no flash cache to flush; just `f_mount(NULL, "0:", 1)`.

### 2.4 Why this stays cheap

The driver gains one byte of state and one `if (g_drive_mode)` per disk_io entry. Driver `.text` grows by perhaps 80-150 B inside the 128 KB AXI-SRAM slot — invisible to the chainloader budget.

### 2.5 Verification

* Build `fatfs.bin`; confirm it still fits in 128 KB and the chainloader still loads it for OSPI FAT partitions (no regression on the existing file-browser flow).
* From the temporary Tools-menu hook (Phase 1), extend it to: load `fatfs.bin`, call `drv->mount(0, 0)`, then `drv->opendir("/", &ctx)` and print the first few entries. Verify on hardware with a known-good FAT-formatted SD card. **Confirm write attempts fail cleanly with `RES_WRPRT`** since `sd_rw.bin` isn't loaded yet.

---

## 2.5. Phase 2.5 — `sd_rw.bin` Dynamic Driver (read-write)

### 2.5.1 Goal

A new minimal dynamic driver binary exposing the SD-card write primitive. It is loaded on demand by the file browser when the user attempts a copy-to-SD, delete-on-SD, or paste-to-SD operation. If the file `/drivers/sd_rw.bin` does not exist on LittleFS, the SD card is read-only and the offending UI options are not shown.

### 2.5.2 New layout (mirrors `src/drivers/fs/lfs_rw/`)

```
src/drivers/sd_rw/
  driver_entry.c
  STM32H7B0_DRIVER.ld
```

### 2.5.3 Driver linker script

Single 128 KB MEMORY region at `0x240E0000` (after `lfs_rw` at `0x240C0000`). `ENTRY(init_driver)`, `KEEP(*(.text.init_driver))` first in `.text`. Identical structure to `src/drivers/fs/lfs_rw/STM32H7B0_DRIVER.ld` except for the ORIGIN.

### 2.5.4 What goes in `driver_entry.c`

* **`sdcard_write_sector(uint32_t sector, const uint8_t *buf) -> int`** — CMD24 SINGLE_WRITE, 0xFE token, 512 bytes via `g_host->sd_xchg`, two CRC dummy bytes, response check, ready-wait. The CMD primitive is duplicated here (small, ~30 B) so we don't need a second host callback for it.
* **`init_driver(sd_rw_ops_t *out_ops, const host_api_t *api)`** — Captures `api` into `g_host` (driver-static) and writes `out_ops->write_sector = sdcard_write_sector`. Mirrors the FAT driver's contract but with a much smaller `sd_rw_ops_t`:

  ```c
  typedef struct {
      int (*write_sector)(uint32_t sector, const uint8_t *buf);
  } sd_rw_ops_t;
  ```

### 2.5.5 Host-side loader (`vfs.c`)

Add `vfs_load_sd_rw_driver(void) -> bool` that:

1. Returns `true` immediately if already loaded (idempotent like `vfs_is_lfs_rw_loaded`).
2. Locates `/drivers/sd_rw.bin` on the LittleFS partition via the existing partition scanner (same iteration loop as `vfs_load_dynamic_driver`).
3. Reads it into `0x240E0000`, cleans dcache, invalidates icache.
4. Calls `init_driver` at `(0x240E0000 | 1)` with a stack-local `sd_rw_ops_t` and the address of `g_host_api`.
5. Patches `g_host_api.sd_write = ops.write_sector;` so `fatfs.bin`'s `disk_write` for SD now finds a non-NULL write callback.
6. Sets an internal `g_sd_rw_loaded = true;` flag, exposed via `vfs_is_sd_rw_loaded()`.

Note that the wrapper between FatFs and the loaded `sdcard_write_sector` still wants the OSPI/SD bracketing — so the actual `sd_write` host-API callback is a **host-side wrapper** function that brackets `switch_ospi_to_sd` → `ops.write_sector(sector, buf)` → `switch_sd_to_ospi`. Step 5 above is more precisely: stash the driver's write pointer in a static, point `g_host_api.sd_write` at the bracketing wrapper that calls it.

### 2.5.6 Makefile

Clone the lfs_rw driver block in the Makefile (`DRIVER_LFS_OBJS`, `DRIVER_LFS_CFLAGS`, `DRIVER_LFS_LDFLAGS`, the three build rules, and the `$(BUILD_DIR)/lfs_rw.bin` target) into a parallel `DRIVER_SD_RW_*` block. Add `$(BUILD_DIR)/sd_rw.bin` to the `all:` target. Add an `install` line for it in `Makefile.common` (or wherever the existing `install` target deploys `fatfs.bin` and `lfs_rw.bin` to the device's LittleFS).

### 2.5.7 Size

Driver binary expected ~400-800 B after `-flto --gc-sections`. Lives in AXI-SRAM, not in the chainloader binary — zero impact on the 600 B budget.

### 2.5.8 Verification

* Build `sd_rw.bin`; confirm well under 2 KB.
* Deploy via the `install` target to the device's LittleFS.
* Boot, navigate to SDCARD in the file browser; confirm RW options (COPY/PASTE/DELETE) now appear.
* Perform a write; confirm sector readback matches.
* Manually delete `/drivers/sd_rw.bin` from LittleFS, reboot, navigate to SDCARD; confirm RW options are hidden and the tab is read-only.

---

## 3. Phase 3 — Synthetic SDCARD Partition Entry

### 3.1 Goal

A single partition row, labelled `SDCARD`, is produced and inserted into the partition list whenever the device has SD enabled. We do not enumerate MBR partitions; we treat the whole card as one volume. If the boot sector is unreadable or has no `0x55 0xAA` signature, the row's type is set to `UNKNOWN` and downstream code ignores it.

### 3.2 Where it plugs in

`src/chainloader/storage/partition.c`:

* Add a final scan stage `STATE_SCAN_SD` after `STATE_GAP_EXT`, before `STATE_COMPLETE`.
* In that stage, run a single SD probe:
  1. `switch_ospi_to_sd()`.
  2. `sdcard_init()` → if false, skip (no SD mod or no card). Goto `STATE_COMPLETE` via OSPI restore.
  3. `sdcard_read_sector(0, buf)` into a stack-local 512 B buffer.
  4. `switch_sd_to_ospi()` immediately — we want OSPI back online before anything else runs.
  5. Inspect `buf[510]==0x55 && buf[511]==0xAA`.
* Append exactly one synthetic partition:
  * `address` = `0xC0000000` (a sentinel that's outside all valid memory regions; the partition viewer's "is_internal/is_external" branches treat anything not in `0x08…` or `0x90…` as the SDCARD bucket).
  * `size` = 0 at synthesis time. Capacity is unknown until FatFs mounts the volume (the host doesn't carry the CSD parser — `sdcard_get_sector_count` lives nowhere in the chainloader). The file browser's existing `statfs` query after mount fills in real Free/Used/Total numbers, which is the only place capacity is actually shown to the user.
  * `type` = `"FAT"` if boot signature was valid (FatFs will figure out FAT12/16/32 vs exFAT itself); else `"UNKNOWN"`.
  * `details` = `"PRESENT"` when init succeeded and signature was valid; `"NO CARD"` when init failed; `"NO FS"` when init succeeded but signature was bad.

### 3.3 Stack-local 512 B buffer

The current partition scan keeps state in a static struct; adding a 512 B local in `partition_scan_update`'s SD branch is a one-shot — exits before any other path consumes the same stack space. Cleaner than a static buffer that lives forever.

### 3.4 Hard rule: SD failure cannot poison OSPI

The CLAUDE.md "Boot Path is Inviolable" rule applies here. Any code path that can leave OSPI deinitialised, or worse, leave the pins in SoftSPI mode while the rest of the app expects memory-mapped flash, is a critical bug. The pattern in Phase 1.4 must be obeyed unconditionally; every early-return inside the SD probe must restore OSPI first.

### 3.5 Verification

* Run on a console with no SD mod: scan completes, no SDCARD row appears.
* Console with SD mod, no card inserted: SDCARD row appears with type `UNKNOWN`, details `NO CARD`. OSPI partitions on the same scan still appear correctly.
* Console with SD mod, FAT32 card: row shows `FAT`, details with capacity.
* Console with SD mod, blank card: row shows `UNKNOWN`, details `NO FS`.

---

## 4. Phase 4 — Partition Viewer Integration

### 4.1 Goal

The partition viewer gains a third section divider, `-SDCARD-`, listing the synthetic SD partition. ERASE is not offered on it (out of scope; FORMAT is a separate future task).

### 4.2 Changes to `src/chainloader/ui/partition_viewer.c`

* `rebuild_virtual_list()` already buckets by `address` range. Add a third bucket: `address >= 0xC0000000` → SDCARD divider `-SDCARD-`.
* `partition_get_label()` currently renders `"BANK1 FAT"` / `"EXT FAT"`. Extend with the third arm: `address >= 0xC0000000` → `"SDCARD <type>"` (e.g. `"SDCARD FAT"` or `"SDCARD UNKNOWN"`).
* `partition_on_action()`: keep the ERASE block-out. The existing guard `address != 0x08000000 && type != "FREE"` opens the context menu; extend to also skip `address >= 0xC0000000` so no ERASE option appears on SD.
* `partition_draw_right_pane()`: for the SD row, render `ADDR.` as `-` and `SIZE.` as `-` (capacity is unknown until mount; this is shown in the file browser via `statfs`, not here). `DETAILS.` shows `PRESENT` / `NO CARD` / `NO FS` as set in Phase 3.

### 4.3 Size

Pure conditional-arm work in already-linked functions, predominantly small literal additions. Estimated < 60 B.

### 4.4 Verification

* On a device with no SD mod: viewer shows INTFLASH + EXTFLASH only.
* On a device with SD mod: viewer shows the third section. Selecting the SDCARD row does **not** offer ERASE.

---

## 5. Phase 5 — File Browser Integration

### 5.1 Goal

When the user selects the SDCARD row in the file browser's FS list, the dynamic `fatfs.bin` is loaded (if not yet) and mounted in SD mode. The user can navigate, copy from / paste to / delete on the card. The browser's tab system continues to work — you can have the SD on one tab and a LittleFS partition on the other.

### 5.2 Changes to `src/chainloader/ui/ui_file_browser.c`

* **FS list build (`build_fs_list`)** — Add SDCARD partition to the list with the same FAT-style probing block as the OSPI FAT case, but the mount call uses `(0, 0)` sentinel. `fs_is_rw[SD]` follows the existing LittleFS pattern: probe LittleFS once for the presence of `/drivers/sd_rw.bin`. Present → RW; absent → RO. `statfs` populates Free/Used/Total after mount.
* **`mount_tab_partition`** — Already dispatches on `type[0]/type[1]`. Recognise the SDCARD address sentinel to choose `(0, 0)` mount arguments; otherwise everything else is unchanged.
* **`fs_list_get_label`** — Currently emits `"FAT @ 0x90400000"` etc. For SD card, emit `"SDCARD"` (no address suffix, since `0xC0000000` is fake).
* **`update_browser_title`** — Currently emits `"FAT/some/dir"`. For SD, emit `"SDCARD/some/dir"` so the user knows which device they're on.
* **Tab switching** — Already unmounts on tab change. SD unmount path must call `g_host`'s SD bracketing through the driver-side `unmount()`. Critically: while OSPI is being switched off, no other code can be running flash reads — we're already on the main UI thread so this is fine in practice, but document the constraint.

### 5.3 Driver loading

`fatfs.bin` continues to be lazy-loaded via `vfs_load_dynamic_driver("FAT", "/drivers/fs/fatfs.bin")` exactly as today. **No new loader logic needed for read.**

For write: `vfs_load_sd_rw_driver()` (Phase 2.5) is called lazily inside `perform_paste` and `perform_delete` when the destination/target is the SD card. If it returns false (driver missing on LittleFS), the UI shows `SD WRITE UNAVAILABLE` and aborts before any disk I/O. In practice `fs_is_rw[SD]` already gates the COPY/PASTE/DELETE menu options so the user never reaches this error path unless the driver file is removed between FS-list build and operation execution.

### 5.4 The crucial mode-switch invariant

When the SD card is mounted, the rest of the chainloader cannot read OSPI — OSPI pins are in SoftSPI mode. This means:

* The active theme assets are already cached/copied in AXI-SRAM at boot, so the menu renders fine.
* The other tab cannot be a LittleFS or FrogFS partition that's *mid-read* while SD is active. The existing tab switch unmount → switch → mount sequence already enforces this serially.
* Background partition scans must not run while SD is mounted. The browser only runs partition scan in `BROWSER_MODE_SCANNING`, which is before any mount happens. Safe.

### 5.5 Size

Mostly conditional arms in functions already linked; new code is small. Estimated 80–120 B.

### 5.6 Verification

* Without SD mod: FS list shows only OSPI partitions as today.
* With SD mod + FAT32 card containing test files: enter SDCARD tab, navigate directories, open a file (verify in-place display), copy a file from LittleFS to SD, copy a file from SD to LittleFS, delete a file on SD.
* Tab-switch SD↔LittleFS: confirm OSPI lives both ways with no corruption (check via partition viewer after switching: LittleFS contents intact, capacity numbers match).

---

## 6. Phase 6 — Safety Rails

### 6.1 Block the power button during operations

Currently in `ui_manager.c`:

```c
if (input_just_pressed(INPUT_PWR)) {
    menu_enter_standby();
}
```

This fires unconditionally. Gate it:

```c
if (input_just_pressed(INPUT_PWR) && !ui_operation_in_progress) {
    menu_enter_standby();
}
```

`ui_operation_in_progress` is already set by `perform_paste`. Extend the same bracketing to `perform_delete` and to any future partition-affecting operations (we already have it on flash operations through `update_progress_ui` paths, but `perform_delete` currently does not raise the flag — fix this in the same patch). This is a project-wide hardening, not SD-specific.

### 6.2 Block hardware-reset paths

The OFW recovery hook lives in the patched OFW, not the chainloader. We can't gate it from here. But we *can* show a confirm modal that warns "DO NOT POWER OFF — operation in progress" by adding a high-contrast banner whenever `ui_operation_in_progress` is true. Tiny implementation: in `ui_draw()`, after the active window draws, if the flag is set, draw a one-line red banner across the bottom of the screen. ~20-30 B.

### 6.3 Space-check before paste

In `perform_paste` (`ui_file_browser.c`):

* After resolving `dst_drv`, call `dst_drv->statfs(&dst_total, &dst_free)`.
* If `dst_free < copy_src_size`, abort with `ui_show_error("NOT ENOUGH SPACE")` and return before opening the destination file.
* If `statfs` is not available on the destination driver (NULL pointer), allow the paste but log a warning to the on-screen debug area in dev builds. In release, treat NULL statfs as failure to be safe (per user's "prioritise blocking dangerous actions" instruction).

The check happens *before* opening `f_dst` so we don't create a zero-length file that then fails partway through.

### 6.4 Delete confirmation extension

`perform_delete` already runs behind `ui_show_confirm("DELETE FILE?", ...)`. Wrap the call to `drv->unlink` with `ui_operation_in_progress = true/false`, same shape as paste, so the power button is gated and the warning banner shows. Tiny add.

### 6.5 Mid-operation abort handling

`perform_paste` already polls `INPUT_PWR` / `INPUT_B` inside the copy loop and cleanly unlinks the partial destination on abort. Verify this path still works when the destination is SD (it should — same VFS API).

### 6.6 Size

~80 B for the gates + banner + statfs check.

### 6.7 Verification

* During a long copy to SD, press POWER — confirm nothing happens and the banner is visible.
* Attempt to paste a 100 MB file onto a card with 50 MB free — confirm `NOT ENOUGH SPACE` error appears before any write begins.
* During a delete, press POWER — confirm nothing happens.
* After every operation completes (success or abort), POWER works again.

---

## 7. Phase 7 — Documentation and Hardware Verification

### 7.1 Documentation updates (in lockstep with code)

Each phase that lands should update, in the same commit:

* **`DESIGN.md`** — Add an "SD Card Support" subsection under §8 (OSPI / external storage). Cover: pin multiplex, host/driver split, mount-mode sentinel, the OSPI/SD mutual-exclusion rule. One Overview + one Implementation block, plain language.
* **`README.md`** — One paragraph in the storage section: "if your console has the SD-card mod installed, the launcher recognises it and lets you browse it like any other partition." No register addresses or jargon.
* **`ACTIVE_WORK.md`** — Add Goal 13 "SD-card mod support" with a checklist mirroring Phases 1–6, and a Debugging block once items are touched.
* **`CHANGELOG.md`** — One terse bullet per landed phase, newest first, with the commit hash.

### 7.2 Hardware test matrix

Final acceptance requires every cell of this matrix verified:

|                        | No SD mod          | SD mod, no card    | SD mod, blank card | SD + FAT32, no `sd_rw.bin` | SD + FAT32 + `sd_rw.bin` | SD + exFAT + `sd_rw.bin` |
| ---------------------- | ------------------ | ------------------ | ------------------ | -------------------------- | ------------------------ | ------------------------ |
| Cold boot              | works              | works              | works              | works                      | works                    | works                    |
| Partition viewer       | no SDCARD row      | row: UNKNOWN / NO CARD | row: UNKNOWN / NO FS | row: FAT / PRESENT      | row: FAT / PRESENT       | row: FAT / PRESENT       |
| File browser FS list   | as today           | no SDCARD entry    | no SDCARD entry    | SDCARD entry (RO)          | SDCARD entry (RW)        | SDCARD entry (RW)        |
| Browse SD              | n/a                | n/a                | n/a                | navigate dirs              | navigate dirs            | navigate dirs            |
| Copy file ← SD         | n/a                | n/a                | n/a                | success                    | success                  | success                  |
| COPY menu offered on SD| n/a                | n/a                | n/a                | yes (source)               | yes                      | yes                      |
| PASTE / DELETE on SD   | n/a                | n/a                | n/a                | **hidden** (RO)            | success                  | success                  |
| Tab switch SD↔OSPI     | n/a                | n/a                | n/a                | no corruption              | no corruption            | no corruption            |
| OSPI partitions intact after SD use | n/a   | yes                | yes                | yes                        | yes                      | yes                      |
| POWER during op        | standby            | standby            | standby            | standby (no op possible)   | gated                    | gated                    |
| Paste no-space         | n/a                | n/a                | n/a                | n/a                        | NOT ENOUGH SPACE         | NOT ENOUGH SPACE         |

### 7.3 Binary size budget tracking

At each phase boundary, log the size delta into ACTIVE_WORK.md's debugging log, so we know how much headroom remains. If the host overshoots the 600 B budget, candidates to cut are (in order of preference):

1. **Move more code into `sd_rw.bin`** — anything called only during write paths is already there; check if anything called only during the post-init OCR validation or the partition-synthesis probe can be deferred behind a driver load. The driver lives in AXI-SRAM and is essentially free.
2. **Inline `sd_xchg` at all call sites** to remove the function-call overhead — costs the driver duplication of a small primitive but cuts host calls. Only worth doing if `host_api_t.sd_xchg` is no longer needed (i.e. the driver carries its own SoftSPI).
3. **Drop the partition-synthesis probe sector-0 read** — accept all detected cards as `FAT` and let mount failure mark them `UNKNOWN` retroactively. Saves the sector-0 read code path entirely.
4. **Final fallback**: drop SD support from this release. RO baseline is already the floor of the design; there is no smaller useful configuration. The codebase is left in a state where the next attempt resumes from Phase 1 with whatever space has been freed elsewhere.

---

## 8. Out of Scope (Explicitly)

The following are deliberately not part of this work and need their own plan if/when desired:

* MBR multi-partition enumeration on the SD card. We treat the whole card as one FAT volume mounted at the start.
* FORMAT operation for SD card. ERASE is hidden from the partition viewer for SD.
* SDHC card-detect pin / SDIO interface. The pin-tap mod is the only target.
* Hot-insert/eject detection. Card presence is sampled once at scan time. User must re-enter the partition viewer / file browser to re-scan.
* Writable LFN/codepage changes. FatFs config in `ffconf.h` stays untouched.
* Stub-side SD knowledge (per user instruction, never).

---

## 9. Reading Order for the Implementer

If a fresh contributor (or future-Claude) picks this up, read in this order:

1. CLAUDE.md (workflow & engineering rules) — already required first.
2. DESIGN.md §5 (boot flow & module map), §8 (OSPI init), §6 (bank swap) — context for how OSPI sits in the boot path.
3. This file.
4. `example projects/game-and-watch-bootloader/Core/Src/gw_sdcard.c` and `softspi.c` — the reference for pin-tap and OSPI/SD multiplex.
5. `src/chainloader/storage/vfs.{c,h}` and `src/drivers/fs/fatfs/driver_entry.c` — the existing host-API / dynamic-driver contract.
6. `src/drivers/fs/lfs_rw/` — the closest precedent for `sd_rw.bin`'s structure (driver_entry.c + linker script + Makefile rules).
7. `src/chainloader/storage/lfs_wrapper.c` — pattern for OSPI memory-mapped toggle plus DCache bracketing.

Only after that should code be written.

# PIE Module Loader — Technical Implementation Checklist

Deep technical tracking for the position-independent (PIE) module loader and the
RO-in-core / RW-as-modules filesystem restructure. This document holds the *granular*
technical state; [ACTIVE_WORK.md](../ACTIVE_WORK.md) holds the overarching goals/tasks.

**Status:** Merged to `main` (commit `693c6c7`). The PIE module loader and the RO-in-core / RW-as-modules split shipped; this document is retained as the implementation record. The live loader uses a bump allocator over the module pool at `MODULE_POOL_BASE = 0x240C0000` (256 KiB); the earlier `0x240A0000` addresses noted below are historical.

**End goal context:** this is the foundation for SD-card support (Goal 13). The address-
independent loader + RO-in-core/RW-as-modules split is what lets filesystem functionality load
from external flash or SD on demand. Keep that in mind for every FS-handling change.

A box is checked **only when built *and* verified on hardware** (a clean build is not proof).

---

## 0. Foundation
- [x] Branch `pie-module-loader` created off `main`; prior WIP preserved on `sdcard-restructure-wip`.
- [x] Flash ceiling raised 32K → **40K** (8K-sector aligned, 5 sectors) — build passes at 32,224 B, 8,736 B free:
  - [x] `STM32H7B0_FLASH_STUB.ld` `FLASH … LENGTH = 40K`
  - [x] `src/common/memory_map.h` `RETROGO_BASE = 0x0800A000`
  - [x] `Makefile.common` `RG_INTFLASH_ADDRESS = 0x0800A000`, `RG_FLASH_LENGTH = 216k`
  - [x] `Makefile` post-build size check raised 32K → 40K.
  - [ ] Retro-Go rebuilt + reflashed at `0x0800A000`. *(hardware)*

## 1. Module format (PIC, GOT-relocated, no r9)
- [x] Module ABI header `module_header_t` { magic `'GMOD'`, entry_offset, reloc_offset, reloc_count, bss_offset, bss_size, version, `abi`, `flags` } + `MODULE_MAGIC` + `MODULE_HEADER` macro. — `src/chainloader/system/module.h`
- [x] **ABI gating:** `mod_load_image` + the multi-FS `vfs_read_module` reject any module whose `abi != MODULE_ABI_VERSION` (in-core, recovery-critical), via shared `src/common/abi.h` `module_abi_ok()`. `flags` bit `MOD_FLAG_TRANSIENT` = load-model (resident vs load-then-`mod_pool_reset`). Host-tested (`scripts/build/test_abi_gate.c`) + on-device (`scripts/tests/test_abi_reject.py`, 6/6). — `loader.c`, `storage/vfs.c`
- [x] `src/modules/module.ld`: ORIGIN 0; `.module_header` first; single contiguous image; `.rel.dyn` kept in-image; exports header symbols + `init_module` entry.
- [x] Module CFLAGS `-fPIC` (NOT `-msingle-pic-base`); LDFLAGS `-pie -T module.ld` (kept `nano.specs` libc — its mem*/str* are leaf/position-independent; no bad relocs).
- [x] **`readelf` validated** on `lfs_rw.elf`: only **5 × `R_ARM_RELATIVE`**, all at offsets `0x04–0x14` (header fields → loader skips them); **zero body relocations** (`.data` empty, code fully PC-relative); `.module_header` first; `.bss` NOBITS; no `.dynamic`.

## 2. Generic loader (`src/chainloader/system/loader.{c,h}`) — separate from VFS
- [ ] `mod_load(path, out_handle)`: read `.bin` via in-core RO reader.
- [ ] Bump allocator over a module RAM pool (AXI-SRAM, e.g. `0x240A0000–0x24100000`); aligned next-free; reset/free between ops. Replaces hardcoded `0x240A0000`/`0x240C0000`.
- [ ] Relocation loop: apply **every** `R_ARM_RELATIVE` with offset ≥ `sizeof(header)` (`*(slot+off) += slot`); skip header region.
- [ ] Zero `.bss`; `SCB_CleanDCache_by_Addr` + `SCB_InvalidateICache` over the slot.
- [ ] Call `init_module` at `slot + entry_offset` with host vtable + `host_api`.

## 3. VFS = filesystem registry only
- [ ] `vfs.c`/`vfs.h`: keep registry + `host_api`; **delegate module loading to `loader.c`** (remove embedded loader from `vfs_load_dynamic_driver`).
- [ ] In-core RO drivers registered at init.
- [ ] RW module registers a `vfs_driver_t` that overrides the RO one (reverse-order `vfs_get_driver`).

## 4. LittleFS end-to-end (the proof)
- [ ] In-core RO `lfs_ro` (reuse `lfs_wrapper.c`, `-DLFS_READONLY`) — browsing + reading module `.bin`s.
- [ ] `lfs_rw` converted to a PIE module (header stub + `module.ld` + `-fPIC`).
- [ ] `ui_file_browser.c`: browse via `lfs_ro`; on write/delete, `mod_load` `lfs_rw`.
- [ ] **On hardware:** browse LittleFS lists files + correct Free/Used.
- [ ] **On hardware:** write/delete loads `lfs_rw` at a bump-allocated addr (verify via `memory.py` it is NOT hardcoded) and succeeds.

## 5. fatfs_ro in-core (fit-permitting)
- [ ] Add in-core `fatfs_ro` (FatFs RO, `FF_FS_READONLY=1 FF_FS_EXFAT=0`).
- [ ] Fit-check at 40K. If it does not fit → fall back to `fatfs_ro` as a PIE module loaded from extflash.
- [ ] (Future) `fatfs_rw`/exFAT PIE module — structure ready, not wired.

## 6. FrogFS
- [ ] Scanner keeps recognition (type + size).
- [ ] Minimal in-core lister (opendir/readdir of names, NO read) if it fits; else recognition-only.

## 7. Theme — EVALUATE (gated, do not pre-commit)
- [ ] Measure in-core theme engine code size vs. offload scaffolding.
- [ ] Decision: theme stays in-core, OR becomes a module — **only if** clearly net-positive AND the early-boot-before-FS-mount timing is cleanly solvable. Otherwise STOP and report a deeper-simplification proposal.

## 8. Verification / device recovery
- [ ] `make clean && make -j16`; core ≤ 40K.
- [ ] Device recovered: `trace.py reset-halt` → `make flash_chainloader`; boots to launcher.
- [ ] Modules deployed to `/modules/filesystems/` on extflash littlefs.
- [ ] Two modules loaded concurrently → no address collision.
- [ ] (Optional) port `trace.py fault` subcommand from `sdcard-restructure-wip`.

---

## Notes / decisions log
- **Module format validated (the key de-risk).** `lfs_rw` built `-fPIC -pie -Tmodule.ld`: `readelf -r` → only 5 `R_ARM_RELATIVE`, all in the header (`0x04–0x14`); **zero body relocations** (`.data` is 0 bytes, code is fully PC-relative, GOT has only reserved slots). So the module runs at any load address with no body relocation; the loader reads the header as offsets, zeroes `.bss`, calls the entry. All three WIP failure modes (static vtable `.data` relocs, GOT-only relocation, `r9`) are designed out: `main`'s `init_module`-fills-host-vtable ABI = no static vtable, and no `-msingle-pic-base` = no `r9`.
- **Loader code complete + builds:** `system/loader.c` (bump allocator over `0x240A0000–0x24100000`, relocation loop skipping header, `.bss` zero, cache maintenance, entry call); `system/loader.h`. VFS refactored: `vfs_load_dynamic_driver` now delegates to `mod_load`; module file-read extracted to `vfs_read_whole`. In-core RO LittleFS (`lfs_wrapper.c`) retained as bootstrap reader. **Pending hardware verification.**
- **Still on hardware:** flash this build (recovers device from the WIP crash-loop), reflash Retro-Go at `0x0800A000`, browse LittleFS, write/delete (loads `lfs_rw` PIE module at a bump-allocated addr — confirm via `memory.py`).

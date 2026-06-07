# Safe FAT compaction + Stage 3 loose ends

Tracks the one remaining open item of the tiered module-memory work (the
power-safe FAT compactor) and the loose ends tied to the FAT module store.
Overview of the store + XIP is in `docs/memory-architecture.md`; the module API
that rides on it is `docs/module-api.md`.

## Where Stage 3 stands

Functionally complete and hardware-verified: the EXT-FAT module store at
`0x90540000`, in-place XIP of r9-pic modules from it (mp3 + Module Overview XIP
from the store; picture is RAM-only, its decode is latency-sensitive), the
generalized install path (any source SD/LFS/FAT to FAT-XIP-or-LFS, highest
version wins, migrate, delete the source iff it is RW), fonts deployed to LFS
(Latin whole-loads to RAM, CJK pages), and the LFN store enum (the full FAT-RW
driver is brought up before `feature_discover`, so the store is enumerated with
LFN instead of the in-core 8.3 reader). The one open feature is the power-safe
compactor; the rest below are smaller loose ends.

## 3d: power-safe FAT compactor (the main open item)

A FAT store fills with holes as modules are installed and removed. FatFs has no
defragmenter, so an install that needs a contiguous run (for XIP) can fail to
find one even when total free space is sufficient. The compactor coalesces free
space by moving clusters.

This is a DESTRUCTIVE write to the RECOVERY-CRITICAL store (it holds the bootstrap
drivers under `/fs/`, the modules, and the fonts). STABILITY IS LAW: a reset mid
move must never corrupt the store, and the menu must always remain reachable.
Trace every failure mode before writing a byte.

- v1 already in place (graceful degradation): `f_expand` is best-effort, so when
  no contiguous run is free, the install simply lands non-contiguous and the
  loader falls back to a full RAM copy (XIP just does not engage; nothing breaks).
- The real work (transactional defragmenter):
  - Trigger only when an install needs a contiguous run and none is free.
  - Move clusters to coalesce free space using commit-last ordering: write the
    moved copy to free space first, fsync, flip the directory entry to point at
    the new location LAST, and only then reclaim the old run.
  - A reset mid-move therefore leaves either the old copy intact (entry not yet
    flipped) or both copies (entry flipped, old not yet reclaimed); a dirty-flag
    boot scan reclaims the orphan in the latter case. Set the dirty flag before
    the move, clear it after the reclaim.
  - Keep an X% reserve so a move always has scratch room; never fill the store.
  - Gate on battery (>= ~15%), blocking + safe-fail, RO-mounted by default, write
    only on a deliberate install (same guards as the install path).
- Verify on hardware: install/remove churn that forces a compaction, power-cut
  mid-move (pull the reset) then confirm the boot scan recovers and the store +
  the Retro-Go round trip survive, and that the menu is reachable throughout.

## Loose ends tied to the FAT / memory restructuring

1. **Full LFN map.** Discovery now enumerates with the LFN driver, but the XIP
   MAP (`vfs_map_file` / `vfs_map_module`) still resolves through the in-core 8.3
   RO reader (`fat_ro_map`), because it needs the file's contiguous mapped-flash
   address and the streaming FAT-RW driver does not expose that. So a long-named
   XIP file would enumerate but fail to MAP. All current names are 8.3-clean, so
   nothing is broken, but to fully drop the 8.3 build guard in `make_fat_image`,
   `fat.bin` needs a "resolve path -> contiguous flash extent" call that
   `vfs_map_file` routes through. Until then, every XIP-mapped, discovered, or
   `/fs/` bootstrap file MUST be 8.3.

2. **Latin-RAM enforcement.** The font design is: the Latin base font is ALWAYS
   RAM-resident (hot path; must survive store writes), CJK is paged or FAT-pulled.
   Today `open_slot` tries store-XIP first, so if `latin.fnt` is in the store the
   base would XIP rather than RAM-load. The fonts currently live on LFS (so the
   base RAM-loads, CJK pages, the no-FAT path), which is correct, but
   `make_fat_image`'s FONTS loop also injects the fonts into the store. To
   strictly guarantee Latin-in-RAM regardless of provisioning, either exclude
   `latin.fnt` from the store (LFS only) or gate `open_slot(SLOT_BASE)` to skip
   the store-XIP attempt.

3. **Store provisioning cleanup.** `make_fat_image` injects the `/fs/` bootstrap
   drivers + the resident modules (theme/language/installer/fileops) + the XIP
   feature apps (mp3, modview) + fonts. Per the "only XIP apps + fonts in the FAT
   store" rule, the non-XIP resident modules are redundant in the store (they load
   from LFS after `lfs.bin` bootstraps), so removing them frees store space. Keep
   the `/fs/` drivers (the bootstrap needs `lfs.bin` there) and verify the boot
   chain (in-core RO FAT reads `/fs/lfs.bin` from the store -> mount LFS -> load
   the resident modules from LFS) before removing them.

4. **Fonts-XIP reclaim (~36 KB).** The whole-load font buffers already shrank
   ~16 KB back to the pool. The remaining reclaim is to drive the resident
   language module footprint down (~48 KB toward ~12 KB) by XIP-ing the CJK glyph
   data from the store rather than paging from LFS when the store has it. Measure
   the resident module footprint after.

5. **Warm-reset SD install skip** (pre-existing): the SD->LFS language install
   scan runs every boot when `/i18n` is on the SD; skip it on a warm Retro-Go
   return for a snappier quit-to-menu.

## SWD debug for this area

- `g_feat_dbg` (feature discovery: enum/registered/reject counters + last_name).
- `g_mod_recs` (loader registry: path, flash=XIP-addr-or-0, ram, flags, reason
  1=XIP-from-store / 2=RAM-not-in-store / 3=RAM-XIP-declined).
- `g_boot_step` (boot bring-up markers, including the hand-rolled HAL init steps).
- `scripts/debug/fat_contig.py` (host-image contiguity check on `build/fat.bin`).

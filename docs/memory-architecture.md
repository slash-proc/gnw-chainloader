# Module Memory Architecture

This document records how the chainloader partitions memory for its loadable modules, why it is laid
out this way, and the staged plan that built it. DESIGN.md §1 holds the authoritative address map;
this file expands on the reasoning and the design of the work that is planned but not yet shipped.

## The problem this solves

Feature modules and the resident driver, theme, and language modules all load into one pool in
AXI-SRAM. When the picture viewer (about 104 KB before Stage 2) loaded on top of the resident
modules (about 91 KB), it overflowed the 192 KB usable pool and failed with "Out of module memory".

That was never a real shortage of memory. The 1 MB AXI-SRAM had a large free gap below the pool, the
128 KB D2 SRAM sat idle while the menu ran, and the 64 MB external flash had megabytes free. The pool
was simply parked in a small corner of the memory that was available. The fix is a tiered layout that
draws on each region for what it is good at, built in independently shippable stages.

## The three tiers

| Tier | Backing store | Holds | Status |
|------|---------------|-------|--------|
| 1 | AXI-SRAM pool (384 KB) | resident drivers plus the one live transient module's code, data, bss | shipped (Stage 1) |
| 2 | D2 AHB-SRAM2 (64 KB) | a transient module's big CPU-only working buffer, borrowed for its run | shipped (Stage 2) |
| 3 | OSPI flash, FAT partition | module code executed in place (XIP); only data and bss use RAM | shipped + hardware-verified (Stage 3) |

The tiers compose. A module's code and small data live in the AXI pool (Tier 1) or, once Tier 3
lands, execute in place from flash with only its data and bss in the pool. Its large transient
working buffers are borrowed from idle D2 SRAM (Tier 2) instead of bloating its footprint.

## Tier 1: the AXI module pool

The pool is a region of AXI-SRAM that a simple bump allocator (`src/chainloader/system/loader.c`)
hands out to PIE modules. Each module is relocated to its slot base, so the pool base is free to
choose. The pool reserves an address range but only consumes what is loaded, and its size is driven
by module code size, never by filesystem content.

Current AXI-SRAM layout (`0x24000000` to `0x24100000`, 1 MB):

```
0x24000000  app: code + data + bss (incl. 300 KB framebuffers) ... ~521 KB  USED
0x24082480  free gap (app / lzma headroom) ......................   ~55 KB
0x24090000  MODULE POOL  (bump-up from base) .................... 384 KB usable
0x240F0000  stack guard (UI stack descends from _estack) ........  64 KB
0x24100000  _estack
```

Stage 1 grew the pool from 192 KB to 384 KB by lowering `MODULE_POOL_BASE` from `0x240C0000` to
`0x24090000`, reclaiming the previously unused gap below it. This was safe because the boot-time LZMA
asset decompression heap is a static buffer inside the app's bss (below the gap), not in the gap
itself, and the framebuffers sit below it too. The pool is purely where modules load, so the change
did not touch the boot path. With the resident modules (about 91 KB) and the 64 KB stack guard, about
293 KB remains for the one live transient feature module. The AXI-SRAM has room to grow the pool
further if needed; it is the same one-constant change.

## Tier 2: borrowed D2 scratch

A module's large working buffer (for example the picture viewer's 64 KB PNG inflate window) does not
need to live in the module's bss, where it counts against the pool for the module's whole lifetime.
Only one transient module runs at a time, so a single shared scratch region can be lent to whichever
module is live.

The feature host vtable (`feature_host_t` in `src/chainloader/system/feature.h`) exposes:

```c
void *scratch_get(uint32_t size);
```

It returns the base of a 64 KB region in **D2 AHB-SRAM2** (`0x30010000`, `D2_SCRATCH_BASE` in
`memory_map.h`) for any size up to the bank, or NULL for a larger request. There is no allocator and
no free: the region is exclusive to the running module and is reclaimed implicitly when it returns.

Two facts make this safe:

- **D2-2 is idle while the launcher menu runs.** It is the bank the OFW patch uses, and the OFW patch
  only executes during an actual OFW boot, never in the menu. Fastcap (the live framebuffer capture)
  uses the other bank, D2-1 (`0x30000000`), and only during a live SWD capture. So a transient module
  in the menu can borrow D2-2 without colliding with either. The D2 SRAM clock is enabled at
  SystemInit, so no clock handling is needed in `scratch_get`.
- **D2 is for CPU access only, never DMA.** The SAI1 audio path uses `DMA1_Stream0`, which is on the
  AXI interconnect and cannot reach the D2 domain. So D2 scratch is correct for CPU-driven working
  buffers (decode windows, inflate scratch) but a DMA buffer, such as the future video player's audio
  ring, must stay in AXI. The API comment states this so no one parks a DMA buffer in D2.

D2 is uncached, which makes CPU-heavy buffer access slower than AXI, but correct. For the picture
viewer that tradeoff is acceptable; the decode is not real-time. Stage 2 moved the picture viewer's
64 KB JPEG and PNG scratch union out of bss to `scratch_get`, dropping the picture module footprint
from 104 KB to 38 KB.

## Tier 3: XIP modules from a FAT partition (shipped, hardware-verified)

> Stage 3 is built and verified on hardware. The design below is what shipped; the
> as-built deltas (the r9-pic XIP mechanism, the LFN store enum, the generalized
> install path, the font reality, and what compaction actually does today) are in
> **As built** at the end, and the one open item, the power-safe compactor, is in
> `docs/fat-compaction.md`.

The scalable tier runs the code of modules that are not latency sensitive **in place from external
flash** (execute in place, XIP), so only their data and bss use RAM. This is for the growing number
of tools and UI modules and for user-added modules whose count is unknown in advance. Latency
sensitive code (the JPEG decode loop, the MP3 and audio path, a future video frame path) stays
RAM-resident, flagged per module.

### The store: FAT plus f_expand

The module store is a **FAT partition** on the external flash whose module `.bin` files are allocated
contiguously with FatFs `f_expand`. A contiguous file on the memory-mapped flash has a single fixed
mapped address, so each `f_expand`'d `.bin` is its own XIP image. There is no separate raw partition,
no extraction step, and no custom allocator: `f_expand` is the allocator and the FAT directory is the
manifest. This reuses the FatFs the project already ships.

FAT was chosen over a raw partition because a raw blob has no directory of its own. A raw store would
need an external index, and losing that index means scanning or guessing. FAT's directory is the
index, for free. Raw storage remains the right choice only for write-once, fixed-location data such
as the OFW asset blocks, where there is nothing to manage.

### Power safety: FAT for code, LFS for mutable state

FAT is not power-loss safe the way LFS is. The design keeps that exposure away from anything that
must survive a power cut:

- **FAT holds read-mostly, re-installable data**: module code and assets. XIP reads never write.
  Installs are deliberate and re-runnable, so a power cut during one corrupts at most that install.
- **LFS holds power-sensitive mutable state**: configs and game saves, written through the loaded
  LFS-RW driver. LFS copy-on-write keeps those safe across a power cut.

Write guards on the FAT partition bound the remaining risk to the brief, deliberate install window:
mounted read-only by default; writes only on a deliberate install, gated on battery at or above about
15 percent, blocking, and failing safely; the directory entry committed last so a power cut leaves
orphaned clusters (recoverable) rather than a corrupt directory; and a dirty-flag boot scan that
reclaims orphans after an interrupted write.

### The split-segment loader (two base relocation)

Today the loader relocates a module to one base. XIP needs two: `.text` and `.rodata` execute from
the OSPI mapped address, while `.data`, `.got`, and `.bss` live in RAM. A new module linker variant
links the two segment groups for their two homes, `module_header_t` marks which relocations take the
text base versus the data base, and the loader applies both. PIE keeps `.text` position independent
so it runs at any mapped slot, and the RAM GOT is re-patched per load. This changes the module ABI
contract, so it bumps `MODULE_ABI_VERSION` and rebuilds every module.

### Install and load

Install writes the `.bin` `f_expand`'d into the FAT partition using the established sequence of
exiting OSPI memory-mapped mode, programming, and re-entering it (the pattern in
`storage/lfs_wrapper.c`). Load looks the file up, takes its contiguous mapped base, XIPs `.text` in
place, relocates only `.data` and `.bss` into RAM, and locks the extent so a future compactor cannot
move it mid-run.

A write and an XIP read of the flash cannot overlap in time. Exiting memory-mapped mode to program
darkens the whole `0x90000000` window, so anything executing or reading from it at that instant reads
garbage. This is naturally satisfied: installs run from the menu, where no feature module is
executing, and the menu pauses its draw for the brief write. RAM-resident code is unaffected because
it is not on that bus, which is also why fonts may stay paged into RAM (see below).

### Compaction

Adds use `f_expand` and are contiguous as long as a free run exists. Removes leave holes. A shuffle is
only needed when accumulated holes block a new contiguous allocation. FatFs has no built-in
defragmenter, so that compaction is the project's to write: a power-safe transactional move (copy to
fresh reserve, commit a single pointer atomically, roll an interrupted move forward or back at boot)
with a small free reserve. The first version simply errors or evicts the least-recently-used module
when no contiguous run is free; the power-safe compactor is a later upgrade. The delivered partition
is pre-aligned (a host tool lays the factory modules down contiguously), so compaction is a runtime
churn concern only.

### Reslice of the external flash

Stage 3 moves the two 128 KB OFW backups from their current offsets down behind the 5 MB asset
blocks, freeing about 2.75 MB for the FAT partition. The FAT partition stays below `EXTFLASH_OFFSET`
(8 MB, asserted at build time) so Retro-Go, which owns 8 MB and up, never sees it. The core keeps one
in-core read-only FAT reader that serves both the SD card and this partition; LFS demotes from an
in-core bootstrap to a loaded driver. The runtime partition scanner is signature based, so it keeps
working with any combination of filesystems present, or none.

### Bonus: fonts XIP

The resident language module spends about 36 KB on the font pager (slot buffers plus a paged-glyph LRU
cache in `src/modules/language/font_ext.c`). XIP'ing the `.fnt` files from a contiguous OSPI region
lets the glyph blitter read codepoints and bitmaps straight from the mapped flash, with no slot
buffers, no paging cache, and no per-glyph filesystem seeks. That hands most of the 36 KB back to the
pool. The catch is that the menu's text rendering is itself a constant flash reader, so an install
(which darkens the flash) would have to pause rendering. If that coupling proves awkward the fonts
stay paged into RAM (which tolerates the write window) and only modules go XIP. The decision is
deferred to when this is built.

## As built (what shipped, and the one open item)

- **Stage 1** (AXI pool 192 to 384 KB): shipped to `main`.
- **Stage 2** (borrowed D2 scratch): shipped.
- **Stage 3** (FAT + `f_expand` XIP store, two-base loader, install + load, fonts):
  shipped and hardware-verified per sub-phase. The details the design above does
  not spell out:
  - **XIP gating is r9-pic, not plain PIE.** A `-fPIC` module bus-faults when
    XIP'd: its PC-relative GOT assumes `.text`/`.got` are contiguous, which XIP
    breaks. XIP-able modules are built `-msingle-pic-base -mpic-register=r9
    -mno-pic-data-is-text-relative`; the loader sets r9 to the module's GOT base
    (`data_base`) at every entry and the app is `-ffixed-r9` so r9 survives
    core to module to callback crossings. Only transient r9-pic modules XIP from
    the store (mp3, Module Overview); resident and `-fPIC` modules full-copy.
    Picture is deliberately RAM-only (its JPEG/PNG decode is latency-sensitive).
  - **The store is enumerated with LFN.** The full FAT-RW driver (`/fs/fat.bin`)
    is brought up before `feature_discover`, so `/modules/features` is read with
    the LFN driver rather than the in-core 8.3 reader (which mishandles the LFN
    directory entries pyfatfs writes and silently drops a second store entry). The
    8.3 constraint still applies to the XIP MAP: `vfs_map_file` needs a file's
    contiguous mapped-flash address, which only the in-core reader returns, so
    every XIP-mapped / discovered / `/fs/` bootstrap file must be 8.3 until
    `fat.bin` exposes a flash-extent call (see `docs/fat-compaction.md`).
  - **The install path is generalized.** Any source (SD / LFS / FAT) installs to
    FAT if the module is XIP-able, else LFS; highest version wins; with migration
    and source deletion iff the source is RW.
  - **Fonts ship on LFS:** the Latin base whole-loads to RAM (hot path), CJK pages
    glyph-by-glyph from LFS (or FAT-pulls from the store when present); the
    whole-load buffers shed ~16 KB back to the pool. The further ~36 KB fonts-XIP
    reclaim is a loose end (`docs/fat-compaction.md`).
  - **Compaction today is graceful degradation, not error/evict:** `f_expand` is
    best-effort, so an install with no contiguous run simply lands non-contiguous
    and the loader full-copies it (XIP just does not engage). The power-safe
    transactional compactor is the one open Stage-3 item; its design and the other
    loose ends are in `docs/fat-compaction.md`.

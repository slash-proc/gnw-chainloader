# Size optimization (the 40 KB chainloader)

How the chainloader's compressed size is structured, what governs it, and the levers
that actually move it. Written after a whole-core size pass that took Free space from
76 bytes to 1948 bytes (see Results). The headline lesson: the binding stage is LZMA,
so the biggest wins come from feeding LZMA better input, not from hand-trimming source.

## 1. What "Free space" measures

The 40 KB region is the stub flash area (`STM32H7B0_FLASH_STUB.ld`). It holds two
things: the stub code (uncompressed) and the LZMA-compressed RAM-app blob linked into
the stub (`app.bin.lzma.o`). At boot the stub decompresses the app into AXI-SRAM and
jumps to it.

    Free space = 40960 - stub_code - compressed_app

The Makefile prints the `Free space` line at the end of every build. That number is the
one and only metric. Reference point at the start of the pass:

    app.bin (raw RAM app) 54644 B  ->  app.bin.lzma 36831 B  (ratio 0.674)
    stub.elf .text 40872 B (stub code ~4041 B + the 36831 B app blob)  ->  76 B free

Reducing either the compressed app or the stub code raises Free space. The app is the
bulk and is what the LZMA stage acts on, so that is where the leverage is.

## 2. The compression pipeline (and the stub decode it must match)

The app is compressed in the Makefile (the `app.bin.lzma` rule):

    xz -f -c --format=raw --armthumb --lzma1=dict=128KiB,lc=1,lp=1,pb=1

Two filters are chained: the ARMTHUMB BCJ filter, then raw LZMA1.

The stub (`src/chainloader/stub_main.c`) reverses them in order: it runs `LzmaDecode`
with the matching props, then undoes the BCJ in place over the decoded image, then
flushes caches and jumps. The props passed to `LzmaDecode` are
`{0x37, 0x00, 0x00, 0x02, 0x00}`: the first byte encodes lc/lp/pb as
`(pb*5 + lp)*9 + lc = (1*5+1)*9+1 = 0x37`; the next four are the 128 KiB dictionary
size little-endian.

This is the boot-critical decode path. The encoder (Makefile) and decoder (stub) must
stay in lock-step: if the xz filter chain, lc/lp/pb, or dict size change, the stub's
props byte and/or the inverse-BCJ call must change to match, or the app will not
decompress and the device will not boot. A mismatch fails immediately (no menu), so it
is caught at once and is trivially reverted (build-config plus a tiny stub function).

### The ARMTHUMB BCJ filter

The RAM app is Thumb-2 code. A BCJ ("branch / call / jump") filter rewrites relative
branch targets to absolute addresses before compression, which turns near-identical
branch encodings into repeated byte sequences that LZMA back-references almost for free.
The stub's `armthumb_unfilter()` is the inverse transform (LZMA SDK `ARMT_Convert`,
decode direction, now_pos = 0 because the whole image is decoded as one buffer). It is a
lossless, position-in-stream byte transform: the runtime load address is irrelevant, so
it is always reversible. Use `--armthumb` (Thumb), not `--arm`; the latter measured
worse here, confirming the code is Thumb.

## 3. LZMA dynamics (the mental model)

LZMA is an LZ77 dictionary matcher plus a context-modeled range coder. Two facts govern
everything:

1. Repetition is nearly free. Any byte run LZMA has seen becomes a short back-reference.
   Duplicated code and data cost almost nothing.
2. Unique, high-entropy bytes are what you pay for. Novel instructions, one-off strings,
   distinct immediates, and library helper code dominate the compressed size.

Consequences, all confirmed by measurement on this codebase:

- Hand-consolidating small functions or pointer tables BACKFIRES. The toolchain already
  inlines small leaves (`-flto -Os`) and LZMA already compresses the repeats, so a shared
  extern just adds un-inlined body plus call overhead. Measured: collapsing duplicate
  vtable pointers cost +4 B; hoisting two byte-identical tiny leaf functions into one
  shared `utils` function cost +12 B. Both were reverted.
- The wins come from removing UNIQUE bytes (a one-off string, a whole feature, library
  pull-ins like 64-bit math / float / printf) or from feeding LZMA more compressible
  input (the BCJ filter).
- Counter-intuitively, `preset=9e` and `pb=0` both measured WORSE than the defaults here.
  Never assume a knob helps; measure it.
- Per-symbol `nm` sizes are unreliable under LTO (the big `W` symbols are LTO/LMA
  artifacts). Use them only to find candidates; the `Free space` line is the verdict.

## 4. What is already modularized (why obvious slop keeps vanishing)

The architecture already pushes non-base code out of the 40 KB, so `--gc-sections` plus
prior modularization have absorbed most easy slop:

- i18n: only English ASCII is baked in; all languages and the i18n machinery live in
  `/modules/language.bin`. `ui/i18n.c` is a thin delegating shim.
- Themes: color-only in core; sprite themes are `/modules/theme`.
- Filesystems: read-only drivers in core; read-write (lfs_rw, fatfs) are modules; heavy
  file operations are `/modules/fileops`. Feature apps (MP3, etc.) are modules.
- The LZMA decoder is shared stub to app: the stub publishes a fixed-address services
  table (`src/common/stub_services.h`, `STUB_SERVICES_ADDR 0x08000040`, magic 'STSV')
  so the app reuses the stub's `LzmaDecode` and carries only the allocator/heap.
- Dead code is already removed by `--gc-sections`. For example `assets.c` still defines a
  full sprite blitter (`gui_draw_asset*`) that nothing references (the theme module
  carries its own), and it is absent from `app.elf`. Deleting such source is a clarity
  cleanup, not a binary win.
- `board.c` and the HAL are compiled into both the stub and the app, but `--gc-sections`
  strips each binary to a disjoint used subset, so they are not a real duplicate. The big
  app-only HAL (ospi/spi/flash/ltdc/mdma) is absent from the stub, so it cannot be
  borrowed via the services table without growing the stub by more than the app saves.

All size flags are already maxed: `-Os -flto -fmerge-all-constants -fno-exceptions
-fno-unwind-tables -fomit-frame-pointer -ffunction-sections -fdata-sections
-Wl,--gc-sections -specs=nano.specs`. There is no remaining flag win.

## 5. Methodology

- Measure every change against the `Free space` line: `make clean && make -j16`. Keep
  measured wins, revert measured losses, treat swings under ~16 B as noise.
- For LZMA parameter and filter searches, use `scripts/debug/lzma_sweep.sh`. It
  compresses the already-built `build/app/app.bin` many ways and reports each size, so a
  whole matrix is explored without rebuilding (compressed size depends only on the xz
  settings, not the stub props).
- Any stub / decode / boot change must be verified on hardware: the device must reach the
  menu and the Retro-Go return round trip must still work. For LZMA changes specifically,
  a decode mismatch means no boot, so flash and check immediately and keep the previous
  `app.bin.lzma` to bisect.

### Levers, by whether they help

Help (remove unique bytes, or feed LZMA better input):
- BCJ filter for the target architecture (the big one here).
- Remove unique library pull-ins: 64-bit math, non-power-of-2 divide/modulo, float,
  printf/sprintf.
- Remove or shorten one-off string literals and dead data; move a non-base feature into
  its owning module.

Backfire under `-flto` plus LZMA (do not do for size):
- Merging small functions, sharing tiny helpers, table-driving branchy logic to "dedupe".
  The toolchain and LZMA already did the cheap version. Only dedupe when the duplicate is
  large, not inlined, and spans translation units.

## 6. Results (2026-06-04)

- Baseline: 76 B free.
- Tier 1 (LZMA stream): ARMTHUMB BCJ filter plus lc1/lp1/pb1 tuning. Compressed app
  36831 -> 34990 B; stub grew ~137 B for the inverse-BCJ. Net 76 -> 1780 B free (+1704).
- Tier 2b (modularization): the `.lang` pack format was duplicated (the language module
  already had `lang_parse`, yet core `vfs.c` also carried `vfs_read_lang_lfs` and
  `vfs_lfs_enum_langs`). Both removed from core; the module now discovers via the generic
  `vfs_lfs_enum_dir` (snapshot names, then read and parse each 76-byte header itself) and
  loads via a new generic `vfs_lfs_read`. Net 1780 -> 1948 B free (+168).
- Total: 76 -> 1948 B free (about 25x), all verified booting (and rendering the German
  menu, which exercises language discovery, load, and restore) on hardware.

## 7. Future opportunities (assessed, not done)

- OFW sprite-asset extraction into the theme module. `board_load_dynamic_assets`
  (`assets.c`) plus `load_palette_from_compressed_memory` populate the dynamic
  tileset/palette that only the theme module renders. It belongs in the theme module, but
  it is deeply entangled: it runs at boot before the theme module loads, it sets the base
  GUI colors, and it is full of fragile OFW-specific magic offsets plus dependencies
  (ospi init, valid-app check, scratch buffer, the LZMA decoder, ui_update_theme). Moving
  it cleanly needs several new host hooks (which offset the saving) and a split of the
  base color setup (stays in core) from the sprite extraction (moves). Estimated ~1 KB.
  AGREED PLAN: untangle and clean this up as its own deliberate task, with the dead core
  blitter source deleted at the same time, and verify by switching to the sprite theme on
  hardware. Not worth rushing in a sweep given the headroom now available.
- Incremental slop hunt across the larger app files, LZMA-aware (unique-byte removals and
  genuine cross-translation-unit non-inlined dups only). Low urgency at current headroom.
- HAL bare-metal init. The largest raw cluster, but boot-path-critical, has no reference
  to port, and is already gc-stripped per binary. Pursue only if a future need demands it.

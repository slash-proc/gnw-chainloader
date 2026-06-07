#!/usr/bin/env python3
"""
extract_smb1_player_sprites.py

Extract clean walking (and other) Mario & Luigi sprites from the SMB1 ROM that
ships inside the Mario Game & Watch firmware.

WHY THIS IS NEEDED
------------------
The G&W clock tileset only contains *standing* Mario/Luigi. The actual
walking/running animation frames live in the embedded SMB1 NES ROM's CHR-ROM,
but they are NOT laid out as contiguous sprites -- SMB1's metasprite engine
assembles each player frame from a list of 8x8 tiles defined in a PRG-ROM table
called `PlayerGraphicsTable`. You must read that table to know which tiles make
up each frame.

THE METHOD (5 steps)
--------------------
1. Locate the SMB1 ROM. In a stock-decrypted external flash it is at offset 0x0
   (iNES header + PRG + CHR). We use the already-dumped build/smb1.nes for
   convenience; the bytes are identical.
       PRG  = rom[16 : 16 + rom[4]*16384]      (32 KiB)
       CHR  = rom[16+len(PRG) : ...][:rom[5]*8192]  (8 KiB = 512 8x8 2bpp tiles)

2. Find PlayerGraphicsTable in the PRG by searching for the first frame's tile
   bytes "00 01 02 03 04 05" (the standing/walk-1 frame). In USA SMB1 it lands
   at PRG file offset 0x6E17. Each table entry is **8 bytes = 8 tile indices**,
   laid out as a 2-wide x 4-tall (16x32 px) metasprite, row-major:
       index 0,1 = top row    (tiles at x=0/x=8, y=0)
       index 2,3 = 2nd row     (y=8)
       index 4,5 = 3rd row     (y=16)
       index 6,7 = bottom row  (y=24)
   Tile value 0xFC means "blank" (transparent). SMALL Mario frames have the top
   four tiles = 0xFC, so only the bottom 16x16 is drawn.

3. Frame indices (USA SMB1 PlayerGraphicsTable order):
       BIG  Mario:  0=stand 1,2,3=walk 4=skid 5=jump 6-9=swim 10=crouch 11=climb
       SMALL Mario: 12=stand 13,14,15=walk 16=skid 17=jump 18,19=swim ...
   (Big = frames 0-11, small = 12+.)

4. Decode each 8x8 CHR tile as standard NES 2bpp: 16 bytes/tile, first 8 = bit
   plane 0, next 8 = bit plane 1; pixel = bit0 | (bit1<<1), giving color 0-3.
   Color 0 = transparent.

5. Color with the NES master palette stored in the firmware's external flash at
   0xA8B84 (64 colors x 3-byte RGB). The standard SMB player sub-palette is
   {transparent, $16, $27, $18}. Mario uses red $16; Luigi swaps red->green $1A.

OUTPUT: build_mario/nes_assets/walking/{mario,luigi}_{big,small}_{pose}.png
(opaque + *_transparent.png) plus *_walkcycle.png filmstrips.

To integrate into the project: this is pure offset+palette logic with no extra
deps beyond Pillow. Drop the constants into a MarioGnW helper and emit the PNGs
during patching, reading the SMB1 ROM and NES palette straight from
self.external instead of build/smb1.nes.
"""
from pathlib import Path

from PIL import Image

# --- fixed offsets ---------------------------------------------------------
SMB1_ROM = Path("build/smb1.nes")
STOCK_FLASH = Path("build_mario/decrypt_flash_stock.bin")
NES_PALETTE_OFF = 0xA8B84          # in external flash: 64 colors x 3-byte RGB
PLAYER_TABLE_SIG = bytes([0, 1, 2, 3, 4, 5])  # start of PlayerGraphicsTable
FRAME_TILES = 8                    # 8 tiles per frame (2x4, 16x32)
BLANK = 0xFC                       # transparent tile marker

# frame index -> name (USA SMB1 PlayerGraphicsTable order)
BIG = {"big_stand": 0, "big_walk1": 1, "big_walk2": 2, "big_walk3": 3,
       "big_skid": 4, "big_jump": 5}
SMALL = {"small_stand": 12, "small_walk1": 13, "small_walk2": 14,
         "small_walk3": 15, "small_skid": 16, "small_jump": 17}

OUT = Path("build_mario/nes_assets/walking")
BG = (107, 140, 255)               # sky blue, only for the opaque previews


def load():
    rom = SMB1_ROM.read_bytes()
    prg = rom[16:16 + rom[4] * 16384]
    chr_ = rom[16 + len(prg):][: rom[5] * 8192]
    flash = STOCK_FLASH.read_bytes()
    nes = [tuple(flash[NES_PALETTE_OFF + i * 3: NES_PALETTE_OFF + i * 3 + 3]) for i in range(64)]
    tbl = prg.find(PLAYER_TABLE_SIG)
    if tbl < 0:
        raise SystemExit("PlayerGraphicsTable not found")
    return prg, chr_, nes, tbl


def frame_rgba(prg, chr_, tbl, idx, subpal):
    """subpal = [c1, c2, c3] RGB; color 0 is transparent. Returns autocropped RGBA."""
    tiles = prg[tbl + idx * FRAME_TILES: tbl + idx * FRAME_TILES + FRAME_TILES]
    im = Image.new("RGBA", (16, 32), (0, 0, 0, 0))
    positions = [(0, 0), (8, 0), (0, 8), (8, 8), (0, 16), (8, 16), (0, 24), (8, 24)]
    for t, (ox, oy) in zip(tiles, positions):
        if t == BLANK:
            continue
        lo = chr_[t * 16: t * 16 + 8]
        hi = chr_[t * 16 + 8: t * 16 + 16]
        for y in range(8):
            for x in range(8):
                b = ((lo[y] >> (7 - x)) & 1) | (((hi[y] >> (7 - x)) & 1) << 1)
                if b:
                    im.putpixel((ox + x, oy + y), subpal[b - 1] + (255,))
    bb = im.getbbox()
    return im.crop(bb) if bb else im


def save(im, path, scale=7, opaque_bg=None):
    if opaque_bg:
        bg = Image.new("RGB", im.size, opaque_bg)
        bg.paste(im, (0, 0), im)
        im = bg
    im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(path)


def main():
    prg, chr_, nes, tbl = load()
    print(f"PlayerGraphicsTable @ PRG 0x{tbl:X}")
    OUT.mkdir(parents=True, exist_ok=True)
    sub = {"mario": [nes[0x16], nes[0x27], nes[0x18]],
           "luigi": [nes[0x1A], nes[0x27], nes[0x18]]}
    for who, subpal in sub.items():
        imgs = {}
        for nm, idx in {**BIG, **SMALL}.items():
            im = frame_rgba(prg, chr_, tbl, idx, subpal)
            imgs[nm] = im
            save(im, OUT / f"{who}_{nm}.png", opaque_bg=BG)
            save(im, OUT / f"{who}_{nm}_transparent.png")
        for tag, keys in [("big_walk", ["big_stand", "big_walk1", "big_walk2", "big_walk3"]),
                          ("small_walk", ["small_stand", "small_walk1", "small_walk2", "small_walk3"])]:
            seq = [imgs[k] for k in keys]
            W = sum(i.width for i in seq) + 6 * (len(seq) - 1)
            H = max(i.height for i in seq)
            strip = Image.new("RGB", (W, H), BG)
            x = 0
            for i in seq:
                strip.paste(i, (x, H - i.height), i)
                x += i.width + 6
            save(strip, OUT / f"{who}_{tag}cycle.png")
    print(f"-> {OUT}/  (big+small, mario+luigi, opaque + transparent + walkcycles)")


if __name__ == "__main__":
    main()

"""Zelda-specific convenience helpers shared by the visual tools.

The CHR + sprite-palette acquisition was duplicated across ``extract_*`` and
``render_*`` scripts; it lives here so ``extract.py`` and ``render.py`` share one copy.
"""
from __future__ import annotations

import shutil
from pathlib import Path

from . import flashio, nesgfx
from . import palette as palmod


def chr_and_sprite_palette(console: str = "zelda", rom: str | None = None):
    """Return ``(chr_rom_bytes, sprite_palette)`` for NES CHR rendering.

    CHR comes from a NES ROM *file* when *rom* is given, else it is reconstructed from the
    embedded ROM in decrypted external flash (note: with the current backups no plaintext
    ROM is present, so the flash path raises — pass ``--rom`` for a working extraction).
    The NES master palette always comes from decrypted flash via the offset registry.
    """
    ext = flashio.load_decrypted(console)
    reg = flashio.info(console)

    if rom:
        nes = flashio.load_raw(rom)
        hdr = nesgfx.parse_ines(nes)
        chr_off = hdr.chr_offset if hdr.valid else 0x20010
        chr_rom = nes[chr_off:chr_off + 128 * 1024]
    else:
        rom_off = reg["rom_offset"]
        if ext[rom_off:rom_off + 4] != b"NES\x1a":
            found = ext.find(b"NES\x1a")
            if found == -1:
                raise SystemExit(
                    "NES ROM magic not found in decrypted external flash (the ROM is not "
                    "stored in plaintext here). Pass the ROM file explicitly with --rom PATH.")
            rom_off = found
        hdr = nesgfx.parse_ines(ext[rom_off:])
        chr_off = rom_off + (hdr.chr_offset if hdr.valid else 0x20010)
        chr_rom = ext[chr_off:chr_off + 128 * 1024]

    nes_palette = palmod.load_nes_master_palette(ext, reg["nes_master_palette_offset"], 64)
    return chr_rom, palmod.link_sprite_palette(nes_palette)


def extract_assets(ext_data, metadata, out_dir, *, gnw_palette_offset, tiles_offset,
                   rom_offset=None, nes_palette_offset=None):
    """Render the Zelda asset PNGs described by ``zelda_tiles_v3.json``.

    Authoritative port of the extractor that used to live inline in
    ``scripts/build/patch_firmware.py`` (the build pipeline's reference logic). It is
    moved here verbatim so the build and the debug tooling share one copy.

    *ext_data* is the decrypted/patched external flash (byte-identical to
    ``build/patched_external_zelda.bin``); *metadata* is the parsed layout JSON.
    Per-asset PNGs land under *out_dir* (``build/extracted_zelda``); the labeled and 8x8
    tileset overviews land alongside it. When *rom_offset* / *nes_palette_offset* are
    supplied and the embedded Zelda II CHR-ROM is present, the walking-Link frames are
    emitted as well.
    """
    from PIL import Image, ImageDraw

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Define quadrant decoding helper for Zelda I 16x16 sprites
    def assemble_quadrant_sprite(cell_bytes):
        pixels = [[0] * 16 for _ in range(16)]
        quad_offsets = [(0, 0), (0, 8), (8, 0), (8, 8)]  # TL, BL, TR, BR
        for k, (dx, dy) in enumerate(quad_offsets):
            tile = cell_bytes[k * 64 : (k + 1) * 64]
            for ty in range(8):
                for tx in range(8):
                    if ty * 8 + tx < len(tile):
                        pixels[dy + ty][dx + tx] = tile[ty * 8 + tx]
        return pixels

    # Load Zelda I G&W Palette
    pal_offset = gnw_palette_offset
    palette = []
    for i in range(256):
        c_off = pal_offset + i * 4
        if c_off + 4 > len(ext_data):
            break
        b, g, r, a = ext_data[c_off : c_off + 4]
        palette.append((r, g, b))

    def extract_node(val, prefix=""):
        if isinstance(val, dict):
            if "tile" in val:
                tile_str = val["tile"]
                vertical = val.get("vertical") == True

                parts = tile_str.split('.')
                cell_idx = int(parts[0])
                sub_idx = int(parts[1]) if len(parts) > 1 else 0

                tile_data_offset = tiles_offset + cell_idx * 256
                cell_bytes = ext_data[tile_data_offset : tile_data_offset + 256]
                sprite_pixels = assemble_quadrant_sprite(cell_bytes)

                if vertical:
                    img = Image.new("RGBA", (8, 16), (0, 0, 0, 0))
                    pixels = img.load()
                    x_offset = 0 if sub_idx == 0 else 8
                    for py in range(16):
                        for px in range(8):
                            color_idx = sprite_pixels[py][x_offset + px]
                            if color_idx == 0:
                                color = (0, 0, 0, 0)
                            elif color_idx < len(palette):
                                r, g, b = palette[color_idx]
                                color = (r, g, b, 255)
                            else:
                                color = (0, 0, 0, 255)
                            pixels[px, py] = color
                    img_scaled = img.resize((64, 128), Image.Resampling.NEAREST)
                    img_scaled.save(out_dir / f"{prefix}.png")
                else:
                    img = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
                    pixels = img.load()
                    quad_offsets = [(0, 0), (0, 8), (8, 0), (8, 8)]
                    dx, dy = quad_offsets[sub_idx]
                    quad_tile = cell_bytes[sub_idx * 64 : (sub_idx + 1) * 64]
                    for ty in range(8):
                        for tx in range(8):
                            if ty * 8 + tx < len(quad_tile):
                                color_idx = quad_tile[ty * 8 + tx]
                                if color_idx == 0:
                                    color = (0, 0, 0, 0)
                                elif color_idx < len(palette):
                                    r, g, b = palette[color_idx]
                                    color = (r, g, b, 255)
                                else:
                                    color = (0, 0, 0, 255)
                                pixels[tx, ty] = color
                    img_scaled = img.resize((64, 64), Image.Resampling.NEAREST)
                    img_scaled.save(out_dir / f"{prefix}.png")
            else:
                for k, v in val.items():
                    extract_node(v, f"{prefix}_{k}" if prefix else k)

        elif isinstance(val, list):
            for tile_idx in val:
                tile_data_offset = tiles_offset + tile_idx * 256
                cell_bytes = ext_data[tile_data_offset : tile_data_offset + 256]
                sprite_pixels = assemble_quadrant_sprite(cell_bytes)
                img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
                pixels = img.load()
                for py in range(16):
                    for px in range(16):
                        color_idx = sprite_pixels[py][px]
                        if color_idx == 0:
                            color = (0, 0, 0, 0)
                        elif color_idx < len(palette):
                            r, g, b = palette[color_idx]
                            color = (r, g, b, 255)
                        else:
                            color = (0, 0, 0, 255)
                        pixels[px, py] = color
                img_scaled = img.resize((64, 64), Image.Resampling.NEAREST)
                if len(val) == 1:
                    img_scaled.save(out_dir / f"{prefix}.png")
                else:
                    img_scaled.save(out_dir / f"{prefix}_{tile_idx}.png")

        elif isinstance(val, int):
            tile_idx = val
            tile_data_offset = tiles_offset + tile_idx * 256
            cell_bytes = ext_data[tile_data_offset : tile_data_offset + 256]
            sprite_pixels = assemble_quadrant_sprite(cell_bytes)
            img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
            pixels = img.load()
            for py in range(16):
                for px in range(16):
                    color_idx = sprite_pixels[py][px]
                    if color_idx == 0:
                        color = (0, 0, 0, 0)
                    elif color_idx < len(palette):
                        r, g, b = palette[color_idx]
                        color = (r, g, b, 255)
                    else:
                        color = (0, 0, 0, 255)
                    pixels[px, py] = color
            img_scaled = img.resize((64, 64), Image.Resampling.NEAREST)
            img_scaled.save(out_dir / f"{prefix}.png")

        elif isinstance(val, str):
            parts = val.split('.')
            cell_idx = int(parts[0])
            quad_idx = int(parts[1]) if len(parts) > 1 else 0

            tile_data_offset = tiles_offset + cell_idx * 256
            cell_bytes = ext_data[tile_data_offset : tile_data_offset + 256]

            quad_offsets = [(0, 0), (0, 8), (8, 0), (8, 8)]
            dx, dy = quad_offsets[quad_idx]
            tile = cell_bytes[quad_idx * 64 : (quad_idx + 1) * 64]

            img = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
            pixels = img.load()
            for ty in range(8):
                for tx in range(8):
                    if ty * 8 + tx < len(tile):
                        color_idx = tile[ty * 8 + tx]
                        if color_idx == 0:
                            color = (0, 0, 0, 0)
                        elif color_idx < len(palette):
                            r, g, b = palette[color_idx]
                            color = (r, g, b, 255)
                        else:
                            color = (0, 0, 0, 255)
                        pixels[tx, ty] = color
            img_scaled = img.resize((64, 64), Image.Resampling.NEAREST)
            img_scaled.save(out_dir / f"{prefix}.png")

    extract_node(metadata)

    # Custom assemble dungeon frame
    dungeon = metadata["dungeon_environment"]
    img_frm = Image.new("RGBA", (256, 192), (0, 0, 0, 0))
    pixels_frm = img_frm.load()
    all_tiles = []
    for c in ["top_left", "top_right", "bottom_left", "bottom_right"]:
        if c in dungeon["corners"]:
            all_tiles.append(dungeon["corners"][c])
    for w in ["top_wall_border", "left_wall_border", "right_wall_border", "bottom_wall_border", "inner_wall_shading"]:
        if w in dungeon["walls_and_frames"]:
            val = dungeon["walls_and_frames"][w]
            if isinstance(val, list):
                all_tiles.extend(val)
            elif isinstance(val, dict):
                for item in val.values():
                    if isinstance(item, dict) and "tile" in item:
                        parts = item["tile"].split('.')
                        all_tiles.append(int(parts[0]))
                    elif isinstance(item, int):
                        all_tiles.append(item)

    for tile_idx in all_tiles:
        row = tile_idx // 16
        col = tile_idx % 16
        tile_data_offset = tiles_offset + tile_idx * 256
        cell_bytes = ext_data[tile_data_offset : tile_data_offset + 256]
        sprite_pixels = assemble_quadrant_sprite(cell_bytes)
        for py in range(16):
            for px in range(16):
                color_idx = sprite_pixels[py][px]
                if color_idx == 0:
                    color = (0, 0, 0, 0)
                elif color_idx < len(palette):
                    r, g, b = palette[color_idx]
                    color = (r, g, b, 255)
                else:
                    color = (0, 0, 0, 255)
                img_x = col * 16 + px
                img_y = row * 16 + py
                if 0 <= img_x < 256 and 0 <= img_y < 192:
                    pixels_frm[img_x, img_y] = color
    img_frm.save(out_dir / "dungeon_environment_walls_and_frames_dungeon_frame.png")

    friendly_aliases = {
        "items_and_inventory_potions_and_containers_healing_heart.png": "healing_heart.png",
        "items_and_inventory_quest_items_dungeon_key.png": "key.png"
    }
    for src_name, dst_name in friendly_aliases.items():
        src_path = out_dir / src_name
        dst_path = out_dir / dst_name
        if src_path.exists():
            shutil.copy(src_path, dst_path)

    # Labeled Zelda I G&W Tileset rendering using quadrant assembly
    grid_w = 16
    grid_h = 16
    scale = 2
    cell_size = 16 * scale + 2  # 34x34
    img_lbl = Image.new("RGB", (grid_w * cell_size, grid_h * cell_size), (40, 40, 40))
    draw = ImageDraw.Draw(img_lbl)

    for ty in range(grid_h):
        for tx in range(grid_w):
            tile_idx = ty * grid_w + tx
            tile_data_offset = tiles_offset + tile_idx * 256
            tile_bytes = ext_data[tile_data_offset : tile_data_offset + 256]
            sprite_pixels = assemble_quadrant_sprite(tile_bytes)

            cx = tx * cell_size + 1
            cy = ty * cell_size + 1

            for py in range(16):
                for px in range(16):
                    color_idx = sprite_pixels[py][px]
                    # Force color 0 to be black (0, 0, 0)
                    if color_idx == 0:
                        color = (0, 0, 0)
                    elif color_idx < len(palette):
                        color = palette[color_idx]
                    else:
                        color = (0, 0, 0)
                    for dy in range(scale):
                        for dx in range(scale):
                            img_lbl.putpixel((cx + px * scale + dx, cy + py * scale + dy), color)

            draw.rectangle([cx - 1, cy - 1, cx + 16 * scale, cy + 16 * scale], outline=(80, 80, 80))
            draw.text((cx + 1, cy + 1), str(tile_idx), fill=(255, 255, 255))

    img_lbl.save(out_dir.parent / "zelda1_tileset_labeled.png")

    # Generate 8x8 tileset (raw Zelda I tileset)
    img_8x8 = Image.new("RGB", (16 * 8 * 2, 64 * 8 * 2))
    pixels_8x8 = img_8x8.load()
    for ty in range(64):
        for tx in range(16):
            tile_idx = ty * 16 + tx
            tile_data_offset = tiles_offset + tile_idx * 64
            tile_bytes = ext_data[tile_data_offset : tile_data_offset + 64]
            cx = tx * 16
            cy = ty * 16
            for py in range(8):
                for px in range(8):
                    if py * 8 + px < len(tile_bytes):
                        color_idx = tile_bytes[py * 8 + px]
                    else:
                        color_idx = 0
                    # Force color 0 to be black (0, 0, 0)
                    if color_idx == 0:
                        color = (0, 0, 0)
                    elif color_idx < len(palette):
                        color = palette[color_idx]
                    else:
                        color = (0, 0, 0)
                    for dy in range(2):
                        for dx in range(2):
                            pixels_8x8[cx + px * 2 + dx, cy + py * 2 + dy] = color
    img_8x8.save(out_dir.parent / "zelda_tileset_8x8.png")

    # 2. Extract Zelda II walking Link sprites from NES CHR-ROM (Page 3 offsets 0, 8, 40)
    if rom_offset is None or nes_palette_offset is None:
        return
    # Since the NES ROM in the G&W flash is headerless, we don't check for "NES\x1a" magic.
    # Instead, we directly load the CHR-ROM which is at offset rom_offset + 0x20000.
    chr_offset = rom_offset + 0x20000
    if len(ext_data) >= chr_offset + 128 * 1024:
        print("Extracting Zelda II walking Link from external flash...")
        chr_rom = ext_data[chr_offset : chr_offset + 128 * 1024]

        # NES palette
        nes_palette = []
        for i in range(64):
            c_off = nes_palette_offset + i * 3
            r, g, b = ext_data[c_off : c_off + 3]
            nes_palette.append((r, g, b))

        link_color_indices = list(flashio.LINK_COLOR_INDICES)
        sprite_palette = [nes_palette[idx] for idx in link_color_indices]

        link_mapping = [
            (0, 0), (0, 8),
            (8, 0), (8, 8),
            (0, 16), (0, 24),
            (8, 16), (8, 24)
        ]

        walk_offsets = [0, 8, 40]
        link_scale = 4  # Scale 4x for consistency

        for idx, start_t in enumerate(walk_offsets):
            img_l = Image.new("RGB", (16, 32))
            pix_l = img_l.load()

            for tile_i in range(8):
                t_rel = start_t + tile_i
                tile_idx = 3 * 2048 + t_rel
                tile_bytes_offset = tile_idx * 16
                tile_bytes = chr_rom[tile_bytes_offset : tile_bytes_offset + 16]

                tx_start, ty_start = link_mapping[tile_i]
                for py in range(8):
                    low = tile_bytes[py]
                    high = tile_bytes[py + 8]
                    for px in range(8):
                        bit0 = (low >> (7 - px)) & 1
                        bit1 = (high >> (7 - px)) & 1
                        color_idx = (bit1 << 1) | bit0
                        pix_l[tx_start + px, ty_start + py] = sprite_palette[color_idx]

            # Save left frame
            img_l_scaled = img_l.resize((16 * link_scale, 32 * link_scale), Image.Resampling.NEAREST)
            img_l_scaled.save(out_dir / f"link_walk_left_{idx}.png")

            # Save right frame
            img_r = img_l.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
            img_r_scaled = img_r.resize((16 * link_scale, 32 * link_scale), Image.Resampling.NEAREST)
            img_r_scaled.save(out_dir / f"link_walk_right_{idx}.png")

            print(f"Extracted walking frame {idx} (left/right)")

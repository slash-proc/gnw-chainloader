#!/usr/bin/env python3
"""
Display testing utility for the Game & Watch (STM32H7B0).
Fills the framebuffer with test patterns (solid, stripes, checkerboard),
manages D-cache coherency, and triggers LTDC shadow reload.
"""

import argparse
import sys
import time
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# LTDC Registers
LTDC_BASE = 0x50001000
LTDC_SRCR = LTDC_BASE + 0x024      # Shadow Reload Control Register
LTDC_L1CFBAR = LTDC_BASE + 0x0AC    # Layer 1 Color Frame Buffer Address Register

# Default Framebuffer settings
DEFAULT_FB_ADDR = 0x240002E0
SCREEN_W = 320
SCREEN_H = 240
FB_SIZE = SCREEN_W * SCREEN_H


def clean_dcache(backend):
    print("Cleaning D-cache for coherency...")
    try:
        backend("dcache clean", decode=False)
        print("  D-cache cleaned via OpenOCD command.")
        return
    except Exception:
        pass

    try:
        # Clean data cache via CP15 DSB instruction injection
        backend("arm mcr 15 0 7 10 4", decode=False)
        print("  D-cache cleaned via ARM CP15 DSB.")
    except Exception as e:
        print(f"  Warning: D-cache cleaning failed: {e}")


def trigger_reload(backend):
    print("Triggering LTDC shadow reload...")
    try:
        # Write 1 to LTDC_SRCR.IMR (immediate shadow reload)
        backend.write_uint32(LTDC_SRCR, 1)
        print("  Reload triggered.")
    except Exception as e:
        print(f"  Warning: Failed to trigger LTDC reload: {e}")


def get_fb_address(backend):
    try:
        fb_addr = backend.read_uint32(LTDC_L1CFBAR)
        if fb_addr >= 0x20000000 and fb_addr < 0x38000000:
            print(f"Detected active framebuffer address from LTDC: 0x{fb_addr:08X}")
            return fb_addr
    except Exception:
        pass
    print(f"Using default framebuffer address: 0x{DEFAULT_FB_ADDR:08X}")
    return DEFAULT_FB_ADDR


def generate_pattern(pattern_type, args):
    w, h = SCREEN_W, SCREEN_H
    data = bytearray(FB_SIZE)

    if pattern_type == "solid":
        color = args.color & 0xFF
        data = bytes([color]) * FB_SIZE
        print(f"Generated solid pattern with color index {color}")

    elif pattern_type == "stripes":
        if args.stripe_mode == "palette":
            print("Generated palette bands (6 colors)")
            band_h = h // 6
            for y in range(h):
                color = min(y // band_h, 5)
                data[y * w : (y + 1) * w] = bytes([color]) * w
        else:
            color1 = args.color1 & 0xFF
            color2 = args.color2 & 0xFF
            stripe_h = args.stripe_h
            print(f"Generated alternating stripes of index {color1} and {color2} (height {stripe_h})")
            for y in range(h):
                color = color1 if (y // stripe_h) % 2 == 0 else color2
                data[y * w : (y + 1) * w] = bytes([color]) * w

    elif pattern_type == "checkerboard":
        color1 = args.color1 & 0xFF
        color2 = args.color2 & 0xFF
        size = args.check_size
        print(f"Generated checkerboard of index {color1} and {color2} (square size {size})")
        for y in range(h):
            row_y = y // size
            for x in range(0, w, size):
                col_x = x // size
                color = color1 if (row_y + col_x) % 2 == 0 else color2
                chunk_size = min(size, w - x)
                data[y * w + x : y * w + x + chunk_size] = bytes([color]) * chunk_size

    return data


def main():
    parser = argparse.ArgumentParser(
        description="Display testing utility for the Game & Watch (STM32H7B0)."
    )
    parser.add_argument("--no-halt", action="store_true", help="Do not halt the target CPU before operations")
    parser.add_argument("--fb-addr", type=lambda x: int(x, 0), help="Override framebuffer start address")
    parser.add_argument("--no-cache-clean", action="store_true", help="Do not clean D-cache")
    parser.add_argument("--no-reload", action="store_true", help="Do not trigger LTDC shadow reload")
    parser.add_argument("--resume", action="store_true", help="Resume target CPU execution after test")

    subparsers = parser.add_subparsers(dest="command", required=True, help="Pattern type to draw")

    # Solid
    solid_parser = subparsers.add_parser("solid", help="Solid color pattern")
    solid_parser.add_argument("color", type=int, nargs="?", default=1, help="Palette color index (0-255)")

    # Stripes
    stripes_parser = subparsers.add_parser("stripes", help="Horizontal stripes pattern")
    stripes_parser.add_argument("stripe_mode", choices=["palette", "alt"], nargs="?", default="palette",
                                help="Stripe mode: 'palette' (6-color bands) or 'alt' (alternating two colors)")
    stripes_parser.add_argument("--color1", type=int, default=1, help="First color index for alt mode")
    stripes_parser.add_argument("--color2", type=int, default=0, help="Second color index for alt mode")
    stripes_parser.add_argument("--stripe-h", type=int, default=8, help="Stripe height in rows for alt mode")

    # Checkerboard
    checker_parser = subparsers.add_parser("checkerboard", help="Checkerboard pattern")
    checker_parser.add_argument("--color1", type=int, default=1, help="First color index")
    checker_parser.add_argument("--color2", type=int, default=0, help="Second color index")
    checker_parser.add_argument("--check-size", type=int, default=16, help="Checkerboard square size in pixels")

    # Reload only
    subparsers.add_parser("reload", help="Just trigger shadow reload and D-cache clean without modifying memory")

    args = parser.parse_args()

    backend = OpenOCDBackend()
    backend.open()
    try:
        if not args.no_halt:
            backend.halt()

        fb_addr = args.fb_addr if args.fb_addr is not None else get_fb_address(backend)

        if args.command in ["solid", "stripes", "checkerboard"]:
            data = generate_pattern(args.command, args)
            print(f"Writing pattern to 0x{fb_addr:08X}...")
            backend.write_memory(fb_addr, data)

        if not args.no_cache_clean:
            clean_dcache(backend)

        if not args.no_reload:
            trigger_reload(backend)

        if args.resume:
            print("Resuming CPU...")
            backend.resume()
        elif not args.no_halt and args.command == "reload":
            # If we halted just for reload and didn't ask to resume, restore previous state
            print("Resuming CPU...")
            backend.resume()

    finally:
        backend.close()


if __name__ == "__main__":
    main()

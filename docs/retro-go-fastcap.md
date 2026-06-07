# Retro-Go Remote Input and Fastcap Integration

This document outlines the architecture, memory design, and integration steps for incorporating host remote control and fast frame capture (fastcap) hooks into the Retro-Go firmware (`retro-go-sd`) on the Game & Watch (STM32H7B0).

---

## 1. Memory Design & Handshake Cells

To allow the host-side debug tools, chainloader, and Retro-Go to communicate, a region of DTCMRAM is reserved for handshake cells. 

The stack pointer (`_estack`) was lowered to `0x2001FF00` in the Retro-Go linker scripts:
* [STM32H7B0VBTx_FLASH.ld](file:///home/doug/Nerd/git/gnw-chainloader/retro-go-sd/STM32H7B0VBTx_FLASH.ld)
* [STM32H7B0VBTx_SDCARD.ld](file:///home/doug/Nerd/git/gnw-chainloader/retro-go-sd/STM32H7B0VBTx_SDCARD.ld)

This leaves 256 bytes at the top of DTCMRAM (`0x2001FF00` to `0x2001FFFF`) completely untouched by the stack and BSS. The handshake cell assignments are defined as follows:

| Address | Cell Name | Purpose / Usage |
|---|---|---|
| `0x2001FF00` | `STATUS_FLAG_ADDR` | Device writes `1` (frame ready); host writes `0` (acknowledge). |
| `0x2001FF04` | `FASTCAP_HOOK_ADDR` | Pointer to the fastcap hook function (e.g. `encode_frame`). |
| `0x2001FF08` | `RESET_FLAG_ADDR` | Host writes `1` to force a keyframe refresh. |
| `0x2001FF0C` | `FASTCAP_QUAL_ADDR` | Host writes JPEG quality (`1`..`100`). |
| `0x2001FF14` | `FASTCAP_MODE_ADDR` | Host writes mode (`0` = async/real-time, `1` = sync/frame-perfect). |
| `0x2001FF18` | `PALETTE_ADDR` | Pointer to the active color lookup table (CLUT) in LUT8 modes, or `0` for RGB565. |
| `0x2001FFF4` | `SRAM_REMOTE_INPUT_ADDR` | Shadow cell for remote button bitmask injection. |

---

## 2. AHB RAM (D2-SRAM) Partitioning

The fastcap JPEG encoder runs from D2 AHB-SRAM1 (`0x30000000`). To prevent collisions between Retro-Go's emulation memory and fastcap:
1. Retro-Go's `AHBRAM` section base address was shifted from `0x30000000` to `0x30010000` (AHB-SRAM2).
2. Its length was reduced from 128 KiB to 64 KiB.

This ensures both the JPEG encoder buffers and the emulator heap/bss remain isolated.

---

## 3. Remote Input Hook

Remote control is integrated in [gw_buttons.c](file:///home/doug/Nerd/git/gnw-chainloader/retro-go-sd/Core/Src/gw_buttons.c). When compiled with `-DREMOTE_INPUT`, `buttons_get` reads the shadow cell:
* The host writes the unified button mask format to `0x2001FFF4`.
* `buttons_get` maps the unified format bits (UP, DOWN, LEFT, RIGHT, A, B, START, SELECT, etc.) to Retro-Go's native gamepad mapping and ORs them with the physical button states.

---

## 4. Fastcap Hook & LUT8 Palette Support

The capture hook is triggered inside `lcd_swap` in [gw_lcd.c](file:///home/doug/Nerd/git/gnw-chainloader/retro-go-sd/Core/Src/gw_lcd.c):
1. **Hook Invocation**: If the hook pointer at `FASTCAP_HOOK_ADDR` is non-NULL, the active framebuffer address is passed to it.
2. **Palette Handling**: 
   - In **LUT8 mode** (e.g. Game Boy, NES games with custom palettes), the active palette pointer (`active_clut`) is written to `PALETTE_ADDR` (`0x2001FF18`).
   - In **RGB565 mode**, `PALETTE_ADDR` is set to `0`.

The fastcap encoder in [src/fastcap/main.c](file:///home/doug/Nerd/git/gnw-chainloader/src/fastcap/main.c) and [src/fastcap/jpeg_enc.c](file:///home/doug/Nerd/git/gnw-chainloader/src/fastcap/jpeg_enc.c) was modified to detect a non-zero `PALETTE_ADDR`. When present, it resolves the 8-bit pixel color indices against the palette's RGB values before tile hashing and JPEG encoding.

---

## 5. Build Integration

The build settings in [Makefile.common](file:///home/doug/Nerd/git/gnw-chainloader/Makefile.common) dynamically append `-DREMOTE_INPUT` to Retro-Go's compilation flags if `REMOTE_INPUT` is set:
```make
build-rg:
	$(MAKE) -C retro-go-sd -j$(shell nproc) $(RG_VARS) CFLAGS_EXTRA="$(if $(filter-out 0,$(REMOTE_INPUT)),-DREMOTE_INPUT) $(CFLAGS_EXTRA)" all littlefs_image
```
This enables compiling and flashing the SD card build with remote input capabilities using:
```bash
make REMOTE_INPUT=1 flash-rg-sd
```

# gnw-chainloader

A bare-metal STM32H7B0 (Cortex-M7) chainloader for the Game & Watch handheld.

It resides at `0x08000000` (Bank 1) and uses a **Dieted RAM-boot architecture**: a minimal bare-metal loader (flash stub) inflates the main application from an LZMA-compressed blob into AXI-SRAM. This maximizes space, allowing for rich features like exFAT support within the 40 KiB internal flash limit. It evaluates boot conditions on startup and headlessly jumps to either the patched Official Firmware (OFW) on Bank 2 or Retro-Go Launcher on Bank 1.

---

## Key Features

*   **Fast, display-off boot:** Near-instant cold boots. The display only initializes for the menu, recovery, or flashing, ensuring a standard Game & Watch power-on experience.
*   **Automatic OFW Switching:** Hold **LEFT** or **RIGHT** at power-on to boot into Mario or Zelda OFW. The launcher automatically detects the active internal firmware and flashes the requested version from OSPI if necessary.
*   **Pick what to launch from the menu:** A single **Launch** item cycles Retro-Go, Mario, or Zelda; press the A button to start the highlighted one (a game not currently in the swappable slot is flashed there first). The label and the whole menu appear in your chosen language.
*   **Full Filesystem Support:** Includes a universal File Browser with **exFAT** support, Long File Names (255 chars), and hardware RTC timestamping for SD cards.
*   **Themed menus:** Mario and Zelda menus with an animated cursor and background sprites drawn from the real game art, plus semi-transparent blending. The sprite themes load as an optional module; if it is absent the menu uses a plain colored theme and stays fully usable.
*   **Main Menu Hiding:** The UI automatically fades out after 30 seconds of inactivity, leaving only the themed animations visible (ambient clock mode).
*   **Retro-Go state preservation:** Reads Retro-Go's magic words to tell when it wants to return to its own launcher, and reserves a 4 KB DTCM Safe Zone so Retro-Go's in-progress game state survives the warm reset.
*   **Toggle Fast-Boot Setting:** Configure the device via the Settings menu to bypass the chainloader and boot straight to Retro-Go on startup, with a dedicated hardware override (holding **PAUSE/SET** at boot) to return to the launcher.
*   **On-screen Diagnostics:** Deep system info, bank-usage stats, and a **Partition Viewer** with non-blocking background flash scanning and secure deletion.
*   **Multi-Language Menus:** The menu can be shown in 16 languages (English, German, French, Spanish, Italian, Portuguese, Dutch, Polish, Romanian, Russian, Ukrainian, Greek, Japanese, Simplified and Traditional Chinese, and Korean; English is the default). Pick one under **Settings → Language** and the whole UI switches and remembers the choice. Only English is built into the firmware; every other language is a small data file on the device's storage, so English always works even if those files are absent. To add a language, copy its files into the SD card's `/i18n/` folder; on the next boot the device asks you to confirm ("Install N language(s) from SD?") before copying them into its own storage, then applies the new languages right away. The same on-demand installer also brings over new device modules from the SD card after a separate confirmation, and those take effect on the following boot.

---

## Repository Structure

*   **[src/chainloader/](src/chainloader/)**: Core chainloader codebase (clocks, LTDC GUI, OSPI flash driver, main boot orchestrator).
*   **[src/patch/](src/patch/)**: The recovery hook and keypress-detection routines compiled and injected into the stock OFW binaries.
*   **[src/common/](src/common/)**: Shared registers, flash control, and inline bank-swapping implementations.
*   **[src/modules/](src/modules/)**: Loadable PIE modules (filesystem drivers, theme sprites, language packs) brought in on demand by the module loader.
*   **[src/fastcap/](src/fastcap/)**: Framebuffer-capture codec used to stream the screen over the debug probe during development.
*   **[scripts/debug/](scripts/debug/)**: Suite of JTAG/GDB debug and diagnostic utility scripts.
*   **[gnwmanager/](gnwmanager/)**: Git submodule for the host command-line utility.
*   **deps/**: Third-party vendored code (CMSIS, STM32 HAL, FatFs, LittleFS); not part of the chainloader proper.

---

## Getting Started

### Prerequisites

You need a modern version of the ARM GNU Toolchain (GCC 13+ is required) and `gnwmanager` installed on your host system:

*   **ARM GNU Toolchain:** Download and install `arm-none-eabi-gcc` from the official [Arm GNU Toolchain Downloads](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) page. Using package manager versions (such as `gcc-arm-none-eabi` from older `apt` repositories) is not recommended, as outdated compilers may lack optimizations required to fit the chainloader within the 40 KB flash limit.
*   **gnwmanager:** Ensure `gnwmanager` is installed on your host. You can verify it with:
    ```bash
    gnwmanager --version
    ```


### Cloning & Submodule Registration

This project relies on a custom fork of `gnwmanager` containing flashing and offset-patching changes that are not currently merged into the official upstream repository. Therefore, `gnwmanager` is integrated as a local Git submodule (tracked on the `extflash_offset` branch). Note that if these changes are merged upstream in the future, this submodule will no longer be required and you will be able to use the standard official `gnwmanager` package directly.

To clone the repository and initialize the submodule, run:

```bash
git clone --recurse-submodules https://github.com/your-username/gnw-chainloader.git
```

---

## Build & Flashing Pipeline

Flashing configurations automatically enforce the chainloader's **40 KiB** flash ceiling (40,960 bytes) and clean up any dynamically generated submodule cache files.

```bash
# Always clean the workspace before building
make clean

# Compile the chainloader (produces build/gnw_chainloader.bin)
make -j$(nproc)

# Patch the stock Mario/Zelda OFW binaries (inject recovery hook, relocate assets to external flash)
make patch

# Flash the chainloader and both patched OFW binaries at once
make flash_all

# Flash only the chainloader to Bank 1
make flash_chainloader

# Build optimized Retro-Go binaries
make build_rg

# Flash Retro-Go internal, FrogFS, and LittleFS images
make flash_rg
```

> [!NOTE]
> You can configure a custom external flash size using the `EXTFLASH_SIZE` parameter (minimum `16`, defaults to `64` MB):
> ```bash
> make EXTFLASH_SIZE=16 flash_all
> ```

---

## Design & Architecture

For a detailed breakdown of internal memory partitioning, flash offsets, patching mechanisms, bank swap registers, and boot orchestrator flows, see **[DESIGN.md](DESIGN.md)**.

---

## Credits & Acknowledgments

This project is built upon the incredible reverse engineering and homebrew work of the Game & Watch developer community.

### Development & Provenance Notes
- **AI-assisted development:** This codebase was developed with substantial AI assistance. The architecture, integration, on-hardware debugging, and final review were directed by the author.
- **Hardware bring-up:** The clock-tree, LCD (LTDC) panel timings, and OSPI flash configuration *values* were referenced from `game-and-watch-retro-go-sd`. These are hardware-dictated facts re-expressed in this project's own code — no source from that (GPL-2.0) project is included. See [LICENSE](LICENSE) for the full attribution breakdown.

### Related Projects
- **[game-and-watch-bootloader](https://github.com/sylverb/game-and-watch-bootloader)**
- **[game-and-watch-retro-go](https://github.com/sylverb/game-and-watch-retro-go-sd)**
- **[game-and-watch-patch](https://github.com/brianpugh/game-and-watch-patch)**
- **[gnwmanager](https://github.com/brianpugh/gnwmanager)**

### Special Thanks

A very special thanks to the core developers and reverse engineers who did all the actual hard work in decoding the STM32H7 Game & Watch hardware, initially hacking it, and building the community around it:
- **Thomas Roth ([stacksmashing](https://github.com/stacksmashing))** and **Kai Beckmann ([kbeckmann](https://github.com/kbeckmann))** for initially hacking the Game & Watch hardware and establishing the foundation for homebrew development.
- **Brian Pugh ([BrianPugh](https://github.com/brianpugh))**
- **Sylver Bruneau ([sylverb](https://github.com/sylverb))**
- **Marian Muller ([marian-m12l](https://github.com/marian-m12l))** — whose `game-and-watch-patch` fork is the source of the bank-swapping technique reimplemented in `src/common/banks.h`, and the reference `src/patch/main.c` is derived from.




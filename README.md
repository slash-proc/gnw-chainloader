# gnw-chainloader

The **gnw-chainloader** is a bare-metal storage management, partition inspection, and multi-boot utility for the Game & Watch handheld. Located at `slash-proc/gnw-chainloader`, the project's primary goals are to establish an inviolable, stable boot path (guaranteeing the system always boots safely) and to provide a robust framework for memory and storage oversight. By leveraging a hardware bank-swapping technique and an extensible dynamic module plugin system, it enables a stable triple-boot configuration for users who possess legal copies of both official Nintendo firmware (OFW) binaries and Retro-Go, while allowing optional features (such as file browsers and audio players) to load dynamically from RAM.

---

## Getting Started

### 1. User-Populated Files
Before building or flashing the project, you must populate specific directories with backups and ROM files:

*   **OFW Firmware Backups (`backup/`):** Place your official stock dumps inside this directory. These files are required by the patching and flashing tools:
    *   `backup/internal_flash_backup_mario.bin`
    *   `backup/flash_backup_mario.bin`
    *   `backup/internal_flash_backup_zelda.bin`
    *   `backup/flash_backup_zelda.bin`
*   **Retro-Go ROMs (`retro-go-sd/roms/`):** Place game ROM files in the appropriate subdirectory (e.g. `nes/` for NES games, `gb/` for Game Boy games, `sms/` for Sega Master System games) to package them for Retro-Go.

### 2. Turnkey Compilation & Flashing (Docker)
The simplest and most reliable way to compile and flash the project is using **Docker**. This encapsulates the compiler, Python scripts, and dependencies inside a container, removing any host setup overhead.

*   **Build and Flash Everything (SD Card variant):**
    ```bash
    make all-in-one-sd-docker
    ```
*   **Upgrade Launcher Only (preserves OFW, Retro-Go saves, and persistent files while updating chainloader and Retro-Go):**
    ```bash
    make upgrade-sd-docker
    ```

### 3. Native Compilation (Host Machine)

#### Clone the Repository
Cloning requires recursive submodule initialization:
```bash
git clone --recurse-submodules https://github.com/slash-proc/gnw-chainloader.git
```

#### Install Toolchain and Host Dependencies (Debian/Ubuntu)
1. Install system utilities and library dependencies:
   ```bash
   sudo apt-get update
   sudo apt-get install -y make patch xz-utils build-essential libusb-1.0-0 ffmpeg git python3 python3-pip python3-venv
   ```
2. **ARM GCC Toolchain:** Download and extract the official [Arm GNU Toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) (GCC 13 or newer is required). Make sure the compiler binaries are added to your shell's `PATH`.

3. **Python Virtual Environment Setup:** Configure and activate a virtual environment inside the repository directory:
   ```bash
   # Create a virtual environment inside the repository directory
   python3 -m venv .venv

   # Activate the virtual environment
   source .venv/bin/activate

   # Install required packages within the active environment
   pip install -r requirements.txt
   pip install -r requirements-dev.txt
   ```

#### Working with the Virtual Environment (`venv`)
To run Python helper scripts or use `make` compilation targets natively, the virtual environment must be active in your current shell session. Always run:
```bash
source .venv/bin/activate
```
before running host commands in the repository. If you close the terminal or start a new shell, you must re-activate it. Once activated, dependencies (such as `gnwmanager` and packages for compiling fonts/assets) are loaded cleanly from the isolated `.venv` directory.

#### Host Make Targets
Run these targets with the virtual environment activated:
*   **Build and Flash Everything:**
    ```bash
    make all-in-one-sd
    ```
*   **Launcher Upgrade (`upgrade-sd`):**
    ```bash
    make upgrade-sd
    ```
    *Note: This is a **non-destructive** update target that updates the chainloader and Retro-Go (including its internal flash image/`retrogo.crc`), while preserving the OFW, Retro-Go save files, and general persistent files (FAT and LittleFS partitions/modules remain untouched).*
*   **Granular Targets:**
    ```bash
    make clean
    make -j$(nproc)               # Compile the core launcher
    make patch                    # Patch stock OFW binaries
    make flash-all                # Flash chainloader and patched internal OFWs
    make push-modules             # Deploy dynamic modules and language packs
    ```

---

## Key Features

*   **Triple-Boot Multi-Boot System:** Boots and switches between three main targets: the custom Retro-Go launcher (in Bank 1), the patched stock Official Nintendo Firmware (OFW, Mario or Zelda Edition in Bank 2), and other external payloads. (Requires copy of both official firmware binaries and Retro-Go).
*   **Fast Boot:** Near-instant boot with display off, preserving the stock Game & Watch power-up feel. LCD initializes only when opening the menu or performing diagnostic/management operations.
*   **Dynamic Module Plugin System:** Runs optional modules (like custom themes, audio players, or filesystem utilities) dynamically from RAM, keeping the core system extremely compact.
*   **Storage & Partition Inspection:** Features an on-screen Partition Viewer that scans and identifies internal/external flash regions and partitions.
*   **File Manager:** A file browser for copying, pasting, and deleting files on internal partitions or external SD cards.
*   **Multi-Language UI & Font Streaming:** Supports 18 languages with dynamic font loading from flash, including Right-to-Left (RTL) mirror support for Arabic and Farsi.
*   **Authentic Game Themes:** Displays menus with sprites and color palettes extracted from stock firmware binaries.

---

## Repository Structure

*   **[src/chainloader/](src/chainloader/)**: Core chainloader codebase (GPIO, clocks, LTDC driver, window manager, and boot orchestrator).
*   **[src/patch/](src/patch/)**: Recovery hooks and input overrides injected into the stock OFW binaries.
*   **[src/common/](src/common/)**: Registers, boot magic definitions, and hardware bank-swapping utilities.
*   **[src/modules/](src/modules/)**: Relocatable PIE module plugins (filesystem drivers, theme sprite engines, and language subsystems).
*   **[src/fastcap/](src/fastcap/)**: Framebuffer capture utility code.
*   **[scripts/debug/](scripts/debug/)**: Hardware debugging, memory inspection, and profiling tools.
*   **[gnwmanager/](gnwmanager/)**: Git submodule for host-to-device communication.
*   **deps/**: Pinned third-party code (CMSIS, STM32 HAL, FatFs, and LittleFS).

---

## Design & Architecture

For details regarding internal memory mapping, flash offsets, relocatable loader internals, and register-level operations, see **[DESIGN.md](DESIGN.md)**.

---

## Related Projects & Libraries

*   **[game-and-watch-bootloader](https://github.com/sylverb/game-and-watch-bootloader)**
*   **[game-and-watch-retro-go](https://github.com/sylverb/game-and-watch-retro-go-sd)**
*   **[game-and-watch-patch](https://github.com/brianpugh/game-and-watch-patch)**
*   **[gnwmanager](https://github.com/brianpugh/gnwmanager)**

---

## Special Thanks

To the developers and reverse engineers who did the core reverse engineering and built the Game & Watch hacking community:
*   Brian Pugh
*   Sylver Bruneau
*   Marian Muller
*   Thomas Roth
*   Kai Beckmann

---

## Development Notes

### Re-Purposed Code
*   **Display & Clock Parameters:** The clock tree configuration, LCD timings, and OSPI registers were referenced and adapted from `game-and-watch-retro-go-sd` to match the STM32H7 hardware specifications.
*   **Bank-Swapping & Patches:** The hardware bank-swapping register sequence and stock OFW patching hooks were re-purposed from Marian Muller's `game-and-watch-patch` fork.

### Third-Party Libraries
*   **FatFs:** FatFs FAT file system module used for SD card reads/writes (ChaN).
*   **LittleFS:** littlefs file system used for internal flash read/write access (ARM).
*   **miniz / tinfl:** Streaming inflate decompression algorithm used for PNG images (Rich Geldreich).
*   **TJpgDec:** Tiny JPEG Decompressor used for JPEG images (ChaN).
*   **minimp3:** Minimal MP3 audio decoder (lieff).
*   **LZMA SDK:** LZMA decompression routines used for asset unpacking (Igor Pavlov).
*   **STM32H7 HAL & CMSIS:** Device drivers and registers (STMicroelectronics).

### Fonts
*   **Fusion Pixel:** A pixel font used for ASCII, Japanese, Chinese, and Korean scripts (takwolf, SIL OFL 1.1).
*   **Vazirmatn:** A Persian/Arabic outline font used for Arabic and Farsi scripts (Rastikerdar, SIL OFL 1.1).

### Engineering & Review
*   **AI-Assisted Engineering:** The software architecture, relocatable module loader interfaces, on-hardware debugging routines, and translation layout engine were designed and iterated with substantial AI assistance.

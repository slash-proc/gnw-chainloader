# gnw-chainloader Screenshots

This folder contains on-device screenshots showcasing the main menu, settings, built-in utilities, and relocatable feature modules. All captures are native 320×240 resolution.

---

### Main Menu
The launcher menu boots with custom sprites and color palettes dynamically extracted from official Nintendo firmware binaries. If no official graphics assets are found, it falls back to a clean, basic design.

| | |
| :---: | :---: |
| ![Main Menu (Zelda)](Main%20Menu.png) | ![Main Menu (Fallback)](Main%20Menu%20(Fallback%20Design).png) |

---

### File Browser
A capability-gated, instanceable file browser showing directory traversal, file sizes, and options for copying, pasting, and deleting files across internal partitions and external SD cards.

| | |
| :---: | :---: |
| ![File Browser 1](File%20Browser%201.png) | ![File Browser 2](File%20Browser%202.png) |
| ![File Browser 3](File%20Browser%203.png) | ![File Browser (Arabic)](File%20Browser%20(Arabic).png) |

---

### Partition Viewer
Scans internal and external flash storage regions and details partition mappings, filesystem formats (LittleFS, FrogFS, FAT), block counts, and free space.

| | |
| :---: | :---: |
| ![Partition Viewer 1](Partition%20Viewer%201.png) | ![Partition Viewer 2](Partition%20Viewer%202.png) |
| ![Partition Viewer 3](Partition%20Viewer%203.png) | ![Partition Viewer 4](Partition%20Viewer%204.png) |
| ![Partition Viewer 5](Partition%20Viewer%205.png) | ![Partition Viewer 6](Partition%20Viewer%206.png) |

---

### Relocatable Feature Modules
Optional features run as transient position-independent executable (PIE) binary plugins loaded dynamically from RAM.

#### MP3 Player
A localized feature module playing MP3 files from the SD card using register-level SAI/DMA and the minimp3 library. Shows visual playlist controls and track queues (Japanese locale shown).

| | | |
| :---: | :---: | :---: |
| ![MP3 Player (Japanese)](MP3%20Player%20(Japanese).png) | ![MP3 Player Playlist](MP3%20Player%20Playlist.png) | ![MP3 Player Playing](MP3%20Player%20Playing.png) |

#### Picture Viewer
A relocatable gallery module decompressing PNG, BMP (including 16-bit RGB565 screenshots), and hardware-accelerated JPEG files (Russian locale shown).

![Picture Viewer (Russian)](Picture%20Viewer%20(Russian).png)

#### Module Overview
A Tools diagnostic listing loaded modules, their memory footprint, and base execution addresses.

![Module Overview](Module%20Overview.png)

---

### Settings
The settings menu provides runtime configuration options, including a live language selector and dynamic font loading.

| | | |
| :---: | :---: | :---: |
| ![Settings (English)](Settings%20(English).png) | ![Settings (Spanish)](Settings%20(Spanish).png) | ![Settings (Chinese)](Settings%20(Chinese).png) |

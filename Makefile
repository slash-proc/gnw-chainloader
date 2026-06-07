TARGET = gnw_chainloader

DEBUG ?= 1
OPT ?= -Os

BUILD_DIR = build
STUB_BUILD_DIR = $(BUILD_DIR)/stub
APP_BUILD_DIR = $(BUILD_DIR)/app

# FatFs is staged here (see the FS Drivers section for why/how) so a project-owned
# ffconf.h can shadow the submodule's without editing it. Defined early because
# BOTH the in-core RO FatFs (app) and the RW driver build from these staged files.
FATFS_SRC_DIR = $(BUILD_DIR)/fatfs_src
FATFS_STAGED_HDRS = $(FATFS_SRC_DIR)/ff.h $(FATFS_SRC_DIR)/diskio.h $(FATFS_SRC_DIR)/ffconf.h

# gnwmanager regenerates this cached file on every invocation, dirtying the
# submodule working tree. Use $(RESTORE_KEYSTONE) (with a leading '-' so make
# ignores failures) at the end of any recipe that runs gnwmanager.
KEYSTONE_CACHE   = gnwmanager/cli/gnw_patch/keystone_cache.json
RESTORE_KEYSTONE = git -C gnwmanager restore $(KEYSTONE_CACHE)

# HAL sources for the stub (minimal set)
STUB_HAL_SOURCES = \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c

# HAL sources for the main app
HAL_SOURCES = \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ospi.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ltdc.c \
deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c

STUB_C_SOURCES = \
src/chainloader/stub_main.c \
src/chainloader/board.c \
src/chainloader/system_stm32h7xx.c \
deps/Core/Src/LzmaDec.c \
$(STUB_HAL_SOURCES)

APP_SOURCES := \
src/chainloader/main.c \
src/chainloader/board.c \
src/chainloader/assets.c \
src/chainloader/assets_gen.c \
src/chainloader/system/utils.c \
src/chainloader/system/input.c \
src/chainloader/system/loader.c \
src/chainloader/system/feature.c \
src/chainloader/system/crash_log.c \
src/chainloader/system/ofw_verify.c \
src/chainloader/ui/ui_manager.c \
src/chainloader/ui/ui_list.c \
src/chainloader/ui/gui_font.c \
src/chainloader/ui/strings.c \
src/chainloader/ui/i18n.c \
src/chainloader/ui/partition_viewer.c \
src/chainloader/ui/ui_file_browser.c \
src/chainloader/storage/partition.c \
src/chainloader/storage/lfs_wrapper.c \
src/chainloader/storage/vfs.c \
src/chainloader/storage/sdcard.c \
deps/littlefs/lfs.c \
deps/littlefs/lfs_util.c \
src/chainloader/gui.c \
src/chainloader/gui_text.c \
src/chainloader/menu.c \
src/chainloader/system_stm32h7xx.c \
deps/Core/Src/flash.c \
$(HAL_SOURCES)

COMMON_ASM_SOURCES = src/chainloader/startup.s

PREFIX = arm-none-eabi-
ifdef GCC_PATH
CC = $(GCC_PATH)/$(PREFIX)gcc
AS = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
OBJCOPY = $(GCC_PATH)/$(PREFIX)objcopy
SZ = $(GCC_PATH)/$(PREFIX)size
else
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
OBJCOPY = $(PREFIX)objcopy
SZ = $(PREFIX)size
endif
BIN = $(OBJCOPY) -O binary -S

CPU = -mcpu=cortex-m7
FPU = -mfpu=fpv5-d16
FLOAT-ABI = -mfloat-abi=hard
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

C_DEFS = \
-DUSE_HAL_DRIVER \
-DSTM32H7B0xx \
-DHEADLESS=0 \
-DHAL_ADC_MODULE_ENABLED \
-DSDCARD_SPI1 \
-DMODULE_LFS_SIZE=$(LFS_SIZE) \
-DNDEBUG

C_INCLUDES = \
-I. \
-Isrc/chainloader \
-Isrc/chainloader/ui \
-Isrc/chainloader/storage \
-Isrc/chainloader/system \
-Ideps/littlefs \
-Ideps/FatFs \
-Ideps/Core/Inc \
-Ideps/Core/Src \
-Ideps/Drivers/STM32H7xx_HAL_Driver/Inc \
-Ideps/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
-Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include \
-Ideps/Drivers/CMSIS/Include

CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fomit-frame-pointer -fmerge-all-constants -g -DLZMA_ONESHOT -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_TRACE -fno-builtin-strchr -DLFS_READONLY
ASFLAGS = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections -g

LIBS = -lc -lm -lnosys
LDFLAGS = $(MCU) -specs=nano.specs $(LIBS) -Wl,--gc-sections -flto

STUB_LDFLAGS = $(LDFLAGS) -Tlinker/STM32H7B0_FLASH_STUB.ld -Wl,-Map=$(STUB_BUILD_DIR)/stub.map
APP_LDFLAGS  = $(LDFLAGS) -ffixed-r9 -Tlinker/STM32H7B0_RAM_APP.ld -Wl,-Map=$(APP_BUILD_DIR)/app.map

STUB_OBJECTS = $(addprefix $(STUB_BUILD_DIR)/,$(STUB_C_SOURCES:.c=.o))
STUB_OBJECTS += $(addprefix $(STUB_BUILD_DIR)/,$(COMMON_ASM_SOURCES:.s=.o))

APP_OBJECTS = $(addprefix $(APP_BUILD_DIR)/,$(APP_SOURCES:.c=.o))
APP_OBJECTS += $(addprefix $(APP_BUILD_DIR)/,$(COMMON_ASM_SOURCES:.s=.o))

# default action
all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/fatfs.bin $(BUILD_DIR)/lfs_rw.bin $(BUILD_DIR)/theme.bin $(BUILD_DIR)/language.bin $(BUILD_DIR)/installer.bin $(BUILD_DIR)/fileops.bin $(BUILD_DIR)/example.bin $(BUILD_DIR)/example_set.bin $(BUILD_DIR)/mp3.bin $(BUILD_DIR)/picture.bin $(BUILD_DIR)/modview.bin

# Asset Cooking
src/chainloader/assets_gen.c src/chainloader/assets_gen.h: scripts/build/cook_assets.py src/chainloader/mario_tiles.json src/chainloader/zelda_tiles_v3.json | $(BUILD_DIR)
	@echo "COOK assets"
	@python3 scripts/build/cook_assets.py --out-c src/chainloader/assets_gen.c --out-h src/chainloader/assets_gen.h

# RAM Application Build
# -ffixed-r9: the RAM app reserves r9 so the core never clobbers an r9-pic (XIP) feature module's
# GOT base (the module addresses its GOT via r9, set by the loader at each core->module entry).
# Must match the app LTO link (APP_LDFLAGS). Not on the stub (runs no modules, at the flash ceiling).
$(APP_BUILD_DIR)/%.o: %.c src/chainloader/assets_gen.h | $(APP_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "CC $< (app)"
	@$(CC) -c $(CFLAGS) -ffixed-r9 -DVECT_TAB_SRAM $< -o $@

# ofw_verify.c bakes in the committed CRC headers. There are no header depfiles, so name them
# explicitly: a re-baked CRC (make ofw-crc / retrogo-crc) must rebuild this object, or the binary
# ships a stale CRC and the OFW/Retro-Go gates misfire. (Merges with the pattern rule above.)
$(APP_BUILD_DIR)/src/chainloader/system/ofw_verify.o: src/common/ofw_crc.h src/common/retrogo_crc.h

$(APP_BUILD_DIR)/%.o: %.s | $(APP_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "AS $< (app)"
	@$(AS) -c $(ASFLAGS) $< -o $@

# In-core read-only FatFs (linked into the app). Compiled from the STAGED FatFs
# sources (build/fatfs_src, see the driver section) with the RO config so
# f_write/exFAT are removed and the FATFS/FIL/DIR struct layouts match across
# ff.c, the diskio and the wrapper. -I$(FATFS_SRC_DIR) comes FIRST so the
# unqualified #include "ff.h"/"ffconf.h" resolve to our staged shadow, not the
# submodule. fatfs_diskio.c / fatfs_ro.c are built ONLY here (not in APP_SOURCES)
# precisely because they need this RO config + staging include.
APP_FATFS_RO_DIR = $(APP_BUILD_DIR)/fatfs_ro
# Bare-bones in-core FAT: read-only, no exFAT, NO long filenames (drops ffunicode
# entirely and the LFN code in ff.c, ~2.6 KB + ~0.5 KB RAM), minimized API
# (opendir/readdir/open/read only), single SBCS code page. This is just a
# bootstrap reader for the card; the FULL FatFs (LFN + RW + exFAT) ships as a PIE
# module on the LittleFS. Consequence: FAT files show 8.3 short names.
APP_FATFS_RO_CFLAGS = -I$(FATFS_SRC_DIR) $(CFLAGS) -ffixed-r9 -DVECT_TAB_SRAM -DFF_FS_READONLY=1 -DFF_FS_EXFAT=0 -DFF_USE_LFN=0 -DFF_FS_MINIMIZE=1 -DFF_CODE_PAGE=437
APP_FATFS_RO_OBJS = \
$(APP_FATFS_RO_DIR)/ff.o \
$(APP_FATFS_RO_DIR)/ffunicode.o \
$(APP_FATFS_RO_DIR)/fatfs_diskio.o \
$(APP_FATFS_RO_DIR)/fatfs_ro.o

$(APP_FATFS_RO_DIR)/ff.o: $(FATFS_SRC_DIR)/ff.c $(FATFS_STAGED_HDRS) | $(APP_FATFS_RO_DIR)
	@echo "CC ff.c (fatfs_ro, in-core RO)"
	@$(CC) -c $(APP_FATFS_RO_CFLAGS) $< -o $@

$(APP_FATFS_RO_DIR)/ffunicode.o: $(FATFS_SRC_DIR)/ffunicode.c $(FATFS_STAGED_HDRS) | $(APP_FATFS_RO_DIR)
	@echo "CC ffunicode.c (fatfs_ro, in-core RO)"
	@$(CC) -c $(APP_FATFS_RO_CFLAGS) $< -o $@

$(APP_FATFS_RO_DIR)/%.o: src/chainloader/storage/%.c $(FATFS_STAGED_HDRS) | $(APP_FATFS_RO_DIR)
	@echo "CC $< (fatfs_ro, in-core RO)"
	@$(CC) -c $(APP_FATFS_RO_CFLAGS) $< -o $@

$(APP_BUILD_DIR)/app.elf: $(APP_OBJECTS) $(APP_FATFS_RO_OBJS)
	@echo "LD $@"
	@$(CC) $(APP_OBJECTS) $(APP_FATFS_RO_OBJS) $(APP_LDFLAGS) -o $@
	@$(SZ) $@

$(APP_BUILD_DIR)/app.bin: $(APP_BUILD_DIR)/app.elf
	@echo "BIN $@"
	@$(BIN) $< $@

$(BUILD_DIR)/app.bin.lzma: $(APP_BUILD_DIR)/app.bin
	@echo "LZMA $@"
	@xz -f -c --format=raw --armthumb --lzma1=dict=128KiB,lc=1,lp=1,pb=1 $< > $@

$(BUILD_DIR)/app.bin.lzma.o: $(BUILD_DIR)/app.bin.lzma
	@echo "OBJCOPY $@"
	@$(OBJCOPY) -I binary -O elf32-littlearm -B arm \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

# Flash Stub Build
$(STUB_BUILD_DIR)/%.o: %.c | $(STUB_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "CC $< (stub)"
	@$(CC) -c $(CFLAGS) -DSTUB $< -o $@

$(STUB_BUILD_DIR)/%.o: %.s | $(STUB_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "AS $< (stub)"
	@$(AS) -c $(ASFLAGS) -DSTUB $< -o $@

$(STUB_BUILD_DIR)/stub.elf: $(STUB_OBJECTS) $(BUILD_DIR)/app.bin.lzma.o
	@echo "LD $@"
	@$(CC) $(STUB_OBJECTS) $(BUILD_DIR)/app.bin.lzma.o $(STUB_LDFLAGS) -o $@
	@$(SZ) $@

$(BUILD_DIR)/$(TARGET).bin: $(STUB_BUILD_DIR)/stub.elf | $(BUILD_DIR)
	@echo "BIN $@"
	@$(BIN) $< $@
	@SIZE=$$(stat -c%s $@); \
	echo; \
	echo "Final Bin size: $$SIZE"; \
	echo "Free space: $$(( 40*1024 - $$SIZE))"; \
	if [ $$SIZE -gt 40959 ]; then \
		echo "ERROR: Binary size exceeds 40960 byte (40K) limit!"; \
		exit 1; \
	fi

# PIE module builds
MODULE_BUILD_DIR = $(BUILD_DIR)/modules
MODULE_FATFS_OBJS = \
$(MODULE_BUILD_DIR)/fatfs/module_entry.o \
$(MODULE_BUILD_DIR)/fatfs/ff.o \
$(MODULE_BUILD_DIR)/fatfs/ffunicode.o

# FatFs is staged into $(FATFS_SRC_DIR) so that a project-owned ffconf.h
# (src/common/fatfs/ffconf.h) shadows the submodule's WITHOUT editing the
# submodule. ff.h does an unqualified  #include "ffconf.h" , which the compiler
# resolves from the includer's OWN directory first — so an -I path cannot win
# (and -I- is obsolete). Copying ff.c/ff.h/ffunicode.c/diskio.h next to our
# ffconf.h makes current-dir-first resolve "ffconf.h" to ours; -D flags then
# select the RO (core) vs full RW+exFAT (module) variant from the guarded knobs.
# (FATFS_SRC_DIR / FATFS_STAGED_HDRS are defined near the top of this file.)

# Project shadow config (explicit rule: wins over the generic copy rule below).
$(FATFS_SRC_DIR)/ffconf.h: src/common/fatfs/ffconf.h | $(FATFS_SRC_DIR)
	@echo "STAGE ffconf.h (project shadow)"
	@cp $< $@

# All other FatFs files copied verbatim from the untouched submodule.
$(FATFS_SRC_DIR)/%: deps/FatFs/% | $(FATFS_SRC_DIR)
	@cp $< $@

# module_entry.c also #include "ff.h"; -I$(FATFS_SRC_DIR) (before -Ideps/FatFs)
# makes it see the SAME staged ff.h/ffconf.h as ff.c, so struct layouts match.
# PIE module like lfs_rw: -fPIC + -DMODULE_BUILD (header stub) + module.ld -pie so
# the loader relocates it to a bump-allocated address (full RW+exFAT FatFs here).
MODULE_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG -DMODULE_BUILD -DMODULE_VERSION=1 -fPIC -Isrc/chainloader -I$(FATFS_SRC_DIR) -Ideps/FatFs -Ideps/Core/Inc -Ideps/Drivers/STM32H7xx_HAL_Driver/Inc -Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include -Ideps/Drivers/CMSIS/Include -Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
MODULE_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -fPIC -pie -Tsrc/modules/module.ld

$(MODULE_BUILD_DIR)/fatfs/%.o: src/modules/filesystems/fatfs/%.c $(FATFS_STAGED_HDRS) | $(MODULE_BUILD_DIR)/fatfs
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(MODULE_CFLAGS) $< -o $@

# FatFs library objects, compiled from the staged copies (full RW+exFAT here).
$(MODULE_BUILD_DIR)/fatfs/ff.o: $(FATFS_SRC_DIR)/ff.c $(FATFS_STAGED_HDRS) | $(MODULE_BUILD_DIR)/fatfs
	@mkdir -p $(dir $@)
	@echo "CC ff.c (fatfs driver, staged RW)"
	@$(CC) -c $(MODULE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/fatfs/ffunicode.o: $(FATFS_SRC_DIR)/ffunicode.c $(FATFS_STAGED_HDRS) | $(MODULE_BUILD_DIR)/fatfs
	@mkdir -p $(dir $@)
	@echo "CC ffunicode.c (fatfs driver, staged RW)"
	@$(CC) -c $(MODULE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/fatfs/fatfs.elf: $(MODULE_FATFS_OBJS)
	@echo "LD $@"
	@$(CC) $(MODULE_FATFS_OBJS) $(MODULE_LDFLAGS) -o $@

$(BUILD_DIR)/fatfs.bin: $(MODULE_BUILD_DIR)/fatfs/fatfs.elf
	@echo "BIN $@"
	@$(BIN) $< $@

MODULE_LFS_OBJS = \
$(MODULE_BUILD_DIR)/lfs_rw/module_entry.o \
$(MODULE_BUILD_DIR)/lfs_rw/lfs.o \
$(MODULE_BUILD_DIR)/lfs_rw/lfs_util.o

# PIE module: -fPIC (NOT -msingle-pic-base, so no r9), -DMODULE_BUILD enables the
# header stub; linked -pie with the shared module.ld so the linker emits
# R_ARM_RELATIVE entries the loader patches by the load address.
MODULE_LFS_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG -DMODULE_BUILD -DMODULE_VERSION=1 -fPIC -Isrc/chainloader -Ideps/littlefs -Ideps/Core/Inc -Ideps/Drivers/STM32H7xx_HAL_Driver/Inc -Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include -Ideps/Drivers/CMSIS/Include -Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-strchr -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_TRACE
MODULE_LFS_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -fPIC -pie -Tsrc/modules/module.ld

$(MODULE_BUILD_DIR)/lfs_rw/%.o: src/modules/filesystems/lfs_rw/%.c | $(MODULE_BUILD_DIR)/lfs_rw
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(MODULE_LFS_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/lfs_rw/%.o: deps/littlefs/%.c | $(MODULE_BUILD_DIR)/lfs_rw
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(MODULE_LFS_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/lfs_rw/lfs_rw.elf: $(MODULE_LFS_OBJS)
	@echo "LD $@"
	@$(CC) $(MODULE_LFS_OBJS) $(MODULE_LFS_LDFLAGS) -o $@

$(BUILD_DIR)/lfs_rw.bin: $(MODULE_BUILD_DIR)/lfs_rw/lfs_rw.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Theme sprite module (PIE): carries its own blitter + sprite recipes and the
# per-OFW sprite themes (fairy/coin/Yoshi). Reads the framebuffer + OFW tileset/
# palette via the theme_host_api_t; registers themes for the loaded OFW.
MODULE_THEME_OBJS = \
$(MODULE_BUILD_DIR)/theme/module_entry.o \
$(MODULE_BUILD_DIR)/theme/assets_gen.o

MODULE_THEME_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG -DMODULE_BUILD -DMODULE_VERSION=1 -fPIC -Isrc/chainloader -Ideps/Core/Inc -Ideps/Drivers/STM32H7xx_HAL_Driver/Inc -Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include -Ideps/Drivers/CMSIS/Include -Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
MODULE_THEME_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -fPIC -pie -Tsrc/modules/module.ld

$(MODULE_BUILD_DIR)/theme/module_entry.o: src/modules/theme/module_entry.c src/chainloader/assets_gen.h | $(MODULE_BUILD_DIR)/theme
	@mkdir -p $(dir $@)
	@echo "CC $< (theme module)"
	@$(CC) -c $(MODULE_THEME_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/theme/assets_gen.o: src/chainloader/assets_gen.c src/chainloader/assets_gen.h | $(MODULE_BUILD_DIR)/theme
	@mkdir -p $(dir $@)
	@echo "CC assets_gen.c (theme module)"
	@$(CC) -c $(MODULE_THEME_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/theme/theme.elf: $(MODULE_THEME_OBJS)
	@echo "LD $@"
	@$(CC) $(MODULE_THEME_OBJS) $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/theme.bin: $(MODULE_BUILD_DIR)/theme/theme.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Language module — the former ui/i18n.c + ui/font_ext.c (discovery, pack +
# script-font load, UTF-8 render, switching; SD install now in the installer module).
# Reuses the theme module flags.
$(MODULE_BUILD_DIR)/language/%.o: src/modules/language/%.c | $(MODULE_BUILD_DIR)/language
	@mkdir -p $(dir $@)
	@echo "CC $< (language module)"
	@$(CC) -c $(MODULE_THEME_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/language/language.elf: $(MODULE_BUILD_DIR)/language/module_entry.o $(MODULE_BUILD_DIR)/language/lang_mgr.o $(MODULE_BUILD_DIR)/language/font_ext.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/language.bin: $(MODULE_BUILD_DIR)/language/language.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Installer module (PIE, transient) — SD->LittleFS install of both language packs and
# modules, moved out of the resident language module. Built MOD_FLAG_TRANSIENT so the
# core reclaims its pool slot after each use. Reuses the theme module's flags plus the
# transient flag.
MODULE_INSTALLER_CFLAGS = $(MODULE_THEME_CFLAGS) -DMODULE_FLAGS=MOD_FLAG_TRANSIENT

$(MODULE_BUILD_DIR)/installer/module_entry.o: src/modules/installer/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (installer module)"
	@$(CC) -c $(MODULE_INSTALLER_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/installer/installer.elf: $(MODULE_BUILD_DIR)/installer/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/installer.bin: $(MODULE_BUILD_DIR)/installer/installer.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# File-operations module (PIE, transient) — recursive folder copy / delete / tree-
# size, moved out of the in-core file browser so the 40 KiB core never carries them.
# Loaded on demand for one heavy op, then reclaimed. Reuses the theme module's flags
# plus the transient flag.
MODULE_FILEOPS_CFLAGS = $(MODULE_THEME_CFLAGS) -DMODULE_FLAGS=MOD_FLAG_TRANSIENT

$(MODULE_BUILD_DIR)/fileops/module_entry.o: src/modules/fileops/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (fileops module)"
	@$(CC) -c $(MODULE_FILEOPS_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/fileops/fileops.elf: $(MODULE_BUILD_DIR)/fileops/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/fileops.bin: $(MODULE_BUILD_DIR)/fileops/fileops.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Example feature module (PIE, transient) -- proves the feature-module framework
# (see DESIGN.md §5). Its header manifest (-DMODULE_MENU_*) declares a
# Tools entry the core discovers + lists with NO module-specific core code. Template
# for real feature modules (e.g. the MP3 player). Reuses the theme module's flags.
MODULE_EXAMPLE_CFLAGS = $(MODULE_THEME_CFLAGS) -DMODULE_FLAGS=MOD_FLAG_TRANSIENT \
    -DMODULE_MENU_ID=MODULE_MENU_TOOLS -DMODULE_MENU_LABEL='"Example"'

$(MODULE_BUILD_DIR)/features/example/module_entry.o: src/modules/features/example/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (example feature module)"
	@$(CC) -c $(MODULE_EXAMPLE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/example/example.elf: $(MODULE_BUILD_DIR)/features/example/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/example.bin: $(MODULE_BUILD_DIR)/features/example/example.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Module Overview feature module: a Tools diagnostic that lists the loaded modules (source / pool
# RAM / state / load model), read from the loader registry via the feature host. English-only, so
# no strings. Rebuilt AS a module (the in-core view is disabled to hold the 40K stub ceiling).
# Module Overview is a trivial list view (no latency-sensitive loops), so it XIPs from the store like mp3.
MODULE_MODOVERVIEW_CFLAGS = $(MODULE_THEME_CFLAGS) $(MODULE_XIP) -DMODULE_FLAGS='(MOD_FLAG_TRANSIENT|MOD_FLAG_R9_PIC)' \
    -DMODULE_MENU_ID=MODULE_MENU_TOOLS -DMODULE_MENU_LABEL='"Module Overview"'
MODULE_MODOVERVIEW_LDFLAGS = $(MODULE_THEME_LDFLAGS) $(MODULE_XIP)

$(MODULE_BUILD_DIR)/features/modoverview/module_entry.o: src/modules/features/modoverview/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (modoverview feature module)"
	@$(CC) -c $(MODULE_MODOVERVIEW_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/modoverview/modoverview.elf: $(MODULE_BUILD_DIR)/features/modoverview/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_MODOVERVIEW_LDFLAGS) -o $@

# The DEVICE filename must fit FAT 8.3 (modview.bin): the in-core RO FAT reader has LFN disabled, so
# a long name ("modoverview.bin") gets a long-filename entry it can't resolve at discovery/load.
$(BUILD_DIR)/modview.bin: $(MODULE_BUILD_DIR)/features/modoverview/modoverview.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Settings variant of the example feature module — same source, header manifest set to
# MODULE_MENU_SETTINGS so it proves the Settings splice (entry lands between Fast-Boot and
# Reset Defaults, Reset stays last). Built into its own object dir so the -D difference
# from the Tools build can't collide.
MODULE_EXAMPLE_SET_CFLAGS = $(MODULE_THEME_CFLAGS) -DMODULE_FLAGS=MOD_FLAG_TRANSIENT \
    -DMODULE_MENU_ID=MODULE_MENU_SETTINGS -DMODULE_MENU_LABEL='"Demo Setting"'

$(MODULE_BUILD_DIR)/features/example_set/module_entry.o: src/modules/features/example/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (example feature module, Settings)"
	@$(CC) -c $(MODULE_EXAMPLE_SET_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/example_set/example_set.elf: $(MODULE_BUILD_DIR)/features/example_set/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/example_set.bin: $(MODULE_BUILD_DIR)/features/example_set/example_set.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# MP3 player feature module (PIE, transient) — first real feature module. Registers a Tools
# entry "MP3 Player" AND claims the .mp3 extension (browser dispatch). Two objects: the
# framework entry + a register-level SAI1/DMA audio driver (the 40K core has no HAL SAI/DMA,
# so the module brings the peripheral up itself). minimp3 (vendored, header-only) joins in
# the decode stage. Reuses the theme module's flags + own include dir.
# Compiled-in translations: cook_modstrings.py turns i18n/modules/mp3/*.json into a
# mp3_strings_gen.{c,h} matrix linked into mp3.bin (no device-side files). Regenerated
# from the JSONs (and by `make i18n`), compiled with the module's own PIE flags.
MP3_GEN = $(MODULE_BUILD_DIR)/features/mp3/mp3_strings_gen
# Shared FAT-XIP flag set, opted into per-module via MOD_FLAG_R9_PIC. -msingle-pic-base puts the GOT
# base in r9; -mno-pic-data-is-text-relative routes .data/.bss through the GOT too (MANDATORY: XIP
# splits .text in flash from .data/.bss in the RAM slot, so text-relative data would land in flash
# and bus-fault). Must be on the -flto link too.
MODULE_XIP = -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative
# MP3 player is FAT-XIP: the decode runs fine from flash, the audio DMA buffer (RAM slot) absorbs the latency.
MODULE_MP3_CFLAGS = $(MODULE_THEME_CFLAGS) $(MODULE_XIP) -Isrc/modules/features/mp3 -I$(MODULE_BUILD_DIR)/features/mp3 \
    -DMODULE_FLAGS='(MOD_FLAG_TRANSIENT|MOD_FLAG_R9_PIC)' \
    -DMODULE_MENU_ID=MODULE_MENU_TOOLS -DMODULE_MENU_LABEL='"MP3 Player"' -DMODULE_FILE_EXT='"mp3"'
MODULE_MP3_LDFLAGS = $(MODULE_THEME_LDFLAGS) $(MODULE_XIP)

# One recipe emits both .c and .h; depend .h on .c (not a dual-target rule) so parallel make
# doesn't run cook_modstrings twice and race.
$(MP3_GEN).c: $(wildcard i18n/modules/mp3/*.json) scripts/build/cook_modstrings.py
	@mkdir -p $(dir $@)
	@echo "GEN mp3 strings (i18n/modules/mp3/*.json)"
	@python3 scripts/build/cook_modstrings.py mp3 --out $(dir $@)
$(MP3_GEN).h: $(MP3_GEN).c
	@test -f $@

$(MODULE_BUILD_DIR)/features/mp3/module_entry.o: src/modules/features/mp3/module_entry.c $(MP3_GEN).h
	@mkdir -p $(dir $@)
	@echo "CC $< (mp3 feature module)"
	@$(CC) -c $(MODULE_MP3_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/mp3/audio_sai.o: src/modules/features/mp3/audio_sai.c
	@mkdir -p $(dir $@)
	@echo "CC $< (mp3 audio driver)"
	@$(CC) -c $(MODULE_MP3_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/mp3/mp3_strings_gen.o: $(MP3_GEN).c $(MP3_GEN).h
	@echo "CC $< (mp3 strings matrix)"
	@$(CC) -c $(MODULE_MP3_CFLAGS) $(MP3_GEN).c -o $@

$(MODULE_BUILD_DIR)/features/mp3/mp3.elf: $(MODULE_BUILD_DIR)/features/mp3/module_entry.o $(MODULE_BUILD_DIR)/features/mp3/audio_sai.o $(MODULE_BUILD_DIR)/features/mp3/mp3_strings_gen.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_MP3_LDFLAGS) -o $@

$(BUILD_DIR)/mp3.bin: $(MODULE_BUILD_DIR)/features/mp3/mp3.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Picture Viewer feature module (PIE, transient) — second real feature module. Registers a
# Tools entry "Picture Viewer" AND claims the .jpg extension (browser dispatch). Decodes JPEG
# with vendored TJpgDec (ChaN R0.03) straight into the framebuffer — no whole-image buffer, and
# oversized photos are descaled in-decode. Reuses the theme module's flags + own include dir.
# Compiled-in translations: cook_modstrings.py turns i18n/modules/picture/*.json into a
# picture_strings_gen.{c,h} matrix linked into picture.bin (no device-side files).
PIC_GEN = $(MODULE_BUILD_DIR)/features/picture/picture_strings_gen
# PICTURE_HW_JPEG=1 (default) decodes JPEGs on the STM32 JPEG hardware codec, falling back to
# software tjpgd for anything it can't handle (PNG, unsupported subsampling); =0 forces the
# pure-software path.
PICTURE_HW_JPEG ?= 1
# Picture is RAM-only (-fPIC full-copy): its JPEG/PNG decode is latency-sensitive, too slow run from
# flash. A module opts INTO FAT-XIP with MODULE_XIP + MOD_FLAG_R9_PIC (see the mp3 rule); picture doesn't.
MODULE_PICTURE_CFLAGS = $(MODULE_THEME_CFLAGS) -Isrc/modules/features/picture -I$(MODULE_BUILD_DIR)/features/picture \
    -DMODULE_FLAGS=MOD_FLAG_TRANSIENT -DPICTURE_HW_JPEG=$(PICTURE_HW_JPEG) \
    -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_MALLOC \
    -DMODULE_MENU_ID=MODULE_MENU_TOOLS -DMODULE_MENU_LABEL='"Picture Viewer"' -DMODULE_FILE_EXT='"jpg,jpeg,png,bmp"'

# One recipe emits both .c and .h; depend .h on .c so parallel make doesn't race cook_modstrings.
$(PIC_GEN).c: $(wildcard i18n/modules/picture/*.json) scripts/build/cook_modstrings.py
	@mkdir -p $(dir $@)
	@echo "GEN picture strings (i18n/modules/picture/*.json)"
	@python3 scripts/build/cook_modstrings.py picture --out $(dir $@)
$(PIC_GEN).h: $(PIC_GEN).c
	@test -f $@

$(MODULE_BUILD_DIR)/features/picture/module_entry.o: src/modules/features/picture/module_entry.c $(PIC_GEN).h
	@mkdir -p $(dir $@)
	@echo "CC $< (picture feature module)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/tjpgd.o: src/modules/features/picture/tjpgd.c
	@mkdir -p $(dir $@)
	@echo "CC $< (TJpgDec decoder)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/png.o: src/modules/features/picture/png.c
	@mkdir -p $(dir $@)
	@echo "CC $< (PNG decoder)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/miniz.o: src/modules/features/picture/miniz.c
	@mkdir -p $(dir $@)
	@echo "CC $< (miniz inflate)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/bmp.o: src/modules/features/picture/bmp.c
	@mkdir -p $(dir $@)
	@echo "CC $< (BMP decoder)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/jpeg_hw.o: src/modules/features/picture/jpeg_hw.c
	@mkdir -p $(dir $@)
	@echo "CC $< (HW JPEG decoder)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/features/picture/picture_strings_gen.o: $(PIC_GEN).c $(PIC_GEN).h
	@echo "CC $< (picture strings matrix)"
	@$(CC) -c $(MODULE_PICTURE_CFLAGS) $(PIC_GEN).c -o $@

$(MODULE_BUILD_DIR)/features/picture/picture.elf: $(MODULE_BUILD_DIR)/features/picture/module_entry.o $(MODULE_BUILD_DIR)/features/picture/tjpgd.o $(MODULE_BUILD_DIR)/features/picture/png.o $(MODULE_BUILD_DIR)/features/picture/bmp.o $(MODULE_BUILD_DIR)/features/picture/miniz.o $(MODULE_BUILD_DIR)/features/picture/jpeg_hw.o $(MODULE_BUILD_DIR)/features/picture/picture_strings_gen.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/picture.bin: $(MODULE_BUILD_DIR)/features/picture/picture.elf
	@echo "BIN $@"
	@$(BIN) $< $@

# Dummy module (PIE) — ON-DEVICE ABI-REJECTION TEST ONLY (scripts/tests/
# test_abi_reject.py). A do-nothing module rebuilt at a matching ABI
# (DUMMY_ABI=1, accepted) and a mismatched one (DUMMY_ABI=2 / 0, rejected) to
# prove the loader's module-ABI gate fires on hardware. Not built by `all`; never
# in a release. Reuses the theme module's flags plus the ABI override. (The
# harness removes the object between variants since make can't see the -D change.)
DUMMY_ABI ?= 1
MODULE_DUMMY_CFLAGS = $(MODULE_THEME_CFLAGS) -DMODULE_ABI_VERSION=$(DUMMY_ABI)

$(MODULE_BUILD_DIR)/dummy/module_entry.o: src/modules/dummy/module_entry.c
	@mkdir -p $(dir $@)
	@echo "CC $< (dummy module, ABI=$(DUMMY_ABI))"
	@$(CC) -c $(MODULE_DUMMY_CFLAGS) $< -o $@

$(MODULE_BUILD_DIR)/dummy/dummy.elf: $(MODULE_BUILD_DIR)/dummy/module_entry.o
	@echo "LD $@"
	@$(CC) $^ $(MODULE_THEME_LDFLAGS) -o $@

$(BUILD_DIR)/dummy.bin: $(MODULE_BUILD_DIR)/dummy/dummy.elf
	@echo "BIN $@"
	@$(BIN) $< $@

.PHONY: dummy
dummy: $(BUILD_DIR)/dummy.bin

$(BUILD_DIR) $(STUB_BUILD_DIR) $(APP_BUILD_DIR) $(MODULE_BUILD_DIR) $(MODULE_BUILD_DIR)/fatfs $(MODULE_BUILD_DIR)/lfs_rw $(MODULE_BUILD_DIR)/theme $(MODULE_BUILD_DIR)/language $(MODULE_BUILD_DIR)/fileops $(FATFS_SRC_DIR) $(APP_FATFS_RO_DIR):
	mkdir -p $@

clean:
	rm -fR $(BUILD_DIR)
	-$(RESTORE_KEYSTONE)

# Host unit tests (pure C, run on the build host — no device needed; link the
# real firmware sources). test_gui_text: the i18n text layer (utf8 / glyph /
# width / strings). test_abi_gate: the ABI gate (src/common/abi.h, the exact
# helpers loader.c / vfs.c call) rejects a mismatched module or .lang pack.
test-host: | $(BUILD_DIR)
	gcc -std=c11 -Wall -Wextra -Isrc/chainloader -Isrc/chainloader/ui \
		scripts/build/test_gui_text.c \
		src/chainloader/gui_text.c \
		src/chainloader/ui/gui_font.c \
		src/chainloader/ui/strings.c \
		src/modules/language/font_ext.c \
		-o $(BUILD_DIR)/test_gui_text
	gcc -std=c11 -Wall -Wextra -Isrc -Isrc/chainloader -Isrc/chainloader/ui \
		scripts/build/test_abi_gate.c \
		-o $(BUILD_DIR)/test_abi_gate
	@rc=0; $(BUILD_DIR)/test_gui_text || rc=1; $(BUILD_DIR)/test_abi_gate || rc=1; exit $$rc

# --- i18n asset generation ---------------------------------------------------
# Cook the language packs + per-script fonts from i18n/ sources into build/i18n/ —
# a clean SD mirror: <code>.lang + fonts/<script>.fnt (copy the folder onto an SD
# card's /i18n/ and the device installs them itself). Only English is baked into
# the firmware (ui/gui_font.c + ui/strings.c); every other language is data the
# device discovers from the packs. Requires host python3 + fontTools + Pillow. Not
# part of `all`; run when a translation, langs.json, or the string table changes.
I18N_OUT   = $(BUILD_DIR)/i18n
I18N_TMP   = $(BUILD_DIR)/i18n_tmp
# Font variant for both the in-core ASCII font and the external script fonts —
# must match across them so the shared baseline (GUI_FONT_REF_TOP) lines up.
FONT_STYLE = 12px-monospaced
COOK_FONT  = python3 scripts/build/cook_font.py --style $(FONT_STYLE)
COOK_LANG  = python3 scripts/build/cook_lang.py

# Regenerate the committed in-core ASCII font (ui/gui_font.{c,h}) from the OTF.
i18n-corefont:
	$(COOK_FONT) ascii

# Packs (build/i18n/*.lang) + per-script .chars (build/i18n_tmp), then the external
# script fonts (build/i18n/fonts/*.fnt).
i18n: | $(I18N_OUT)
	$(COOK_LANG) build
	$(COOK_FONT) blob --script latin
	$(COOK_FONT) blob --script ja      --chars $(I18N_TMP)/ja.chars
	$(COOK_FONT) blob --script zh_hans --chars $(I18N_TMP)/zh_hans.chars
	$(COOK_FONT) blob --script zh_hant --chars $(I18N_TMP)/zh_hant.chars
	$(COOK_FONT) blob --script ko      --chars $(I18N_TMP)/ko.chars
	python3 scripts/build/cook_modstrings.py mp3 --out $(MODULE_BUILD_DIR)/features/mp3 --chars-append $(I18N_TMP)/arabic.chars
	python3 scripts/build/cook_modstrings.py picture --out $(MODULE_BUILD_DIR)/features/picture --chars-append $(I18N_TMP)/arabic.chars
	$(COOK_FONT) blob --script arabic  --chars $(I18N_TMP)/arabic.chars

$(I18N_OUT):
	mkdir -p $@/fonts

# Deploy cooked i18n assets directly to the device LittleFS (/i18n/<code>.lang +
# /i18n/fonts/<script>.fnt), one chained gnwmanager session — the dev shortcut. For
# the real user flow, copy build/i18n/ onto an SD card's /i18n/ and let the device
# install them. Run after `make i18n` with the device attached.
ifeq ($(DOCKER),1)
push-i18n:
	$(IN_DOCKER)
else
push-i18n: i18n
	@LFS_SIZE=$(LFS_SIZE) python3 scripts/build/push_batched.py \
	    $(foreach f,$(wildcard $(I18N_OUT)/fonts/*.fnt),/i18n/fonts/$(notdir $(f))=$(f)) \
	    $(foreach f,$(wildcard $(I18N_OUT)/*.lang),/i18n/$(notdir $(f))=$(f))
	-$(RESTORE_KEYSTONE)
endif

.PHONY: all clean test-host i18n i18n-corefont push-i18n

# Patch payload (keep mostly unchanged)
PATCH_LDSCRIPT = src/patch/STM32H7B0VBTx_FLASH.ld
PATCH_CFLAGS = $(MCU) -Os -Wall -fdata-sections -ffunction-sections -nostdlib -Isrc/common -Ideps/Core/Src -Ideps/Core/Inc -DLZMA_NO_MAIN_H

# Debug/test remote-input instrumentation (scripts/debug/remote_input.py drives
# the UI over SWD). Opt-in only: `make REMOTE_INPUT=1 ...`. Off by default so the
# golden production build carries zero extra bytes under the hard flash ceiling.
# Remote input is a shipped feature (drive the UI over the probe), not just a
# debug aid, so it's ON by default. Opt out with `make REMOTE_INPUT=0 ...`.
REMOTE_INPUT ?= 1
ifneq ($(REMOTE_INPUT),0)
C_DEFS += -DREMOTE_INPUT
PATCH_CFLAGS += -DREMOTE_INPUT
endif

# On-device ABI-gate self-test hook (scripts/tests/test_abi_reject.py). Opt-in
# only: `make ABI_SELFTEST=1 ...`. Off by default so the release build carries no
# extra bytes. Compiles the menu.c hook that runs the real module + pack gates and
# leaves g_abi_selftest_mod / g_abi_selftest_pack for the harness to read.
ABI_SELFTEST ?= 0
ifneq ($(ABI_SELFTEST),0)
C_DEFS += -DABI_SELFTEST
endif

# Force the right-to-left UI mirror on (gui_rtl=1) regardless of the active language,
# so the RTL layout can be verified on hardware with English before any Arabic pack
# exists. Opt-in only: `make RTL_TEST=1 ...`. Off by default; zero bytes in a release.
RTL_TEST ?= 0
ifneq ($(RTL_TEST),0)
C_DEFS += -DRTL_TEST
endif

# Boot-timing instrumentation (scripts read g_boot_bench over SWD). Opt-in only:
# `make BOOT_BENCH=1 ...`. Off by default so the golden build carries zero bytes.
# See DESIGN.md §5.
ifdef BOOT_BENCH
C_DEFS += -DBOOT_BENCH
endif

# Crash-log self-test trigger (host writes a DTCM cell to deliberately fault, for
# verifying the D3 crash log via scripts/debug/crash_log.py). Opt-in only:
# `make CRASH_TEST=1 ...`. Off by default — the crash log + HardFault handler ship
# always-on; only the deliberate-fault hook is gated out.
ifdef CRASH_TEST
C_DEFS += -DCRASH_TEST
endif
PATCH_LDFLAGS = $(MCU) -nostdlib -T$(PATCH_LDSCRIPT) -Wl,--gc-sections \
                -Wl,--defsym=__RAM_ORIGIN__=0x30010000 -Wl,--defsym=__RAM_LENGTH__=0x10000 \
                -Wl,--undefined=memcpy_inflate \
                -Wl,--undefined=rwdata_inflate \
                -Wl,--undefined=bss_rwdata_init \
                -Wl,--undefined=bootloader \
                -Wl,--undefined=read_buttons \
                -Wl,--undefined=SMB1_ROM

$(BUILD_DIR)/gw_patch_mario.elf: src/patch/main.c deps/Core/Src/LzmaDec.c | $(BUILD_DIR)
	@echo "CC src/patch/main.c (mario)"
	@$(CC) -c $(PATCH_CFLAGS) -DSTOCK_RESET_HANDLER=0x08017a45 -DSTOCK_READ_BUTTONS=0x08010d49 src/patch/main.c -o $(BUILD_DIR)/gw_patch_mario.o
	@echo "CC deps/Core/Src/LzmaDec.c (mario)"
	@$(CC) -c $(PATCH_CFLAGS) deps/Core/Src/LzmaDec.c -o $(BUILD_DIR)/LzmaDec_mario.o
	@echo "LD $@"
	@$(CC) $(BUILD_DIR)/gw_patch_mario.o $(BUILD_DIR)/LzmaDec_mario.o $(PATCH_LDFLAGS) -Wl,--defsym=__STOCK_ROM_END__=0x18100 -o $@

$(BUILD_DIR)/gw_patch_mario.bin: $(BUILD_DIR)/gw_patch_mario.elf
	@echo "BIN $@"
	@$(BIN) $< $@

$(BUILD_DIR)/gw_patch_zelda.elf: src/patch/main.c deps/Core/Src/LzmaDec.c | $(BUILD_DIR)
	@echo "CC src/patch/main.c (zelda)"
	@$(CC) -c $(PATCH_CFLAGS) -DSTOCK_RESET_HANDLER=0x0801ad49 -DSTOCK_READ_BUTTONS=0x08016809 src/patch/main.c -o $(BUILD_DIR)/gw_patch_zelda.o
	@echo "CC deps/Core/Src/LzmaDec.c (zelda)"
	@$(CC) -c $(PATCH_CFLAGS) deps/Core/Src/LzmaDec.c -o $(BUILD_DIR)/LzmaDec_zelda.o
	@echo "LD $@"
	@$(CC) $(BUILD_DIR)/gw_patch_zelda.o $(BUILD_DIR)/LzmaDec_zelda.o $(PATCH_LDFLAGS) -Wl,--defsym=__STOCK_ROM_END__=0x1B3E0 -o $@

$(BUILD_DIR)/gw_patch_zelda.bin: $(BUILD_DIR)/gw_patch_zelda.elf
	@echo "BIN $@"
	@$(BIN) $< $@

include Makefile.common
include Makefile.patch

# ── QA test suite ───────────────────────────────────────────────────────────
# Each stage runs scripts/tests/run_suite.py, which self-heals missing OCR fonts
# (regenerates build/i18n via `make i18n`) and re-renders qa-report.html after the
# run -- so every stage leaves an up-to-date report and never fails just because
# `make clean` wiped something. Host tiers (L0/L1) need no device; the full / L2-L4
# stages drive the device over SWD (one ST-Link at a time).
QA_RUN = python3 scripts/tests/run_suite.py

# `make qa` dispatches on QA_SCOPE (uses the device BY DEFAULT, best-effort):
#   auto (default) -> full bench run if the programmer is free + a device is
#                     connected (pgrep openocd guards against collisions), else host tiers
#   full           -> force the full bench run (every applicable device test)
#   host-only      -> force L0+L1 only
# e.g. `make qa QA_SCOPE=full`.
QA_SCOPE ?= auto

.PHONY: qa qa-auto qa-full qa-host-only qa-l0 qa-l1 qa-l2 qa-l3 qa-l4 qa-report
qa: qa-$(QA_SCOPE)       # -> qa-auto (default), qa-full, or qa-host-only
qa-auto:                 # self-detect: full bench run if a device is connected (programmer free), else host tiers
	@$(QA_RUN) --auto
qa-full:                 # force the full bench run: every applicable device test (needs the programmer)
	@$(QA_RUN) --adaptive
qa-host-only:            # force L0+L1 host/build gates only, no device
	@$(QA_RUN) --tier L0,L1
qa-l0:                   # L0: host unit tests (settings-word, boot-magic, parse, offline OCR)
	@$(QA_RUN) --tier L0
qa-l1:                   # L1: build gates (size ceiling, determinism, build matrix)
	@$(QA_RUN) --tier L1
qa-l2:                   # L2: component device tests (needs the programmer)
	@$(QA_RUN) --tier L2
qa-l3:                   # L3: scenario device tests (needs the programmer)
	@$(QA_RUN) --tier L3
qa-l4:                   # L4: environment matrix sweep (needs the programmer)
	@$(QA_RUN) --tier L4
qa-report:               # re-render qa-report.html from accumulated results (host-only, runs nothing)
	@python3 scripts/build/render_qa_report.py

TARGET = gnw_chainloader

DEBUG ?= 1
OPT ?= -Os

BUILD_DIR = build
STUB_BUILD_DIR = $(BUILD_DIR)/stub
APP_BUILD_DIR = $(BUILD_DIR)/app

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
src/chainloader/ui/ui_manager.c \
src/chainloader/ui/ui_list.c \
src/chainloader/ui/gui_font.c \
src/chainloader/ui/partition_viewer.c \
src/chainloader/ui/ui_file_browser.c \
src/chainloader/storage/partition.c \
src/chainloader/storage/lfs_wrapper.c \
src/chainloader/storage/vfs.c \
src/chainloader/storage/frogfs_reader.c \
deps/littlefs/lfs.c \
deps/littlefs/lfs_util.c \
src/chainloader/gui.c \
src/chainloader/menu.c \
src/chainloader/system_stm32h7xx.c \
deps/Core/Src/LzmaDec.c \
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

CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -g -DLZMA_ONESHOT -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_TRACE -fno-builtin-strchr -DLFS_READONLY
ASFLAGS = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections -g

LIBS = -lc -lm -lnosys
LDFLAGS = $(MCU) -specs=nano.specs $(LIBS) -Wl,--gc-sections -flto

STUB_LDFLAGS = $(LDFLAGS) -TSTM32H7B0_FLASH_STUB.ld -Wl,-Map=$(STUB_BUILD_DIR)/stub.map
APP_LDFLAGS  = $(LDFLAGS) -TSTM32H7B0_RAM_APP.ld -Wl,-Map=$(APP_BUILD_DIR)/app.map

STUB_OBJECTS = $(addprefix $(STUB_BUILD_DIR)/,$(STUB_C_SOURCES:.c=.o))
STUB_OBJECTS += $(addprefix $(STUB_BUILD_DIR)/,$(COMMON_ASM_SOURCES:.s=.o))

APP_OBJECTS = $(addprefix $(APP_BUILD_DIR)/,$(APP_SOURCES:.c=.o))
APP_OBJECTS += $(addprefix $(APP_BUILD_DIR)/,$(COMMON_ASM_SOURCES:.s=.o))

# default action
all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/fatfs.bin $(BUILD_DIR)/lfs_rw.bin

# Asset Cooking
src/chainloader/assets_gen.c src/chainloader/assets_gen.h: scripts/build/cook_assets.py src/chainloader/mario_tiles.json src/chainloader/zelda_tiles_v3.json | $(BUILD_DIR)
	@echo "COOK assets"
	@python3 scripts/build/cook_assets.py --out-c src/chainloader/assets_gen.c --out-h src/chainloader/assets_gen.h

# RAM Application Build
$(APP_BUILD_DIR)/%.o: %.c src/chainloader/assets_gen.h | $(APP_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "CC $< (app)"
	@$(CC) -c $(CFLAGS) -DVECT_TAB_SRAM $< -o $@

$(APP_BUILD_DIR)/%.o: %.s | $(APP_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "AS $< (app)"
	@$(AS) -c $(ASFLAGS) $< -o $@

$(APP_BUILD_DIR)/app.elf: $(APP_OBJECTS)
	@echo "LD $@"
	@$(CC) $(APP_OBJECTS) $(APP_LDFLAGS) -o $@
	@$(SZ) $@

$(APP_BUILD_DIR)/app.bin: $(APP_BUILD_DIR)/app.elf
	@echo "BIN $@"
	@$(BIN) $< $@

$(BUILD_DIR)/app.bin.lzma: $(APP_BUILD_DIR)/app.bin
	@echo "LZMA $@"
	@xz -f -c --format=raw --lzma1=dict=128KiB $< > $@

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
	echo "Free space: $$(( 32*1024 - $$SIZE))"; \
	if [ $$SIZE -gt 32767 ]; then \
		echo "ERROR: Binary size exceeds 32767 byte limit!"; \
		exit 1; \
	fi

# FS Drivers Build
DRIVER_BUILD_DIR = $(BUILD_DIR)/drivers
DRIVER_FATFS_OBJS = \
$(DRIVER_BUILD_DIR)/fatfs/driver_entry.o \
$(DRIVER_BUILD_DIR)/fatfs/ff.o \
$(DRIVER_BUILD_DIR)/fatfs/ffunicode.o

DRIVER_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG -Isrc/chainloader -Ideps/FatFs -Ideps/Core/Inc -Ideps/Drivers/STM32H7xx_HAL_Driver/Inc -Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include -Ideps/Drivers/CMSIS/Include -Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
DRIVER_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -Tsrc/drivers/fs/fatfs/STM32H7B0_DRIVER.ld

$(DRIVER_BUILD_DIR)/fatfs/%.o: src/drivers/fs/fatfs/%.c | $(DRIVER_BUILD_DIR)/fatfs
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(DRIVER_CFLAGS) $< -o $@

$(DRIVER_BUILD_DIR)/fatfs/%.o: deps/FatFs/%.c | $(DRIVER_BUILD_DIR)/fatfs
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(DRIVER_CFLAGS) $< -o $@

$(DRIVER_BUILD_DIR)/fatfs/fatfs.elf: $(DRIVER_FATFS_OBJS)
	@echo "LD $@"
	@$(CC) $(DRIVER_FATFS_OBJS) $(DRIVER_LDFLAGS) -o $@

$(BUILD_DIR)/fatfs.bin: $(DRIVER_BUILD_DIR)/fatfs/fatfs.elf
	@echo "BIN $@"
	@$(BIN) $< $@

DRIVER_LFS_OBJS = \
$(DRIVER_BUILD_DIR)/lfs_rw/driver_entry.o \
$(DRIVER_BUILD_DIR)/lfs_rw/lfs.o \
$(DRIVER_BUILD_DIR)/lfs_rw/lfs_util.o

DRIVER_LFS_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG -Isrc/chainloader -Ideps/littlefs -Ideps/Core/Inc -Ideps/Drivers/STM32H7xx_HAL_Driver/Inc -Ideps/Drivers/CMSIS/Device/ST/STM32H7xx/Include -Ideps/Drivers/CMSIS/Include -Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_TRACE
DRIVER_LFS_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -Tsrc/drivers/fs/lfs_rw/STM32H7B0_DRIVER.ld

$(DRIVER_BUILD_DIR)/lfs_rw/%.o: src/drivers/fs/lfs_rw/%.c | $(DRIVER_BUILD_DIR)/lfs_rw
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(DRIVER_LFS_CFLAGS) $< -o $@

$(DRIVER_BUILD_DIR)/lfs_rw/%.o: deps/littlefs/%.c | $(DRIVER_BUILD_DIR)/lfs_rw
	@mkdir -p $(dir $@)
	@echo "CC $< (driver)"
	@$(CC) -c $(DRIVER_LFS_CFLAGS) $< -o $@

$(DRIVER_BUILD_DIR)/lfs_rw/lfs_rw.elf: $(DRIVER_LFS_OBJS)
	@echo "LD $@"
	@$(CC) $(DRIVER_LFS_OBJS) $(DRIVER_LFS_LDFLAGS) -o $@

$(BUILD_DIR)/lfs_rw.bin: $(DRIVER_BUILD_DIR)/lfs_rw/lfs_rw.elf
	@echo "BIN $@"
	@$(BIN) $< $@

$(BUILD_DIR) $(STUB_BUILD_DIR) $(APP_BUILD_DIR) $(DRIVER_BUILD_DIR) $(DRIVER_BUILD_DIR)/fatfs $(DRIVER_BUILD_DIR)/lfs_rw:
	mkdir -p $@

clean:
	rm -fR $(BUILD_DIR)
	-$(RESTORE_KEYSTONE)

.PHONY: all clean

# Patch payload (keep mostly unchanged)
PATCH_LDSCRIPT = src/patch/STM32H7B0VBTx_FLASH.ld
PATCH_CFLAGS = $(MCU) -Os -Wall -fdata-sections -ffunction-sections -nostdlib -Isrc/common -Ideps/Core/Src -Ideps/Core/Inc -DLZMA_NO_MAIN_H
PATCH_LDFLAGS = $(MCU) -nostdlib -T$(PATCH_LDSCRIPT) -Wl,--gc-sections \
                -Wl,--defsym=__RAM_ORIGIN__=0x30010000 -Wl,--defsym=__RAM_LENGTH__=0x10000 \
                -Wl,--undefined=memcpy_inflate \
                -Wl,--undefined=rwdata_inflate \
                -Wl,--undefined=bss_rwdata_init \
                -Wl,--undefined=chainloader \
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

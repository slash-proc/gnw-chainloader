# Makefile for building Game & Watch Chainloader Filesystem Modules (Drivers)

REPO_ROOT ?= ../../..

CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy

MCU = -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

BUILD_DIR = $(REPO_ROOT)/build/drivers
DRIVER_FATFS_OUT = $(REPO_ROOT)/build/fatfs.bin
DRIVER_LFS_OUT = $(REPO_ROOT)/build/lfs_rw.bin

# Compile flags
COMMON_CFLAGS = $(MCU) -DUSE_HAL_DRIVER -DSTM32H7B0xx -DNDEBUG \
	-I$(REPO_ROOT)/src/chainloader \
	-I$(REPO_ROOT)/deps/Core/Inc \
	-I$(REPO_ROOT)/deps/Drivers/STM32H7xx_HAL_Driver/Inc \
	-I$(REPO_ROOT)/deps/Drivers/CMSIS/Device/ST/STM32H7xx/Include \
	-I$(REPO_ROOT)/deps/Drivers/CMSIS/Include \
	-Os -Wall -fdata-sections -ffunction-sections -flto -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables

DRIVER_FATFS_CFLAGS = $(COMMON_CFLAGS) -I$(REPO_ROOT)/deps/FatFs
DRIVER_FATFS_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -Tfatfs/STM32H7B0_DRIVER.ld

DRIVER_LFS_CFLAGS = $(COMMON_CFLAGS) -I$(REPO_ROOT)/deps/littlefs -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_TRACE
DRIVER_LFS_LDFLAGS = $(MCU) -specs=nano.specs -lc -lm -lnosys -Wl,--gc-sections -flto -Tlfs_rw/STM32H7B0_DRIVER.ld

# Object files
FATFS_OBJS = \
	$(BUILD_DIR)/fatfs/driver_entry.o \
	$(BUILD_DIR)/fatfs/ff.o \
	$(BUILD_DIR)/fatfs/ffunicode.o

LFS_OBJS = \
	$(BUILD_DIR)/lfs_rw/driver_entry.o \
	$(BUILD_DIR)/lfs_rw/lfs.o \
	$(BUILD_DIR)/lfs_rw/lfs_util.o

all: $(DRIVER_FATFS_OUT) $(DRIVER_LFS_OUT)

$(DRIVER_FATFS_OUT): $(BUILD_DIR)/fatfs/fatfs.elf
	@echo "BIN $@"
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/fatfs/fatfs.elf: $(FATFS_OBJS)
	@echo "LD $@"
	@$(CC) $(FATFS_OBJS) $(DRIVER_FATFS_LDFLAGS) -o $@

$(BUILD_DIR)/fatfs/driver_entry.o: fatfs/driver_entry.c | $(BUILD_DIR)/fatfs
	@echo "CC $<"
	@$(CC) -c $(DRIVER_FATFS_CFLAGS) $< -o $@

$(BUILD_DIR)/fatfs/ff.o: $(REPO_ROOT)/deps/FatFs/ff.c | $(BUILD_DIR)/fatfs
	@echo "CC $<"
	@$(CC) -c $(DRIVER_FATFS_CFLAGS) $< -o $@

$(BUILD_DIR)/fatfs/ffunicode.o: $(REPO_ROOT)/deps/FatFs/ffunicode.c | $(BUILD_DIR)/fatfs
	@echo "CC $<"
	@$(CC) -c $(DRIVER_FATFS_CFLAGS) $< -o $@

$(DRIVER_LFS_OUT): $(BUILD_DIR)/lfs_rw/lfs_rw.elf
	@echo "BIN $@"
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/lfs_rw/lfs_rw.elf: $(LFS_OBJS)
	@echo "LD $@"
	@$(CC) $(LFS_OBJS) $(DRIVER_LFS_LDFLAGS) -o $@

$(BUILD_DIR)/lfs_rw/driver_entry.o: lfs_rw/driver_entry.c | $(BUILD_DIR)/lfs_rw
	@echo "CC $<"
	@$(CC) -c $(DRIVER_LFS_CFLAGS) $< -o $@

$(BUILD_DIR)/lfs_rw/lfs.o: $(REPO_ROOT)/deps/littlefs/lfs.c | $(BUILD_DIR)/lfs_rw
	@echo "CC $<"
	@$(CC) -c $(DRIVER_LFS_CFLAGS) $< -o $@

$(BUILD_DIR)/lfs_rw/lfs_util.o: $(REPO_ROOT)/deps/littlefs/lfs_util.c | $(BUILD_DIR)/lfs_rw
	@echo "CC $<"
	@$(CC) -c $(DRIVER_LFS_CFLAGS) $< -o $@

$(BUILD_DIR)/fatfs $(BUILD_DIR)/lfs_rw:
	mkdir -p $@

install: all
	@echo "Pushing fatfs.bin to LittleFS on device..."
	gnwmanager push --gnw-path=/drivers/fs/fatfs.bin --local-paths=$(DRIVER_FATFS_OUT)
	@echo "Pushing lfs_rw.bin to LittleFS on device..."
	gnwmanager push --gnw-path=/drivers/fs/lfs_rw.bin --local-paths=$(DRIVER_LFS_OUT)
	gnwmanager start bank1

clean:
	rm -fR $(BUILD_DIR)
	rm -f $(DRIVER_FATFS_OUT) $(DRIVER_LFS_OUT)

.PHONY: all clean install

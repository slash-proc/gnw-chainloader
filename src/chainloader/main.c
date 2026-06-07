#include "main.h"
#include "board.h"
#include "gui.h"
#include "menu.h"
#include "partition.h"
#include "input.h"
#include "../common/memory_map.h"
#include "../common/boot_magic.h"
#include "storage/vfs.h"
#include <string.h>

static void app_early_logic(void) {
    /* --- LEVEL 1: PHYSICAL BUTTON OVERRIDES (God Mode) --- */
    
    /* 1.1. LEFT or RIGHT: Boot Mario or Zelda OFW */
    bool hold_left = board_check_button(BTN_Left_GPIO_Port, BTN_Left_Pin);
    bool hold_right = board_check_button(BTN_Right_GPIO_Port, BTN_Right_Pin);

    if (hold_left || hold_right) {
        board_detect_console_type();
        const char *target_name = hold_left ? "Mario" : "Zelda";
        uint32_t spi_offset = hold_left ? 0x007C0000 : 0x007E0000;

        board_console_type_t target_console = hold_left ? CONSOLE_MARIO : CONSOLE_ZELDA;
        if (board_console_type != target_console) {
            /* Not the active OFW: flash it first. partition_flash_ofw draws a
               progress bar via gui_refresh(), which busy-waits on LTDC vertical
               blanking — so the display must be initialized first, otherwise it
               spins forever (black screen). gui_init() is idempotent. */
            board_init();
            gui_init();
            partition_flash_ofw(target_name, spi_offset, 128 * 1024);
        }
        board_jump_to_app(OFW_INTERNAL_BASE);
    }

    /* 1.2. B Button: Direct Retro-Go Boot Shortcut */
    if (board_check_button(BTN_B_GPIO_Port, BTN_B_Pin)) {
        if (board_is_valid_app(RETROGO_BASE)) {
            board_jump_to_app(RETROGO_BASE);
        }
    }

    /* 1.3. START or PAUSE (SET) Button: Force Launcher Menu (skip further checks) */
    if (board_check_button(BTN_START_GPIO_Port, BTN_START_Pin) ||
        board_check_button(BTN_PAUSE_GPIO_Port, BTN_PAUSE_Pin)) {
        return;
    }

    /* --- LEVEL 2: SOFTWARE INTENT (Magic Words) --- */

    /* 2.1. Check "Quit to menu" from Retro-Go.
       This takes priority over other software intents to allow escaping loops. */
    if (boot_magic_check((volatile uint32_t *)RG_MAGIC_ADDR, BOOT_MAGIC_RETROGO) ||
        boot_magic_check((volatile uint32_t *)RG_MAGIC_ADDR, BOOT_MAGIC_RESET)) {
        
        *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
        board_rtc_write_backup(0);
        return; // Proceed to launcher menu
    }

    uint32_t sram_magic = *(volatile uint32_t *)SRAM_MAGIC_ADDR;
    uint32_t target = *(volatile uint32_t *)SRAM_MAGIC_TARGET;

    /* 2.2. Standby Resume (Retro-Go standard) */
    if (boot_magic_check((volatile uint32_t *)SRAM_MAGIC_ADDR, BOOT_MAGIC_STANDBY)) {
        if (board_is_valid_app(RETROGO_BASE)) board_jump_to_app(RETROGO_BASE);
    }

    /* 2.3. Intentional Navigation (Protocol-Aware) */
    bool rtc_boot = (board_rtc_read_backup() == BOOT_MAGIC_BOOT);
    if (boot_magic_check((volatile uint32_t *)SRAM_MAGIC_ADDR, BOOT_MAGIC_BOOT) || rtc_boot) {
        if (board_is_valid_app(target)) {
            board_rtc_write_backup(0);
            *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
            board_jump_to_app(target);
        } 
        if (sram_magic == BOOT_MAGIC_BOOT) {
            if (board_is_valid_app(RETROGO_BASE)) {
                *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
                board_jump_to_app(RETROGO_BASE);
            }
        }
    }

    /* 2.4. Fast-Boot (BKP3R has BOOT_MAGIC_FASTBOOT) */
    if (board_rtc_read_fastboot() == BOOT_MAGIC_FASTBOOT) {
        if (board_is_valid_app(RETROGO_BASE)) {
            board_jump_to_app(RETROGO_BASE);
        }
    }
}

int main(void) {
    /* 1. Basic hardware setup for button/magic checks */
    board_early_init();

    /* 2. Run early logic: Buttons (God Mode), then SRAM Magic (Intent) */
    app_early_logic();

    /* Reaching here means we're committing to the launcher menu. Scrub any
       unconsumed boot intent — notably a stranded FRCE left by the OFW
       recovery hook (read_buttons unswaps and resets into the chainloader, so
       the OFW never consumes its own FRCE) — so it can't poison the next
       bank-swap into the OFW and bounce it straight back here. */
    *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;

    /* 3. Continue with full hardware initialization */
    board_init();

    /* 4. Launcher Mode: Run the themed menu */
    board_ospi_init();

    gui_init();
    input_init();
    vfs_init();
    partition_scan_start();
    menu_run();

    while(1);
}

void exit(int status) {
    (void)status;
    while(1);
}


#include "main.h"
#include "board.h"
#include "gui.h"
#include "menu.h"
#include "partition.h"
#include "input.h"
#include "../common/memory_map.h"
#include "../common/boot_magic.h"
#include "storage/vfs.h"
#include "system/bench.h"
#include "system/ofw_verify.h"
#include <string.h>

#ifdef BOOT_BENCH
volatile uint32_t g_boot_bench[BOOT_BENCH_N] __attribute__((used));
volatile uint32_t g_scan_bench[SCAN_BENCH_N] __attribute__((used));
#endif

static void boot_idle_pump(uint32_t ms);

static void app_early_logic(void) {
    /* --- LEVEL 1: PHYSICAL BUTTON OVERRIDES (God Mode) --- */
    
    /* 1.1. LEFT or RIGHT: Boot Mario or Zelda OFW */
    bool hold_left = board_check_button(BTN_Left_GPIO_Port, BTN_Left_Pin);
    bool hold_right = board_check_button(BTN_Right_GPIO_Port, BTN_Right_Pin);

    if (hold_left || hold_right) {
        /* We need OSPI initialized and the partition scanner started so that
           total_ext_flash_size is set and memory-mapped reads are valid. */
        board_ospi_init();
        partition_scan_start();

        board_detect_console_type();
        const char *target_name = hold_left ? "Mario" : "Zelda";
        uint32_t spi_offset = hold_left ? MARIO_SPI_OFFSET : ZELDA_SPI_OFFSET;

        board_console_type_t target_console = hold_left ? CONSOLE_MARIO : CONSOLE_ZELDA;
        bool ofw_ready = false;

        /* Refuses to boot or copy if the backup/asset CRC check fails. */
        if (ofw_verify_by_spi(spi_offset)) {
            ofw_ready = true;
            if (board_console_type != target_console) {
                /* Not the active OFW: flash it first. partition_flash_ofw draws a
                   progress bar via gui_refresh(), which busy-waits on LTDC vertical
                   blanking — so the display must be initialized first, otherwise it
                   spins forever (black screen). gui_init() is idempotent. */
                board_init();
                gui_settle_hook = boot_idle_pump;
                gui_init();
                /* Flashes the verified backup into internal flash Bank 2. */
                ofw_ready = partition_flash_ofw(target_name, spi_offset, 128 * 1024);
            }
        }

        if (ofw_ready) {
        /* God-mode jumps straight into the OFW, skipping the menu path's
           SRAM_MAGIC scrub (main, below) and input_init()'s remote-shadow clear.
           The bank-swap reset preserves DTCM, so reset-survivor junk in either
           cell rides into the patched OFW, whose bootloader() bounces back on a
           stale FORCE in SRAM_MAGIC and whose read_buttons() ORs the remote
           shadow into the gamepad (LEFT+GAME there reads as "return to
           launcher"). Either makes the OFW bounce straight back here. Scrub both
           so the OFW boots as clean as it does from the menu. */
        *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
        *(volatile uint32_t *)SRAM_REMOTE_INPUT_ADDR = 0;
        board_jump_to_app(OFW_INTERNAL_BASE);
        } /* if (ofw_ready) — else fall through to the menu */
    }

    /* 1.2. B Button: Direct Retro-Go Boot Shortcut */
    if (board_check_button(BTN_B_GPIO_Port, BTN_B_Pin)) {
        if (retrogo_bootable()) {
            board_jump_to_app(RETROGO_BASE);
        }
    }

    /* 1.3. START or PAUSE (SET) Button: Force Launcher Menu (skip further checks) */
    if (board_check_button(BTN_START_GPIO_Port, BTN_START_Pin) ||
        board_check_button(BTN_PAUSE_GPIO_Port, BTN_PAUSE_Pin)) {
        return;
    }

    /* --- LEVEL 2: SOFTWARE INTENT (Magic Words) --- */

    /* 2.1. Retro-Go "Return to Main Menu" (CORE) / warm reset (RESET): re-launch
       Retro-Go so its OWN launcher loads. Quitting to the chainloader is a DIFFERENT
       signal — BOOT at SRAM_MAGIC_ADDR + target, handled by §2.3. Escaping a stuck
       Retro-Go reset loop is the START/PAUSE boot override (§1.3), which runs before
       this. (boot_magic_check zeroes RG_MAGIC_ADDR on a match.) */
    if (boot_magic_check((volatile uint32_t *)RG_MAGIC_ADDR, BOOT_MAGIC_RETROGO) ||
        boot_magic_check((volatile uint32_t *)RG_MAGIC_ADDR, BOOT_MAGIC_RESET)) {

        *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
        board_rtc_write_backup(0);
        if (board_is_valid_app(RETROGO_BASE))
            board_jump_to_app(RETROGO_BASE);   // re-launch Retro-Go
        return; // Retro-Go absent -> fall through to the chainloader menu
    }

    uint32_t sram_magic = *(volatile uint32_t *)SRAM_MAGIC_ADDR;
    uint32_t target = *(volatile uint32_t *)SRAM_MAGIC_TARGET;

    /* 2.2. Standby Resume (Retro-Go standard) */
    if (boot_magic_check((volatile uint32_t *)SRAM_MAGIC_ADDR, BOOT_MAGIC_STANDBY)) {
        if (retrogo_bootable()) board_jump_to_app(RETROGO_BASE);
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
            if (retrogo_bootable()) {
                *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;
                board_jump_to_app(RETROGO_BASE);
            }
        }
    }

    /* 2.4. Fast-Boot (packed settings word; invalid/wiped signature => off). */
    if (settings_fastboot(board_rtc_read_settings())) {
        if (retrogo_bootable()) {
            board_jump_to_app(RETROGO_BASE);
        }
    }
}

uint32_t HAL_GetTick(void);

/* gui_init() settle-delay hook: while the LCD panel settles, advance the
 * partition/SD scan so the (independent-bus) SD + LittleFS probes overlap the
 * otherwise-idle wait instead of running sequentially after the LCD. Spins at
 * least `ms` (running longer is safe — only more panel settle time). */
static void boot_idle_pump(uint32_t ms) {
    uint32_t start = HAL_GetTick();
    do {
        partition_scan_update();
    } while ((HAL_GetTick() - start) < ms);
}

int main(void) {
    /* 1. Basic hardware setup for button/magic checks */
    board_early_init();

    /* DTCM is not zeroed on reset, so a stale fastcap hook pointer from a
       previous capture session would make gui_refresh() call a garbage address
       on the very first frame. Clear it BEFORE app_early_logic(): the god-mode
       OFW path (hold LEFT/RIGHT) brings up the display and draws a progress bar
       via gui_refresh() from INSIDE app_early_logic(), then jumps to the OFW —
       it never reaches the menu setup below, so clearing it there left that
       path calling a garbage hook (HardFault, frozen on "Preparing"). */
    *(volatile uint32_t *)FASTCAP_HOOK_ADDR = 0;

    /* 2. Run early logic: Buttons (God Mode), then SRAM Magic (Intent) */
    app_early_logic();

    /* Reaching here means we're committing to the launcher menu. Scrub any
       unconsumed boot intent — notably a stranded FRCE left by the OFW
       recovery hook (read_buttons unswaps and resets into the chainloader, so
       the OFW never consumes its own FRCE) — so it can't poison the next
       bank-swap into the OFW and bounce it straight back here. */
    *(volatile uint32_t *)SRAM_MAGIC_ADDR = 0;

    BENCH_MARK(0);   /* committed to launcher (see docs/startup-module-probe.md) */

    /* 3. Continue with full hardware initialization */
    board_init();
    BENCH_MARK(6);   /* board_init done (clocks, GPIO, ADC, OFW asset decompress) */

    /* 4. Launcher Mode: Run the themed menu */
    board_ospi_init();
    BENCH_MARK(7);   /* OSPI external-flash init done */

    /* Bring storage + the partition scan up BEFORE the LCD, then point gui_init's
     * settle delays at boot_idle_pump so the SD probe + LittleFS probe overlap the
     * panel bring-up (independent buses) instead of running after it. */
    input_init();
    vfs_init();
    partition_scan_start();
    gui_settle_hook = boot_idle_pump;
    gui_init();
    BENCH_MARK(8);   /* LCD/LTDC bring-up done (scan advanced during settle) */
    menu_run();

    while(1);
}

void exit(int status) {
    (void)status;
    while(1);
}


#include "menu.h"
#include "board.h"
#include "gui.h"
#include "assets.h"
#include <string.h>
#include "partition.h"
#include "flash.h"
#include "utils.h"
#include "input.h"
#include "ui.h"
#include "ui_list.h"
#include "../common/memory_map.h"
#include "../common/boot_magic.h"

/* --- Action Stubs --- */
extern ui_window_t PAGE_BROWSER;
static void action_browser_enter(void) { ui_push(&PAGE_BROWSER); }
static void action_retro_go(void) { board_request_jump(RETROGO_BASE); }
static void action_ofw(void)      { board_jump_to_app(OFW_INTERNAL_BASE); }
static void action_mario(void)    { partition_flash_ofw("Mario OFW", 0x007C0000, 128 * 1024); }
static void action_zelda(void)    { partition_flash_ofw("Zelda OFW", 0x007E0000, 128 * 1024); }
static void action_pwr(void)      { menu_enter_standby(); }

/* --- Forward Declarations --- */
static void menu_main_update(ui_window_t *self);
static void menu_main_draw(ui_window_t *self);
static void menu_tools_update(ui_window_t *self);
static void menu_tools_draw(ui_window_t *self);

static ui_window_t PAGE_MAIN = {
    .title = "GNW CHAINLOADER",
    .x = 50, .y = 55, .w = 220, .h = 130,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 1,
    .draw_content = menu_main_draw,
    .update_content = menu_main_update,
    .exit = NULL
};

static ui_window_t PAGE_TOOLS = {
    .title = "TOOLS",
    .x = 50, .y = 55, .w = 220, .h = 130,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 0,
    .draw_content = menu_tools_draw,
    .update_content = menu_tools_update,
    .exit = NULL
};

/* --- State --- */
static ui_list_t g_list_main;
static ui_list_t g_list_tools;

/* --- Main Menu Implementation --- */

static void action_tools_enter(void) { ui_push(&PAGE_TOOLS); }

static const char* MAIN_LABELS[] = {
    "BOOT RETRO-GO", "BOOT ACTIVE OFW", "SWITCH TO MARIO", "SWITCH TO ZELDA", "TOOLS", "POWER OFF"
};
static void (*MAIN_ACTIONS[])(void) = {
    action_retro_go, action_ofw, action_mario, action_zelda, action_tools_enter, action_pwr
};

static const char* get_main_label(int idx) { return MAIN_LABELS[idx]; }
static void on_main_action(int idx) { MAIN_ACTIONS[idx](); }

static void menu_main_enter(void) {
    ui_list_init(&g_list_main, "GNW CHAINLOADER", 6, get_main_label, on_main_action); 
    g_list_main.visible_lines = 6;
}

static void menu_main_update(ui_window_t *self) {
    ui_list_update(&g_list_main);
    partition_scan_update();
}

static void menu_main_draw(ui_window_t *self) {
    ui_list_draw(&g_list_main, self->x, self->y, self->w, self->h);
}

/* --- Diagnostics Sub-menu Implementation --- */

extern ui_window_t PAGE_PARTITION;

static void diag_action_partition(void) { ui_push(&PAGE_PARTITION); }
static void action_theme_toggle(void)   { ui_theme_toggle(); }
static void action_fastboot_toggle(void) {
    if (board_rtc_read_fastboot() == BOOT_MAGIC_FASTBOOT) {
        board_rtc_write_fastboot(0);
    } else {
        board_rtc_write_fastboot(BOOT_MAGIC_FASTBOOT);
    }
}

static const char* TOOLS_LABELS[] = {
    "FILE BROWSER", "PARTITION VIEWER", "TOGGLE THEME", "TOGGLE FAST-BOOT"
};
static void (*TOOLS_ACTIONS[])(void) = {
    action_browser_enter, diag_action_partition, action_theme_toggle, action_fastboot_toggle
};

static const char* get_tools_label(int idx) {
    if (idx == 3) {
        return (board_rtc_read_fastboot() == BOOT_MAGIC_FASTBOOT) ? "FAST-BOOT: ON" : "FAST-BOOT: OFF";
    }
    return TOOLS_LABELS[idx];
}
static void on_tools_action(int idx) { TOOLS_ACTIONS[idx](); }

static void menu_tools_enter(void) {
    ui_list_init(&g_list_tools, "TOOLS", 4, get_tools_label, on_tools_action);
    g_list_tools.visible_lines = 6;
    g_list_tools.on_back = ui_pop;
}

static void menu_tools_update(ui_window_t *self) {
    ui_list_update(&g_list_tools);
}

static void menu_tools_draw(ui_window_t *self) {
    ui_list_draw(&g_list_tools, self->x, self->y, self->w, self->h);
}

void menu_run(void) {
    ui_init();
    menu_main_enter();
    menu_tools_enter(); // Pre-initialize lists
    ui_push(&PAGE_MAIN);
    while (1) {
        ui_update();
        ui_draw();
    }
}

void menu_enter_standby(void) {
    gui_deinit();
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_LOW);
    HAL_PWREx_ClearWakeupFlag(PWR_FLAG_WKUP1);
    HAL_PWR_EnterSTANDBYMode();
    while (1) HAL_NVIC_SystemReset();
}

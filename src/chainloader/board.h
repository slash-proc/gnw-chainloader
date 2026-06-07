#ifndef BOARD_H
#define BOARD_H

#include "main.h"
#include <stdbool.h>

typedef enum {
    CONSOLE_NONE = 0,
    CONSOLE_MARIO,
    CONSOLE_ZELDA,
    CONSOLE_UNKNOWN
} board_console_type_t;

#define BOOT_DELAY_CYCLES 2000000
#define STABILIZATION_DELAY_CYCLES 100000

extern board_console_type_t board_console_type;

void board_detect_console_type(void);
void board_init(void);
void board_early_init(void);
void board_clocks_init(void);
void board_gpios_init(void);
void board_load_dynamic_assets(void);
void board_lcd_gpios_init(void);
bool board_ospi_init(void);
uint32_t board_ospi_get_size(void);
void board_adc_init(void);
void board_rtc_init(void);
uint32_t board_rtc_get_fattime(void);

bool board_check_button(GPIO_TypeDef *port, uint16_t pin);
uint32_t board_get_battery_raw(void);
uint32_t board_get_battery_millivolts(void);
void board_battery_update(int *out_percent, bool *out_plugged);
uint32_t board_rtc_read_backup(void);
void board_rtc_write_backup(uint32_t val);
uint32_t board_rtc_read_fastboot(void);
void board_rtc_write_fastboot(uint32_t val);
bool board_is_valid_app(uint32_t address);
void board_jump_to_app(uint32_t address);
void board_request_jump(uint32_t address);
bool board_flash_erase(void);
bool board_flash_write(uint32_t dst_addr, const uint8_t *src, uint32_t length);
void board_system_reset(void);
bool board_is_charging(void);
bool board_is_power_good(void);
#endif // BOARD_H

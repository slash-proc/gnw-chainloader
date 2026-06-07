#include "board.h"
#include <string.h>
#include "../common/banks.h"
#include "../common/memory_map.h"
#include "../common/boot_magic.h"

#ifndef STUB
OSPI_HandleTypeDef hospi1;
board_console_type_t board_console_type = CONSOLE_NONE;

/* Boot bring-up progress marker. Set to an increasing constant before each
 * major step and each lock/busy-wait in SystemClock_Config() (1-15) and the
 * OCTOSPI bring-up in MX_OCTOSPI1_Init() (16-19), so a hang during clock or
 * OSPI bring-up can be localized by reading this symbol over SWD.
 * Non-static + used so nm/the debugger can find it. */
volatile uint32_t g_boot_step __attribute__((used));
#endif


void Error_Handler(void) {
    while(1);
}

void wdog_refresh(void) {
    /* Watchdog logic hook placeholder */
}

void SysTick_Handler(void) {
    HAL_IncTick();
}

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
#ifndef STUB
static void MX_OCTOSPI1_Init(void);
bool OSPI_Init(OSPI_HandleTypeDef *hospi);
void OSPI_EnableMemoryMappedMode(void);
void OSPI_DisableMemoryMappedMode(void);
uint32_t OSPI_GetSize(void);
#endif

// Naked function to perform the jump to target pc/sp
static void __attribute__((naked)) start_app(void (*const pc)(void), uint32_t sp)
{
    __asm("           \n\
          msr msp, r1 /* load r1 into MSP */\n\
          bx r0       /* branch to the address at r0 */\n\
    ");
}

static bool board_early_initialized = false;
#ifndef STUB
static bool board_full_initialized = false;
static bool ospi_initialized = false;
#endif

void board_early_init(void) {
    if (board_early_initialized) return;

#ifndef STUB
    HAL_Init();
#endif

    __HAL_RCC_SRDSRAM_CLK_ENABLE(); // Enable SRD SRAM clock (D3 domain) for magic words

    SCB_EnableICache();
    SCB_EnableDCache();

    board_clocks_init();
    board_gpios_init();
    
    // Enable 1.8V & 3.3V power supply early
    // PD4 is active-high (3.3V), PD1 is active-low (1V8)
    GPIOD->BSRR = (GPIO_PIN_4 << 0) | (GPIO_PIN_1 << 16);

#ifdef STUB
    for (volatile int i = 0; i < STABILIZATION_DELAY_CYCLES; i++);
#else
    HAL_Delay(2);
#endif

#ifdef STUB
    for (volatile int i = 0; i < BOOT_DELAY_CYCLES; i++);
#else
    HAL_Delay(50); // Power-up delay
#endif

    board_rtc_init();
    
    board_early_initialized = true;
}

#ifndef STUB
void board_detect_console_type(void) {
    board_console_type = CONSOLE_NONE;
    if (board_is_valid_app(OFW_INTERNAL_BASE)) {
        /* Fast check: Stock reset vectors at offset 4 */
        uint32_t pc = *(volatile uint32_t *)(OFW_INTERNAL_BASE + 4);
        if (pc == 0x08018101UL) { board_console_type = CONSOLE_MARIO; return; }
        if (pc == 0x0801B3E1UL) { board_console_type = CONSOLE_ZELDA; return; }

        /* Slow fallback: Compare first 256 bytes with OSPI backups */
        if (board_ospi_init()) {
            const uint8_t *active = (const uint8_t *)OFW_INTERNAL_BASE;
            const uint8_t *mario = (const uint8_t *)(EXTFLASH_BASE + MARIO_SPI_OFFSET);
            const uint8_t *zelda = (const uint8_t *)(EXTFLASH_BASE + ZELDA_SPI_OFFSET);
            if (board_is_valid_app((uint32_t)mario) && memcmp(active, mario, 256) == 0) {
                board_console_type = CONSOLE_MARIO;
            } else if (board_is_valid_app((uint32_t)zelda) && memcmp(active, zelda, 256) == 0) {
                board_console_type = CONSOLE_ZELDA;
            } else {
                board_console_type = CONSOLE_UNKNOWN;
            }
        }
    }
}

void board_init(void) {
    board_early_init();
    if (board_full_initialized) return;

    board_detect_console_type();
    board_adc_init();
    board_load_dynamic_assets();
    
    board_full_initialized = true;
}
#endif

void board_clocks_init(void) {
    SystemClock_Config();
}

void board_gpios_init(void) {
    MX_GPIO_Init();
}

#ifndef STUB
bool board_ospi_init(void) {
    if (ospi_initialized) return true;
    
    MX_OCTOSPI1_Init();
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
    
    if (!OSPI_Init(&hospi1)) return false;
    OSPI_EnableMemoryMappedMode();
    ospi_initialized = true;
    return true;
}

uint32_t board_ospi_get_size(void) {
    if (!ospi_initialized) return 0;
    return OSPI_GetSize();
}

/*
 * Hand the OSPI flash pins (PB1/PB2/PE11/PD12) to the SD-card SoftSPI bit-bang
 * (the "Yota9" mod taps these same pins). Exit memory-mapped mode and deinit
 * the OCTOSPI peripheral so the SD code can drive the pins as plain GPIO.
 * Mirrors the reference's switch_ospi_gpio(false) peripheral handling.
 * MUST be paired with board_ospi_resume() on every exit path — the menu/theme
 * read the external flash and the boot path must never be left without it.
 */
void board_ospi_suspend(void) {
    if (!ospi_initialized) return;
    OSPI_DisableMemoryMappedMode();
    HAL_OSPI_DeInit(&hospi1);
}

/*
 * Restore the OCTOSPI flash after an SD SoftSPI burst. Re-init the peripheral
 * (HAL_OSPI_MspInit flips the shared pins back to OCTOSPI alternate function)
 * and re-enter memory-mapped mode. The flash chip itself was never reset, so we
 * skip the heavier OSPI_Init() device handshake — matching the reference's
 * lightweight HAL_OSPI_Init-only switch-back.
 */
void board_ospi_resume(void) {
    if (!ospi_initialized) return;
    MX_OCTOSPI1_Init();
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
    OSPI_EnableMemoryMappedMode();
}

void board_adc_init(void) {
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Battery voltage measurement pin (analog) */
    board_gpio_init(GPIOC, GPIO_PIN_4, GPIO_MODE_ANALOG, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0);

    __HAL_RCC_ADC12_FORCE_RESET();
    __HAL_RCC_ADC12_RELEASE_RESET();

    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1);

    ADC12_COMMON->CCR = (1 << ADC_CCR_CKMODE_Pos);

    ADC1->CR &= ~ADC_CR_ADCALDIF;
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL);

    ADC1->SQR1 = (4 << ADC_SQR1_SQ1_Pos) | (0 << ADC_SQR1_L_Pos);
    ADC1->SMPR1 = (7 << ADC_SMPR1_SMP4_Pos);

    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY));
}
#endif

void board_rtc_init(void) {
    PWR->CR1 |= PWR_CR1_DBP;
    __HAL_RCC_RTC_CLK_ENABLE();
    __HAL_RCC_RTC_ENABLE();
}

#ifndef STUB
uint32_t board_rtc_get_fattime(void) {
    uint32_t tr = RTC->TR;
    uint32_t dr = RTC->DR;
    
    int seconds = ((tr & 0x70) >> 4) * 10 + (tr & 0x0F);
    int minutes = ((tr & 0x7000) >> 12) * 10 + ((tr & 0x0F00) >> 8);
    int hours = ((tr & 0x300000) >> 20) * 10 + ((tr & 0x0F0000) >> 16);
    
    int date = ((dr & 0x30) >> 4) * 10 + (dr & 0x0F);
    int month = ((dr & 0x1000) >> 12) * 10 + ((dr & 0x0F00) >> 8);
    int year = ((dr & 0xF00000) >> 20) * 10 + ((dr & 0x0F0000) >> 16);
    
    return (uint32_t)((year + 2000 - 1980) << 25 | month << 21 | date << 16 |
                      hours << 11 | minutes << 5 | seconds >> 1);
}
#endif

bool board_check_button(GPIO_TypeDef *port, uint16_t pin) {
    return (port->IDR & pin) == 0;
}

#ifndef STUB
uint32_t board_get_battery_raw(void) {
    ADC1->ISR |= ADC_ISR_EOC;
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC));
    return ADC1->DR;
}

uint32_t board_get_battery_millivolts(void) {
    uint32_t raw = board_get_battery_raw();
    if (raw == 0) return 0;
    if (raw <= 11000u) return 3200;
    if (raw >= 13000u) return 4200;
    return 3200 + (raw - 11000u) * (4200 - 3200) / (13000u - 11000u);
}

void board_battery_update(int *out_percent, bool *out_plugged) {
    static uint32_t start_ms, next_ms, sum, count, raw;
    static int      shown = -1;
    uint32_t now = HAL_GetTick();
    bool plugged = board_is_power_good();

    if (!start_ms) start_ms = now;
    bool settling = (now - start_ms) < 1000u;

    if (settling) {
        uint32_t v = board_get_battery_raw();
        if (v) { sum += v; count++; raw = sum / count; }
        next_ms = now + 30000u;
    } else if ((int32_t)(now - next_ms) >= 0) {
        if ((raw = board_get_battery_raw())) next_ms = now + 30000u;
    }

    int pct = raw > 11000u ? (int)((raw - 11000u) * 100u / (13000u - 11000u)) : 0;
    if (pct > 100) pct = 100;

    if (settling || shown < 0)        shown = pct;
    else if (plugged)                 { if (pct > shown) shown = pct; }
    else                              { if (pct < shown) shown = pct; }

    *out_percent = shown;
    *out_plugged = plugged;
}
#endif

uint32_t board_rtc_read_backup(void) {
    return TAMP->BKP0R;
}

void board_rtc_write_backup(uint32_t val) {
    TAMP->BKP0R = val;
}

/* TAMP->BKP3R holds the packed settings word (see boot_magic.h): fast-boot bit
 * + per-OFW theme slots + validity signature. Battery-backed. */
uint32_t board_rtc_read_settings(void) {
    return TAMP->BKP3R;
}

void board_rtc_write_settings(uint32_t val) {
    TAMP->BKP3R = val;
}

bool board_is_valid_app(uint32_t address) {
    if (address & 0x03) return false;
    uint32_t hi = address >> 24;
    if (hi != 0x08 && hi != 0x20 && hi != 0x24 && hi != 0x30 && (hi < 0x90 || hi > 0x9F)) return false;

    if (hi == 0x08 && address >= 0x08200000) return false;
    if (hi == 0x20 && address >= 0x20020000) return false;
    if (hi == 0x24 && address >= 0x24100000) return false;
    if (hi == 0x30 && address >= 0x30040000) return false;

    uint32_t sp = *(volatile uint32_t *)address;
    uint32_t pc = *(volatile uint32_t *)(address + 4);

    if ((sp >> 24) != 0x20 && (sp >> 24) != 0x24) return false;
    uint32_t pc_region = (pc >> 24);
    if (pc_region != 0x08 && pc_region != 0x24) return false;

    return true;
}

void board_jump_to_app(uint32_t address) {
    if (!board_is_valid_app(address)) return;

    if ((address >> 24) == 0x08) {
        if (address >= OFW_INTERNAL_BASE) {
            for (volatile int i = 0; i < BOOT_DELAY_CYCLES; i++);
            if (ensure_swapped_banks()) while(1);
            address -= 0x00100000;
        } else {
            for (volatile int i = 0; i < BOOT_DELAY_CYCLES; i++);
            if (ensure_unswapped_banks()) while(1);
        }
    }

    SysTick->CTRL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSVCLR_Msk;
    
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    SCB_CleanDCache();
    SCB_DisableICache();
    SCB_DisableDCache();
    
    uint32_t sp = *(volatile uint32_t *)address;
    uint32_t pc = *(volatile uint32_t *)(address + 4);
    
    SCB->VTOR = address;
    __DSB();
    __ISB();
    
    __set_MSP(sp);
    __set_PSP(sp);
    start_app((void (*const)(void))pc, sp);
    while (1);
}

#ifndef STUB
bool board_flash_erase(void) {
    /* Refuse to erase while the banks are swapped: a swapped FLASH_BANK_2 maps to
     * the physical bank holding the chainloader + Retro-Go, so erasing it would
     * destroy the boot firmware. The chainloader only runs unswapped, so this is
     * defense-in-depth on top of the launcher-only reflash path. */
    if (FLASH_OPTSR_CUR & FLASH_OPTSR_SWAP_BANK) return false;

    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t SectorError = 0;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2);

    EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;
    EraseInitStruct.Banks = FLASH_BANK_2;
    EraseInitStruct.Sector = FLASH_SECTOR_0;
    EraseInitStruct.NbSectors = 16;
    
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }
    HAL_FLASH_Lock();
    return true;
}

bool board_flash_write(uint32_t dst_addr, const uint8_t *src, uint32_t length) {
    HAL_FLASH_Unlock();
    while (length) {
        uint32_t bytes_to_write = length > 16 ? 16 : length;
        uint8_t temp_buf[16];
        const uint8_t *write_ptr = src;
        
        if (bytes_to_write < 16) {
            memset(temp_buf, 0xFF, sizeof(temp_buf));
            memcpy(temp_buf, src, bytes_to_write);
            write_ptr = temp_buf;
        }
        
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, dst_addr, (uint32_t)write_ptr) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        
        dst_addr += 16;
        src += bytes_to_write;
        length -= bytes_to_write;
    }
    HAL_FLASH_Lock();
    return true;
}
#endif

void board_system_reset(void) {
    HAL_NVIC_SystemReset();
}

#ifndef STUB
void board_request_jump(uint32_t address) {
    *(volatile uint32_t *)SRAM_MAGIC_ADDR = BOOT_MAGIC_BOOT;
    *(volatile uint32_t *)SRAM_MAGIC_TARGET = address;
    SCB_CleanDCache();
    __DSB();
    HAL_NVIC_SystemReset();
}

bool board_is_charging(void) {
    return (GPIOE->IDR & GPIO_PIN_7) == 0;
}

bool board_is_power_good(void) {
    return (GPIOA->IDR & GPIO_PIN_2) == 0;
}

void board_lcd_gpios_init(void) {
    /* Re-configure LCD-related GPIOs to ensure precise speed and pull settings 
       before critical display initialization sequence. This overlaps with MX_GPIO_Init
       but provides a clean, display-centric initialization point. */
    board_gpio_init(GPIOD, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_8,
                    GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, 0);
    GPIOD->BSRR = GPIO_PIN_1 | ((uint32_t)GPIO_PIN_4 << 16) | ((uint32_t)GPIO_PIN_8 << 16);   // 1V8 power disable, 3V3 power disable, Reset low

    board_gpio_init(GPIOB, GPIO_PIN_12,
                    GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, 0);
    GPIOB->BSRR = GPIO_PIN_12;

    board_gpio_init(GPIOA, GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6,
                    GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, 0);
    GPIOA->BSRR = (GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6) << 16;
}
#endif

#ifndef STUB
/* Table-driven GPIO setup: each entry is one HAL_GPIO_Init() call. Moving the
   per-pin parameters into .rodata and iterating shrinks .text versus the
   repeated GPIO_InitStruct boilerplate, and keeps the LCD/OSPI/button pin maps
   readable in one place. */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       mode;
    uint8_t       pull;
    uint8_t       speed;
    uint8_t       alternate;
} gpio_cfg_t;

/* Bare-metal GPIO config — replaces HAL_GPIO_Init (saves ~440 B). Decodes the
   HAL GPIO_MODE_* / PULL / SPEED / AF constants straight to the registers; no
   EXTI/IT modes (none are used here). The caller enables the port clock, as
   with HAL. AFR is written before MODER (glitch-free AF switch, like HAL). */
void board_gpio_init(GPIO_TypeDef *g, uint32_t pins, uint32_t mode,
                     uint32_t pull, uint32_t speed, uint32_t af) {
    for (int p = 0; p < 16; p++) {
        if (!(pins & (1u << p))) continue;
        uint32_t s2 = (uint32_t)p * 2u;
        g->OSPEEDR = (g->OSPEEDR & ~(3u << s2)) | ((speed & 3u) << s2);
        g->PUPDR   = (g->PUPDR   & ~(3u << s2)) | ((pull  & 3u) << s2);
        if (mode & 0x10u) g->OTYPER |=  (1u << p);   /* open-drain */
        else              g->OTYPER &= ~(1u << p);   /* push-pull  */
        if ((mode & 3u) == 2u) {                     /* alternate function */
            uint32_t a4 = (uint32_t)(p & 7) * 4u;
            g->AFR[p >> 3] = (g->AFR[p >> 3] & ~(0xFu << a4)) | ((af & 0xFu) << a4);
        }
        g->MODER = (g->MODER & ~(3u << s2)) | ((mode & 3u) << s2);
    }
}

static void gpio_init_table(const gpio_cfg_t *cfg, int n) {
    for (int i = 0; i < n; i++)
        board_gpio_init(cfg[i].port, cfg[i].pin, cfg[i].mode,
                        cfg[i].pull, cfg[i].speed, cfg[i].alternate);
}

static void MX_GPIO_Init(void)
{
  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* Output levels latched into ODR before the pins are configured as outputs */
  GPIOE->BSRR = GPIO_PIN_3 | ((uint32_t)GPIO_PIN_8 << 16);   // Speaker enable, CE_n USB Charger reset
  GPIOB->BSRR = GPIO_PIN_12;  // LCD CS
  GPIOD->BSRR = (uint32_t)GPIO_PIN_8 << 16; // LCD Reset

  static const gpio_cfg_t cfg[] = {
    { GPIOE, GPIO_PIN_3|GPIO_PIN_8,                       GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOC, GPIO_PIN_5|GPIO_PIN_1|GPIO_PIN_13,           GPIO_MODE_INPUT,     GPIO_PULLUP, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOC, GPIO_PIN_11|GPIO_PIN_12,                     GPIO_MODE_INPUT,     GPIO_PULLUP, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOA, GPIO_PIN_0,                                  GPIO_MODE_INPUT,     GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOA, GPIO_PIN_2,                                  GPIO_MODE_INPUT,     GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOE, GPIO_PIN_7,                                  GPIO_MODE_INPUT,     GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOB, GPIO_PIN_12,                                 GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOD, GPIO_PIN_8|GPIO_PIN_1|GPIO_PIN_4,            GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOA, GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6,            GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0 },
    { GPIOD, BTN_A_Pin|BTN_B_Pin|BTN_Left_Pin|BTN_Down_Pin|BTN_Right_Pin|BTN_Up_Pin,
                                                          GPIO_MODE_INPUT,     GPIO_PULLUP, GPIO_SPEED_FREQ_LOW, 0 },
  };
  gpio_init_table(cfg, sizeof(cfg)/sizeof(cfg[0]));

  GPIOA->BSRR = (GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6) << 16;
}
#else
static void MX_GPIO_Init(void) {
  __HAL_RCC_GPIOD_CLK_ENABLE();

  // Set Pin 1 and Pin 4 to Output mode (01)
  // MODER bits: MODE1 is bits [3:2], MODE4 is bits [9:8]
  GPIOD->MODER = (GPIOD->MODER & ~(GPIO_MODER_MODE1 | GPIO_MODER_MODE4)) |
                 (GPIO_MODER_MODE1_0 | GPIO_MODER_MODE4_0);

  // OTYPER defaults to 0 (push-pull), OSPEEDR to 0 (low speed), PUPDR to 0 (no pull)
  GPIOD->OTYPER &= ~(GPIO_OTYPER_OT1 | GPIO_OTYPER_OT4);
  GPIOD->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED1 | GPIO_OSPEEDR_OSPEED4);
  GPIOD->PUPDR &= ~(GPIO_PUPDR_PUPD1 | GPIO_PUPDR_PUPD4);
}
#endif

#ifndef STUB
void HAL_OSPI_MspInit(OSPI_HandleTypeDef* hospi)
{
  if(hospi->Instance != OCTOSPI1) return;

  __HAL_RCC_OCTOSPIM_CLK_ENABLE();
  __HAL_RCC_OSPI1_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  static const gpio_cfg_t cfg[] = {
    { GPIOE, GPIO_PIN_2,  GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_OCTOSPIM_P1  },
    { GPIOA, GPIO_PIN_1,  GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_OCTOSPIM_P1  },
    { GPIOB, GPIO_PIN_1,  GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF11_OCTOSPIM_P1 },
    { GPIOB, GPIO_PIN_2,  GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_OCTOSPIM_P1  },
    { GPIOE, GPIO_PIN_11, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF11_OCTOSPIM_P1 },
    { GPIOD, GPIO_PIN_12, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_OCTOSPIM_P1  },
  };
  gpio_init_table(cfg, sizeof(cfg)/sizeof(cfg[0]));
}

void HAL_LTDC_MspInit(LTDC_HandleTypeDef* hltdc)
{
  if(hltdc->Instance != LTDC) return;

  __HAL_RCC_LTDC_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  static const gpio_cfg_t cfg[] = {
    { GPIOC, GPIO_PIN_0,                                   GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF11_LTDC },
    { GPIOA, GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_11, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF14_LTDC },
    { GPIOB, GPIO_PIN_0,                                   GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_LTDC  },
    { GPIOE, GPIO_PIN_13|GPIO_PIN_15,                      GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF14_LTDC },
    { GPIOB, GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_14|GPIO_PIN_8, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF14_LTDC },
    { GPIOD, GPIO_PIN_10|GPIO_PIN_3|GPIO_PIN_6,            GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF14_LTDC },
    { GPIOC, GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_10,            GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF14_LTDC },
    { GPIOC, GPIO_PIN_9,                                   GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF10_LTDC },
    { GPIOA, GPIO_PIN_10,                                  GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF12_LTDC },
    { GPIOD, GPIO_PIN_2,                                   GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF9_LTDC  },
    { GPIOB, GPIO_PIN_5,                                   GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF11_LTDC },
  };
  gpio_init_table(cfg, sizeof(cfg)/sizeof(cfg[0]));
}

/*
 * OCTOSPI1 + OCTOSPIM bring-up, hand-rolled to direct register writes to
 * byte-crunch the controller config (replacing HAL_OSPI_Init + HAL_OSPIM_Config).
 *
 * This reproduces the EXACT controller end-state of the previous HAL path for the
 * config struct that was used here:
 *   FifoThreshold=4, DualQuad=DISABLE, MemoryType=MACRONIX, DeviceSize=28,
 *   ChipSelectHighTime=2, FreeRunningClock=DISABLE, ClockMode=0,
 *   WrapSize=NOT_SUPPORTED, ClockPrescaler=1, SampleShifting=NONE,
 *   DelayHoldQuarterCycle=DISABLE, ChipSelectBoundary=0,
 *   DelayBlockBypass=BYPASSED, MaxTran=0, Refresh=0; and the OCTOSPIM IO-manager
 *   config ClkPort=1, NCSPort=1, IOLowPort=PORT_1_LOW (DQS/IOHigh ports unused).
 *
 * Only the CONTROLLER config (the inlined HAL_OSPI_Init register writes and the
 * HAL_OSPIM_Config port routing) is replaced. The chip-protocol handshake
 * (OSPI_Init: reset-enable/reset/read-ID/SFDP) and every HAL_OSPI_Command-based
 * flash access plus the memory-mapped enable stay exactly as before, in
 * board_ospi_init()/OSPI_EnableMemoryMappedMode(). The order of operations here
 * matches the HAL: MspInit (clocks+GPIO) -> DCR1/DCR2/DCR3/DCR4 -> CR.FTHRES ->
 * wait !BUSY -> DCR2.PRESCALER + CR.DQM + TCR -> CR.EN, then OCTOSPIM routing.
 *
 * Every register/bitfield below is verified against
 * deps/Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h7b0xx.h.
 *
 * g_boot_step continues the SystemClock_Config numbering (1-15) into 16-19 so an
 * OSPI-init hang localizes over SWD.
 */
static void MX_OCTOSPI1_Init(void)
{
  /* Keep the handle populated: OSPI_GetSize/HAL_OSPI_Command/HAL_OSPI_DeInit and
   * the chip handshake still read these fields. State must end at READY for
   * HAL_OSPI_Command (the flash-read path) to accept the handle. */
  hospi1.Instance = OCTOSPI1;
  hospi1.Init.FifoThreshold = 4;
  hospi1.Init.DualQuad = HAL_OSPI_DUALQUAD_DISABLE;
  hospi1.Init.MemoryType = HAL_OSPI_MEMTYPE_MACRONIX;
  hospi1.Init.DeviceSize = 28;
  hospi1.Init.ChipSelectHighTime = 2;
  hospi1.Init.ClockPrescaler = 1;
  hospi1.Init.DelayBlockBypass = HAL_OSPI_DELAY_BLOCK_BYPASSED;
  hospi1.ErrorCode = HAL_OSPI_ERROR_NONE;
  hospi1.Timeout = HAL_OSPI_TIMEOUT_DEFAULT_VALUE;

  /* Low-level hardware (OCTOSPIM/OSPI1 clocks + the shared OCTOSPIM_P1 GPIOs).
   * HAL_OSPI_Init always ran this here because the handle State is RESET on every
   * call (cold boot and after HAL_OSPI_DeInit in board_ospi_resume()). */
  g_boot_step = 16; /* OSPI MspInit (clocks + pins) */
  HAL_OSPI_MspInit(&hospi1);

  /* ---- Controller config: direct registers, end-state == the old HAL path ----
   * DCR1 = MTYP(Macronix) | DEVSIZE(28-1) | CSHT(2-1) | DLYBYP   (CKCSHT/FRCK/CKMODE = 0)
   *      = MTYP_0 | (27<<DEVSIZE_Pos) | (1<<CSHT_Pos) | DLYBYP */
  g_boot_step = 17; /* OSPI controller register block */
  MODIFY_REG(OCTOSPI1->DCR1,
             (OCTOSPI_DCR1_MTYP | OCTOSPI_DCR1_DEVSIZE | OCTOSPI_DCR1_CSHT | OCTOSPI_DCR1_CKCSHT |
              OCTOSPI_DCR1_DLYBYP | OCTOSPI_DCR1_FRCK | OCTOSPI_DCR1_CKMODE),
             (HAL_OSPI_MEMTYPE_MACRONIX | ((28U - 1U) << OCTOSPI_DCR1_DEVSIZE_Pos) |
              ((2U - 1U) << OCTOSPI_DCR1_CSHT_Pos) | HAL_OSPI_DELAY_BLOCK_BYPASSED));
  /* DCR2 WRAPSIZE = NOT_SUPPORTED (0); PRESCALER is configured below after !BUSY. */
  MODIFY_REG(OCTOSPI1->DCR2, OCTOSPI_DCR2_WRAPSIZE, HAL_OSPI_WRAP_NOT_SUPPORTED);
  /* DCR3 = CSBOUND(0) | MAXTRAN(0); DCR4 = Refresh(0). */
  OCTOSPI1->DCR3 = 0U;
  OCTOSPI1->DCR4 = 0U;
  /* CR FTHRES = (4-1) << FTHRES_Pos. */
  MODIFY_REG(OCTOSPI1->CR, OCTOSPI_CR_FTHRES, ((4U - 1U) << OCTOSPI_CR_FTHRES_Pos));

  /* Wait until the controller is idle before the prescaler/TCR programming, as
   * HAL_OSPI_Init did (OSPI_WaitFlagStateUntilTimeout on BUSY). */
  g_boot_step = 18; /* OSPI wait !BUSY */
  while ((OCTOSPI1->SR & OCTOSPI_SR_BUSY) != 0U) { }

  g_boot_step = 19; /* OSPI prescaler/TCR + enable + OCTOSPIM routing */
  /* DCR2 PRESCALER = (1-1) << PRESCALER_Pos = 0. */
  MODIFY_REG(OCTOSPI1->DCR2, OCTOSPI_DCR2_PRESCALER, ((1U - 1U) << OCTOSPI_DCR2_PRESCALER_Pos));
  /* CR DQM = DualQuad(DISABLE = 0). */
  MODIFY_REG(OCTOSPI1->CR, OCTOSPI_CR_DQM, HAL_OSPI_DUALQUAD_DISABLE);
  /* TCR SSHIFT(NONE=0) | DHQC(DISABLE=0). */
  MODIFY_REG(OCTOSPI1->TCR, (OCTOSPI_TCR_SSHIFT | OCTOSPI_TCR_DHQC),
             (HAL_OSPI_SAMPLE_SHIFTING_NONE | HAL_OSPI_DHQC_DISABLE));
  /* Enable OctoSPI. FreeRunningClock is DISABLE so DCR1.FRCK stays clear. */
  SET_BIT(OCTOSPI1->CR, OCTOSPI_CR_EN);
  hospi1.State = HAL_OSPI_STATE_READY;

  /* ---- OCTOSPIM IO-manager routing (replacing HAL_OSPIM_Config) ----
   * Config: instance 0 (OCTOSPI1), ClkPort=1, NCSPort=1, IOLowPort=PORT_1_LOW,
   * DQSPort=0, IOHighPort=0, MUXEN stays disabled. For this fixed config the HAL
   * MODIFY_REGs reduce to enabling CLK/NCS/IO-Low on port 1 (PCR[0]) sourced from
   * instance 0, and (because IOHighPort is left unused) enabling IO-High on port 2
   * (PCR[1]) per the HAL's else-branch. Source-select fields are 0 for instance 0.
   *
   * Req2AckTime was 0, so the HAL wrote OCTOSPIM->CR.REQ2ACK_TIME = (0-1) = 0xFF
   * (the (Req2AckTime-1) underflow path); reproduced for an exact match. It only
   * affects multiplexed mode (MUXEN), which stays disabled here. */
  MODIFY_REG(OCTOSPIM->CR, OCTOSPIM_CR_REQ2ACK_TIME,
             (0xFFU << OCTOSPIM_CR_REQ2ACK_TIME_Pos));
  MODIFY_REG(OCTOSPIM->PCR[0],
             (OCTOSPIM_PCR_CLKEN | OCTOSPIM_PCR_CLKSRC | OCTOSPIM_PCR_NCSEN | OCTOSPIM_PCR_NCSSRC |
              OCTOSPIM_PCR_IOLEN | OCTOSPIM_PCR_IOLSRC),
             (OCTOSPIM_PCR_CLKEN | OCTOSPIM_PCR_NCSEN | OCTOSPIM_PCR_IOLEN));
  MODIFY_REG(OCTOSPIM->PCR[1],
             (OCTOSPIM_PCR_IOHEN | OCTOSPIM_PCR_IOHSRC),
             (OCTOSPIM_PCR_IOHEN | OCTOSPIM_PCR_IOHSRC_0));
}
#endif

void SystemClock_Config(void)
{
#ifndef STUB
  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_HSI;

  /* ---- Supply / voltage scaling / backup-domain access (direct registers,
   * replacing HAL_PWREx_ConfigSupply + __HAL_PWR_VOLTAGESCALING_CONFIG). On a
   * cold boot the supply config is not yet locked, so ConfigSupply selects the
   * LDO and waits ACTVOSRDY; then VOS0 is selected and we wait VOSRDY. End-state
   * identical to the previous HAL path (verified against the H7B0 PWR map). ---- */
  g_boot_step = 1; /* supply */
  MODIFY_REG(PWR->CR3, (PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS), PWR_CR3_LDOEN);
  while (!(PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) {}
  MODIFY_REG(PWR->SRDCR, PWR_SRDCR_VOS, PWR_REGULATOR_VOLTAGE_SCALE0);
  while (!(PWR->SRDCR & PWR_SRDCR_VOSRDY)) {}

  PWR->CR1 |= PWR_CR1_DBP;
  MODIFY_REG(RCC->BDCR, RCC_BDCR_LSEDRV, RCC_LSEDRIVE_HIGH);

  /* PLL clock source = HSI (PLLCKSELR.PLLSRC = 0). */
  MODIFY_REG(RCC->PLLCKSELR, RCC_PLLCKSELR_PLLSRC, RCC_PLLSOURCE_HSI);

  /* ---- Oscillators + PLL1 (direct registers, replacing HAL_RCC_OscConfig).
   * Reproduces OscConfig's exact end-state and ordering for this struct:
   *   HSI = sysclk source here, so only its calibration trim is adjusted;
   *   LSI on; LSE (drive HIGH) on; PLL1 M16/N140/P2/Q2/R2, RGE 4-8MHz, wide VCO,
   *   FRACN=0, DIVP/Q/R enabled. SYSCLK = HSI(64)/16*140/2 = 280 MHz.
   * Ready flags decoded from the HAL flag bytes:
   *   PLL1RDY=RCC->CR bit, LSIRDY=RCC->CSR bit, LSERDY=RCC->BDCR bit. ---- */

  /* HSI calibration trim (RCC_VER_X not defined for H7B0 -> single MODIFY_REG;
   * RCC_HSICALIBRATION_DEFAULT = 0x40). HSI itself is already on (sysclk). */
  MODIFY_REG(RCC->HSICFGR, RCC_HSICFGR_HSITRIM,
             (uint32_t)RCC_HSICALIBRATION_DEFAULT << RCC_HSICFGR_HSITRIM_Pos);

  /* LSI on. */
  RCC->CSR |= RCC_CSR_LSION;
  g_boot_step = 2; /* wait LSI ready */
  while (!(RCC->CSR & RCC_CSR_LSIRDY)) {}

  /* LSE on (DBP already enabled above; LSEState = RCC_LSE_ON -> just set LSEON). */
  RCC->BDCR |= RCC_BDCR_LSEON;
  g_boot_step = 3; /* wait LSE ready */
  while (!(RCC->BDCR & RCC_BDCR_LSERDY)) {}

  /* PLL1: disable and wait, configure, then enable and wait for lock. */
  RCC->CR &= ~RCC_CR_PLL1ON;
  g_boot_step = 4; /* wait PLL1 off */
  while (RCC->CR & RCC_CR_PLL1RDY) {}
  /* PLLCKSELR: DIVM1 = 16 (PLLSRC already HSI). PLL1DIVR: N-1/P-1/Q-1/R-1. */
  MODIFY_REG(RCC->PLLCKSELR, (RCC_PLLCKSELR_PLLSRC | RCC_PLLCKSELR_DIVM1),
             (RCC_PLLSOURCE_HSI | (16U << 4U)));
  WRITE_REG(RCC->PLL1DIVR, (((140U - 1U) & RCC_PLL1DIVR_N1)
                            | (((2U - 1U) << 9U) & RCC_PLL1DIVR_P1)
                            | (((2U - 1U) << 16U) & RCC_PLL1DIVR_Q1)
                            | (((2U - 1U) << 24U) & RCC_PLL1DIVR_R1)));
  RCC->PLLCFGR &= ~RCC_PLLCFGR_PLL1FRACEN;                       /* FRACN disable */
  MODIFY_REG(RCC->PLL1FRACR, RCC_PLL1FRACR_FRACN1, 0U);          /* FRACN = 0 */
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL1RGE, RCC_PLL1VCIRANGE_2);   /* VCI 4-8MHz */
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL1VCOSEL, RCC_PLL1VCOWIDE);   /* VCO wide */
  RCC->PLLCFGR |= (RCC_PLLCFGR_DIVP1EN | RCC_PLLCFGR_DIVQ1EN | RCC_PLLCFGR_DIVR1EN);
  RCC->PLLCFGR |= RCC_PLLCFGR_PLL1FRACEN;                        /* FRACN enable */
  RCC->CR |= RCC_CR_PLL1ON;
  g_boot_step = 5; /* wait PLL1 lock */
  while (!(RCC->CR & RCC_CR_PLL1RDY)) {}

  /* Direct-register clock-tree bring-up, replacing HAL_RCC_ClockConfig().
   * Reproduces the exact end-state of:
   *   HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) with
   *   SYSCLKSource=PLLCLK, SYSCLKDivider=DIV1, AHBCLKDivider=DIV1,
   *   APB3/APB1/APB2/APB4 = DIV2.
   * The HAL clock constants below are already pre-shifted bitfield values
   * for the H7B0 CDCFGR1/CDCFGR2/SRDCFGR registers, so they are written
   * verbatim with MODIFY_REG (identical to the stores the HAL emits). This
   * drops the increasing/decreasing-divider ceremony and the
   * HAL_RCC_GetSysClockFreq()-based SystemCoreClock recompute, letting that
   * out-of-line helper garbage-collect. PLL1 is already locked here
   * (the direct PLL1 bring-up above enabled it and waited on RCC_CR_PLL1RDY).
   * SYSCLK = HSI(64MHz)/PLLM(16) * PLLN(140) / PLLP(2) = 280 MHz. */

  g_boot_step = 6; /* latency + prescalers + SYSCLK switch */
  /* Raise flash latency BEFORE switching up to 280 MHz (FLASH_LATENCY_7). */
  MODIFY_REG(FLASH->ACR, FLASH_ACR_LATENCY, FLASH_LATENCY_7);

  /* APB3 (CDPPRE) prescaler /2; SYSCLK divider (CDCPRE) /1 in CDCFGR1. */
  MODIFY_REG(RCC->CDCFGR1, RCC_CDCFGR1_CDPPRE, RCC_APB3_DIV2);
  /* APB1 (CDPPRE1) /2 and APB2 (CDPPRE2) /2 in CDCFGR2. */
  MODIFY_REG(RCC->CDCFGR2, RCC_CDCFGR2_CDPPRE1, RCC_APB1_DIV2);
  MODIFY_REG(RCC->CDCFGR2, RCC_CDCFGR2_CDPPRE2, RCC_APB2_DIV2);
  /* APB4 (SRDPPRE) /2 in SRDCFGR. */
  MODIFY_REG(RCC->SRDCFGR, RCC_SRDCFGR_SRDPPRE, RCC_APB4_DIV2);
  /* AHB (HPRE) /1 and SYSCLK core prescaler (CDCPRE) /1 in CDCFGR1. */
  MODIFY_REG(RCC->CDCFGR1, RCC_CDCFGR1_HPRE, RCC_HCLK_DIV1);
  MODIFY_REG(RCC->CDCFGR1, RCC_CDCFGR1_CDCPRE, RCC_SYSCLK_DIV1);

  /* Switch SYSCLK source to PLL1 and wait for the switch to take effect. */
  MODIFY_REG(RCC->CFGR, RCC_CFGR_SW, RCC_SYSCLKSOURCE_PLLCLK);
  while ((RCC->CFGR & RCC_CFGR_SWS) != (RCC_SYSCLKSOURCE_PLLCLK << RCC_CFGR_SWS_Pos)) {}

  /* Publish the resulting core clock directly (HPRE=DIV1, CDCPRE=DIV1 ->
   * SystemCoreClock == SYSCLK == 280 MHz). This is the value the GC'd
   * HAL_RCC_GetSysClockFreq() would have computed. */
  SystemCoreClock = 280000000U;
  HAL_InitTick(uwTickPrio);

  /* ---- Peripheral kernel clocks (direct registers, replacing
   * HAL_RCCEx_PeriphCLKConfig). The selected clocks and their HAL source order
   * are reproduced EXACTLY (SAI1 -> OSPI -> SPI123 -> RTC -> ADC[PLL2] ->
   * LTDC[PLL3] -> CKPER); only these are configured (TIM/TIMPRES was NOT in the
   * selection mask, so it was a no-op and is intentionally omitted). PLL2 (ADC)
   * and PLL3 (LTDC) replicate the static RCCEx_PLL2_Config/RCCEx_PLL3_Config
   * helpers with the same DIVP (PLL2) / DIVR (PLL3) outputs.
   *   PLL2: M25 N192 P5 Q2 R5, RGE 2-4MHz, wide VCO, FRACN=0, DIVP enabled.
   *   PLL3: M4  N9   P2 Q2 R24, RGE 8-16MHz, wide VCO, FRACN=0, DIVR enabled.
   * Muxes: SAI1<-PLL1Q(sel 0), OSPI<-CLKP, SPI123<-CLKP, RTC<-LSE, ADC<-PLL2P
   *   (sel 0), CKPER<-HSI (sel 0). PLL2RDY/PLL3RDY read RCC->CR. ---- */
  g_boot_step = 7; /* PLL2 + ADC mux */

  /* SAI1: source = PLL1 (sel 0). HAL enables PLL1 DIVQ output then clears the
   * SAI1SEL field. */
  RCC->PLLCFGR |= RCC_PLLCFGR_DIVQ1EN;
  MODIFY_REG(RCC->CDCCIP1R, RCC_CDCCIP1R_SAI1SEL, RCC_SAI1CLKSOURCE_PLL);

  /* OSPI kernel clock <- CLKP (per-core peripheral clock). */
  MODIFY_REG(RCC->CDCCIPR, RCC_CDCCIPR_OCTOSPISEL, RCC_OSPICLKSOURCE_CLKP);

  /* SPI1/2/3 kernel clock <- CLKP. */
  MODIFY_REG(RCC->CDCCIP1R, RCC_CDCCIP1R_SPI123SEL, RCC_SPI123CLKSOURCE_CLKP);

  /* RTC <- LSE. Backup-domain write access is already enabled (DBP). The RTC
   * clock source can only change after a backup-domain reset, so reproduce the
   * HAL dance: save BDCR (minus RTCSEL), pulse BDRST, restore, re-wait LSE,
   * then select LSE. Matches HAL_RCCEx_PeriphCLKConfig's RTC block exactly. */
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) != (RCC_RTCCLKSOURCE_LSE & RCC_BDCR_RTCSEL)) {
    uint32_t bdcr = RCC->BDCR & ~RCC_BDCR_RTCSEL;
    RCC->BDCR |= RCC_BDCR_BDRST;
    RCC->BDCR &= ~RCC_BDCR_BDRST;
    RCC->BDCR = bdcr;
  }
  g_boot_step = 8; /* wait LSE ready (post backup reset) */
  while (!(RCC->BDCR & RCC_BDCR_LSERDY)) {}
  /* __HAL_RCC_RTC_CONFIG(LSE): LSE is not a divided HSE so RTCPRE is cleared,
   * then RTCSEL = LSE (0x100). */
  RCC->CFGR &= ~RCC_CFGR_RTCPRE;
  RCC->BDCR |= (RCC_RTCCLKSOURCE_LSE & 0x00000FFFU);

  /* ADC <- PLL2P: configure PLL2 (DIVP output), then ADCSEL = PLL2 (sel 0). */
  RCC->CR &= ~RCC_CR_PLL2ON;
  g_boot_step = 9; /* wait PLL2 off */
  while (RCC->CR & RCC_CR_PLL2RDY) {}
  MODIFY_REG(RCC->PLLCKSELR, RCC_PLLCKSELR_DIVM2, (25U << 12U));
  WRITE_REG(RCC->PLL2DIVR, (((192U - 1U) & RCC_PLL2DIVR_N2)
                            | (((5U - 1U) << 9U) & RCC_PLL2DIVR_P2)
                            | (((2U - 1U) << 16U) & RCC_PLL2DIVR_Q2)
                            | (((5U - 1U) << 24U) & RCC_PLL2DIVR_R2)));
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL2RGE, RCC_PLL2VCIRANGE_1);   /* 2-4MHz */
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL2VCOSEL, RCC_PLL2VCOWIDE);   /* wide */
  RCC->PLLCFGR &= ~RCC_PLLCFGR_PLL2FRACEN;
  MODIFY_REG(RCC->PLL2FRACR, RCC_PLL2FRACR_FRACN2, 0U);
  RCC->PLLCFGR |= RCC_PLLCFGR_PLL2FRACEN;
  RCC->PLLCFGR |= RCC_PLLCFGR_DIVP2EN;                                 /* DIVP (P_UPDATE) */
  RCC->CR |= RCC_CR_PLL2ON;
  g_boot_step = 10; /* wait PLL2 lock */
  while (!(RCC->CR & RCC_CR_PLL2RDY)) {}
  MODIFY_REG(RCC->SRDCCIPR, RCC_SRDCCIPR_ADCSEL, RCC_ADCCLKSOURCE_PLL2);

  /* LTDC: configure PLL3 (DIVR output). HAL configures PLL3 here but does NOT
   * write an LTDC mux register (the LTDC kernel clock is PLL3R by hardware). */
  g_boot_step = 11; /* PLL3 (LTDC) */
  RCC->CR &= ~RCC_CR_PLL3ON;
  g_boot_step = 12; /* wait PLL3 off */
  while (RCC->CR & RCC_CR_PLL3RDY) {}
  MODIFY_REG(RCC->PLLCKSELR, RCC_PLLCKSELR_DIVM3, (4U << 20U));
  WRITE_REG(RCC->PLL3DIVR, (((9U - 1U) & RCC_PLL3DIVR_N3)
                            | (((2U - 1U) << 9U) & RCC_PLL3DIVR_P3)
                            | (((2U - 1U) << 16U) & RCC_PLL3DIVR_Q3)
                            | (((24U - 1U) << 24U) & RCC_PLL3DIVR_R3)));
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL3RGE, RCC_PLL3VCIRANGE_3);   /* 8-16MHz */
  MODIFY_REG(RCC->PLLCFGR, RCC_PLLCFGR_PLL3VCOSEL, RCC_PLL3VCOWIDE);   /* wide */
  RCC->PLLCFGR &= ~RCC_PLLCFGR_PLL3FRACEN;
  MODIFY_REG(RCC->PLL3FRACR, RCC_PLL3FRACR_FRACN3, 0U);
  RCC->PLLCFGR |= RCC_PLLCFGR_PLL3FRACEN;
  RCC->PLLCFGR |= RCC_PLLCFGR_DIVR3EN;                                 /* DIVR (R_UPDATE) */
  RCC->CR |= RCC_CR_PLL3ON;
  g_boot_step = 13; /* wait PLL3 lock */
  while (!(RCC->CR & RCC_CR_PLL3RDY)) {}

  /* CKPER <- HSI (sel 0). */
  MODIFY_REG(RCC->CDCCIPR, RCC_CDCCIPR_CKPERSEL, RCC_CLKPSOURCE_HSI);

  /* ---- CRS: HSI48 trimmed from LSE (direct registers, replacing
   * HAL_RCCEx_CRSConfig). Enable CRS clock, reset CRS, then program SYNC
   * (DIV1/LSE/rising) + reload + error-limit, set HSI48 trim, and start the
   * automatic trimming + frequency-error counter. Reload = 48MHz/32768 - 1. ---- */
  g_boot_step = 14; /* CRS */
  RCC->APB1HENR |= RCC_APB1HENR_CRSEN;
  (void)RCC->APB1HENR;                          /* RCC enable read-back delay */
  RCC->APB1HRSTR |= RCC_APB1HRSTR_CRSRST;       /* CRS force reset */
  RCC->APB1HRSTR &= ~RCC_APB1HRSTR_CRSRST;      /* CRS release reset */
  WRITE_REG(CRS->CFGR,
            (RCC_CRS_SYNC_DIV1 | RCC_CRS_SYNC_SOURCE_LSE | RCC_CRS_SYNC_POLARITY_RISING)
            | __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 32768)
            | (34U << CRS_CFGR_FELIM_Pos));
  MODIFY_REG(CRS->CR, CRS_CR_TRIM, (32U << CRS_CR_TRIM_Pos));
  CRS->CR |= (CRS_CR_AUTOTRIMEN | CRS_CR_CEN);

  g_boot_step = 15; /* clocks done */
#else
  /* Minimal stub clocks: HSI @ 64MHz */
  
  /* LDO Supply: PWR_LDO_SUPPLY */
  MODIFY_REG(PWR->CR3, (PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS), PWR_CR3_LDOEN);
  
  /* Voltage Scaling: VOS0 */
  MODIFY_REG(PWR->SRDCR, PWR_SRDCR_VOS, PWR_SRDCR_VOS_0 | PWR_SRDCR_VOS_1);
  while(!(PWR->CSR1 & PWR_CSR1_ACTVOSRDY));

  RCC->CR |= RCC_CR_HSION;
  while(!(RCC->CR & RCC_CR_HSIRDY));
  
  /* Select HSI as system clock */
  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_HSI;
  while((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI);

  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_2WS;
#endif
}

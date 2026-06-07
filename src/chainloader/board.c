#include "board.h"
#include <string.h>
#include "../common/banks.h"
#include "../common/memory_map.h"
#include "../common/boot_magic.h"

#ifndef STUB
OSPI_HandleTypeDef hospi1;
board_console_type_t board_console_type = CONSOLE_NONE;
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

static void MX_OCTOSPI1_Init(void)
{
  OSPIM_CfgTypeDef sOspiManagerCfg = {0};

  hospi1.Instance = OCTOSPI1;
  hospi1.Init.FifoThreshold = 4;
  hospi1.Init.DualQuad = HAL_OSPI_DUALQUAD_DISABLE;
  hospi1.Init.MemoryType = HAL_OSPI_MEMTYPE_MACRONIX;
  hospi1.Init.DeviceSize = 28;
  hospi1.Init.ChipSelectHighTime = 2;
  hospi1.Init.FreeRunningClock = HAL_OSPI_FREERUNCLK_DISABLE;
  hospi1.Init.ClockMode = HAL_OSPI_CLOCK_MODE_0;
  hospi1.Init.WrapSize = HAL_OSPI_WRAP_NOT_SUPPORTED;
  hospi1.Init.ClockPrescaler = 1;
  hospi1.Init.SampleShifting = HAL_OSPI_SAMPLE_SHIFTING_NONE;
  hospi1.Init.DelayHoldQuarterCycle = HAL_OSPI_DHQC_DISABLE;
  hospi1.Init.ChipSelectBoundary = 0;
  hospi1.Init.DelayBlockBypass = HAL_OSPI_DELAY_BLOCK_BYPASSED;
  hospi1.Init.MaxTran = 0;
  hospi1.Init.Refresh = 0;
  if (HAL_OSPI_Init(&hospi1) != HAL_OK) Error_Handler();

  sOspiManagerCfg.ClkPort = 1;
  sOspiManagerCfg.NCSPort = 1;
  sOspiManagerCfg.IOLowPort = HAL_OSPIM_IOPORT_1_LOW;
  if (HAL_OSPIM_Config(&hospi1, &sOspiManagerCfg, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) Error_Handler();
}
#endif

void SystemClock_Config(void)
{
#ifndef STUB
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  RCC_CRSInitTypeDef RCC_CRSInitStruct = {0};

  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_HSI;

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}
  
  PWR->CR1 |= PWR_CR1_DBP;
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_HIGH);

  __HAL_RCC_PLL_PLLSOURCE_CONFIG(RCC_PLLSOURCE_HSI);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 140;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2|RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK) Error_Handler();

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC|RCC_PERIPHCLK_LTDC|RCC_PERIPHCLK_SPI123|RCC_PERIPHCLK_SAI1|RCC_PERIPHCLK_ADC|RCC_PERIPHCLK_OSPI|RCC_PERIPHCLK_CKPER;
  PeriphClkInitStruct.PLL2.PLL2M = 25;
  PeriphClkInitStruct.PLL2.PLL2N = 192;
  PeriphClkInitStruct.PLL2.PLL2P = 5;
  PeriphClkInitStruct.PLL2.PLL2Q = 2;
  PeriphClkInitStruct.PLL2.PLL2R = 5;
  PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_1;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
  PeriphClkInitStruct.PLL3.PLL3M = 4;
  PeriphClkInitStruct.PLL3.PLL3N = 9;
  PeriphClkInitStruct.PLL3.PLL3P = 2;
  PeriphClkInitStruct.PLL3.PLL3Q = 2;
  PeriphClkInitStruct.PLL3.PLL3R = 24;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_3;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 0;
  PeriphClkInitStruct.OspiClockSelection = RCC_OSPICLKSOURCE_CLKP;
  PeriphClkInitStruct.CkperClockSelection = RCC_CLKPSOURCE_HSI;
  PeriphClkInitStruct.Spi123ClockSelection = RCC_SPI123CLKSOURCE_CLKP;
  PeriphClkInitStruct.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
  PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
  PeriphClkInitStruct.TIMPresSelection = RCC_TIMPRES_ACTIVATED;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) Error_Handler();

  __HAL_RCC_CRS_CLK_ENABLE();
  RCC_CRSInitStruct.Prescaler = RCC_CRS_SYNC_DIV1;
  RCC_CRSInitStruct.Source = RCC_CRS_SYNC_SOURCE_LSE;
  RCC_CRSInitStruct.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  RCC_CRSInitStruct.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,32768);
  RCC_CRSInitStruct.ErrorLimitValue = 34;
  RCC_CRSInitStruct.HSI48CalibrationValue = 32;
  HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);
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

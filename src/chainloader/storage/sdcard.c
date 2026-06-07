/*
 * In-core SD-card block device — faithful port of the proven implementation in
 * `example projects/game-and-watch-bootloader/`:
 *   - softspi.c                       -> the bit-bang SPI engine
 *   - porting/lib/FatFs/user_diskio_softspi.c -> the SD protocol over SoftSPI
 *   - gw_sdcard.c switch_ospi_gpio()  -> the OSPI<->GPIO pin handoff
 *   - gw_timer.c                      -> HAL_GetTick timeout helpers
 *
 * This file is the BLOCK layer (below FatFs): it deals only in 512-byte sectors
 * and depends on no filesystem. The in-core RO FatFs/LFS readers and the RW PIE
 * module sit on top of sdcard_read()/sdcard_write().
 *
 * Phase 2a: the SoftSPI / OSPI-tap ("Yota9") path — bit-banged over the SHARED
 * OCTOSPI flash pins (PB2 CLK / PB1 MOSI / PD12 MISO / PE11 CS). The dedicated-
 * pin SPI1 ("Tim") path is a follow-up (see sdcard_detect()).
 *
 * Deliberate hardening vs. the reference: the reference's init/read failure
 * paths sometimes return WITHOUT restoring OSPI memory-mapped mode, which would
 * strand the flash bus in GPIO mode and break the launcher menu. STABILITY IS
 * LAW here, so every exit routes through a single switch_ospi_gpio(true).
 */

#include "sdcard.h"
#include "board.h"
#include "stm32h7xx.h"
#include <stddef.h>

/* ---- SD transport selection (dist build) ----
 * Default: both transports (a universal binary). A size build for a specific SD
 * mod defines exactly one and drops the other's engine entirely:
 *   -DSDCARD_SPI1     dedicated-pin SPI1 ("Tim") only  — this device
 *   -DSDCARD_SOFTSPI  OSPI-tap bit-bang ("Yota9") only
 * Demoscene: the SPI1 binary carries no SoftSPI code, and vice-versa. */
#if !defined(SDCARD_SPI1) && !defined(SDCARD_SOFTSPI)
#define SDCARD_SPI1
#define SDCARD_SOFTSPI
#endif

/* ---- SoftSPI / OSPI-tap pin map (matches our HAL_OSPI_MspInit in board.c) ---- */
#define SD_SS_CLK_PORT   GPIOB
#define SD_SS_CLK_PIN    GPIO_PIN_2
#define SD_SS_MOSI_PORT  GPIOB
#define SD_SS_MOSI_PIN   GPIO_PIN_1
#define SD_SS_MISO_PORT  GPIOD
#define SD_SS_MISO_PIN   GPIO_PIN_12
#define SD_SS_CS_PORT    GPIOE
#define SD_SS_CS_PIN     GPIO_PIN_11

/* Card-type flags (from the reference's user_diskio_spi.h). */
#define CT_MMC    0x01
#define CT_SD1    0x02
#define CT_SD2    0x04
#define CT_BLOCK  0x08

#define BLOCK_SIZE 512u
#define START_BLOCK_TOKEN 0xFE

/* R1 / R3 response bit positions (from user_diskio_softspi.c). */
#define R1_IDLE             0
#define R1_ILLEGAL_COMMAND  2
#define R3_CCS              30
#define R3_READY            31

/* ============================ module state ============================== */

static sdcard_hw_t g_hw = SDCARD_HW_UNDETECTED;
static uint8_t  g_card_type;   /* CT_* flags; 0 == not initialized */

/* ============================ tiny timers =============================== */
/* Port of gw_timer.c — two HAL_GetTick-based deadline timers. */
static uint32_t s_tmr_start[2], s_tmr_delay[2];
static inline void timer_on(int i, uint32_t ms) { s_tmr_start[i] = HAL_GetTick(); s_tmr_delay[i] = ms; }
static inline int  timer_running(int i) { return (HAL_GetTick() - s_tmr_start[i]) < s_tmr_delay[i]; }

/* The project's watchdog hook (board.c) — a no-op placeholder today, but we call
 * it through the slow bit-bang loops exactly as the reference does, so it stays
 * correct if a real IWDG is ever armed. */
extern void wdog_refresh(void);

#ifdef SDCARD_SOFTSPI
/* ============================ SoftSPI engine ============================ */
/* SoftSPI-only card state (the SPI1 backend runs its own init/handshake, so
 * these would be declared-but-unused there). */
static bool     g_is_sd_v2;
static bool     g_ccs;         /* block (vs byte) addressing */
static uint32_t g_delay_us = 20;  /* bit delay: 20 = slow, 0 = fast */

/* Port of softspi.c: SPI mode 0 (CPOL=0/CPHA=0), MSB first, DelayUs-paced.
 * CS is active-low (csIsInverted). */

static void ss_delay_us(uint32_t usec) {
    uint32_t n = (SystemCoreClock / 1000000u) * (usec / 2u);
    while (n--) __asm volatile("nop");
}

static inline void ss_pause(void) {
    for (int i = 0; i < 16; i++) __asm volatile("nop");
}

/* Full-duplex transfer. tx==NULL sends 0xFF dummies. cs controls CS assertion:
 *   1 = assert (low) for the transfer then deassert,
 *   0 = leave CS deasserted (used for the power-up dummy clocks). */
static void ss_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len, int cs) {
    if (!len) return;

    HAL_GPIO_WritePin(SD_SS_CLK_PORT, SD_SS_CLK_PIN, GPIO_PIN_RESET);
    /* Yota9/OSPI-tap mod: CS is INVERTED (the reference's csIsInverted=true).
     * Driving the GPIO HIGH SELECTS the card; LOW DESELECTS it. Replicate the
     * reference exactly — treating it as standard active-low (drive low to
     * select) deselects the card during every byte and CMD0 never answers. */
    if (cs)
        HAL_GPIO_WritePin(SD_SS_CS_PORT, SD_SS_CS_PIN, GPIO_PIN_SET);   /* select */
    else
        HAL_GPIO_WritePin(SD_SS_CS_PORT, SD_SS_CS_PIN, GPIO_PIN_RESET); /* deselect */

    for (uint32_t i = 0; i < len; i++) {
        uint8_t txb = tx ? tx[i] : 0xFF;
        uint8_t rxb = 0;
        for (int j = 7; j >= 0; j--) {
            HAL_GPIO_WritePin(SD_SS_MOSI_PORT, SD_SS_MOSI_PIN,
                              (txb & (1 << j)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            ss_pause();
            HAL_GPIO_WritePin(SD_SS_CLK_PORT, SD_SS_CLK_PIN, GPIO_PIN_SET);
            ss_delay_us(g_delay_us);
            rxb <<= 1;
            if (HAL_GPIO_ReadPin(SD_SS_MISO_PORT, SD_SS_MISO_PIN) == GPIO_PIN_SET) rxb |= 1;
            HAL_GPIO_WritePin(SD_SS_CLK_PORT, SD_SS_CLK_PIN, GPIO_PIN_RESET);
            ss_delay_us(g_delay_us);
        }
        if (rx) rx[i] = rxb;
    }

    if (cs)
        HAL_GPIO_WritePin(SD_SS_CS_PORT, SD_SS_CS_PIN, GPIO_PIN_RESET); /* deselect (inverted) */
}

/* Convenience wrappers mirroring the reference's SoftSpi_* names. CS asserted. */
static inline void ss_write(const uint8_t *tx, uint32_t len)        { ss_xfer(tx, NULL, len, 1); }
static inline void ss_dummy_read(uint8_t *rx, uint32_t len)         { ss_xfer(NULL, rx, len, 1); }
static inline void ss_dummy_read_cslow(uint8_t *rx, uint32_t len)   { ss_xfer(NULL, rx, len, 0); }

/* ===================== OSPI <-> SD pin handoff ========================== */
/* Port of gw_sdcard.c switch_ospi_gpio(): hand the shared flash pins to GPIO
 * for bit-banging, and back to the OCTOSPI controller afterwards. The OSPI
 * peripheral lifecycle (deinit/reinit + memory-mapped) lives in board.c. */
static void switch_ospi_gpio(int to_ospi) {
    static int is_ospi = 1;
    if (is_ospi == to_ospi) return;

    if (to_ospi) {
        board_ospi_resume();   /* re-inits OCTOSPI; MspInit flips pins back to AF */
    } else {
        board_ospi_suspend();  /* exits mmap + HAL_OSPI_DeInit, frees the pins */

        GPIO_InitTypeDef g = {0};
        HAL_GPIO_WritePin(SD_SS_CS_PORT, SD_SS_CS_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(SD_SS_MOSI_PORT, SD_SS_MOSI_PIN | SD_SS_CLK_PIN, GPIO_PIN_RESET);

        g.Pin = SD_SS_CS_PIN;
        g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        HAL_GPIO_Init(SD_SS_CS_PORT, &g);

        g.Pin = SD_SS_MOSI_PIN | SD_SS_CLK_PIN;   /* both on GPIOB */
        HAL_GPIO_Init(SD_SS_MOSI_PORT, &g);

        g.Pin = SD_SS_MISO_PIN;
        g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(SD_SS_MISO_PORT, &g);
    }
    is_ospi = to_ospi;
}

/* ===================== SD command / response layer ====================== */
/* Faithful port of user_diskio_softspi.c. A response is accumulated into a
 * 64-bit value (r0); R1 is the low byte for short responses, byte[4] for R3/R7. */

/* Command indices into the table below. */
enum { C_GO_IDLE, C_SEND_OP_COND, C_IF_COND, C_STOP_TRAN,
       C_READ_SINGLE, C_READ_MULTI, C_WRITE_SINGLE, C_WRITE_MULTI,
       C_OP_COND_ACMD, C_APP_CMD, C_READ_OCR };

static const struct { uint8_t cmd; uint8_t crc; uint8_t kind; } sd_cmds[] = {
    [C_GO_IDLE]      = { 0,  0x95, 1 }, /* kind: 1=R1, 3=R3R7, 8=CMD8, 12=CMD12 */
    [C_SEND_OP_COND] = { 1,  0x00, 1 },
    [C_IF_COND]      = { 8,  0x86, 8 },
    [C_STOP_TRAN]    = { 12, 0x00, 12 },
    [C_READ_SINGLE]  = { 17, 0x00, 1 },
    [C_READ_MULTI]   = { 18, 0x00, 1 },
    [C_WRITE_SINGLE] = { 24, 0x00, 1 },
    [C_WRITE_MULTI]  = { 25, 0x00, 1 },
    [C_OP_COND_ACMD] = { 41, 0x00, 1 },
    [C_APP_CMD]      = { 55, 0x00, 1 },
    [C_READ_OCR]     = { 58, 0x00, 3 },
};

static bool resp_R1(uint8_t *r) {
    *r = 0xFF;
    for (int i = 0; i < 10 && *r == 0xFF; i++)
        ss_dummy_read(r, 1);
    return *r != 0xFF;
}

static bool resp_R3R7(uint8_t *r) {
    /* r[4] = R1, then 4 trailing bytes -> r[3..0] */
    if (!resp_R1(&r[4])) return false;
    ss_dummy_read(&r[3], 1);
    ss_dummy_read(&r[2], 1);
    ss_dummy_read(&r[1], 1);
    ss_dummy_read(&r[0], 1);
    return !r[4] || r[4] == (1 << R1_IDLE);
}

static bool resp_CMD8(uint8_t *r) {
    if (resp_R3R7(r)) return true;
    if (r[4] & (1 << R1_ILLEGAL_COMMAND)) return true; /* SD v1: illegal cmd is expected */
    return false;
}

static bool resp_CMD12(uint8_t *r) {
    *r = 0xFF;
    ss_dummy_read(NULL, 1); /* skip stuff byte */
    for (int i = 0; i < 10 && *r == 0xFF; i++)
        ss_dummy_read(r, 1);
    return *r != 0xFF;
}

static void send_cmd_payload(uint8_t cmd, uint32_t arg, uint8_t crc) {
    uint8_t p[6] = { (uint8_t)(cmd | 0x40),
                     (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                     (uint8_t)(arg >> 8),  (uint8_t)arg,
                     (uint8_t)(crc | 0x01) };
    ss_dummy_read(NULL, 2);
    ss_write(p, sizeof(p));
    wdog_refresh();
}

static bool send_cmd_once(int idx, uint32_t arg, uint8_t resp[8]) {
    send_cmd_payload(sd_cmds[idx].cmd, arg, sd_cmds[idx].crc);
    switch (sd_cmds[idx].kind) {
        case 8:  return resp_CMD8(resp);
        case 12: return resp_CMD12(resp);
        case 3:  return resp_R3R7(resp);
        default: return resp_R1(resp);
    }
}

/* Returns the accumulated 64-bit response value (r0). Retries up to 10x. */
static uint64_t send_cmd(int idx, uint32_t arg) {
    uint8_t resp[8];
    for (int i = 0; i < 10; i++) {
        for (int k = 0; k < 8; k++) resp[k] = 0;
        if (send_cmd_once(idx, arg, resp)) {
            uint64_t r0 = 0;
            for (int k = 0; k < 8; k++) r0 |= (uint64_t)resp[k] << (8 * k);
            return r0;
        }
    }
    return 0;
}

static void finish_read(void) { ss_dummy_read(NULL, 2); } /* discard CRC */

static bool finish_write(void) {
    uint8_t b;
    ss_dummy_read(NULL, 2);                 /* dummy CRC */
    /* Wait for the data-response token, BOUNDED so a card pulled/dead mid-write
     * returns an error instead of hanging here forever. */
    timer_on(1, 500);
    do { wdog_refresh(); ss_dummy_read(&b, 1); } while (b == 0xFF && timer_running(1));
    if ((b & 0x0F) != 0x05) return false;   /* not accepted (or timed out at 0xFF) */
    /* Wait out the busy (programming) phase, also bounded. */
    timer_on(1, 500);
    do { wdog_refresh(); ss_dummy_read(&b, 1); } while (b == 0x00 && timer_running(1));
    return (b != 0x00);                     /* false if still busy at timeout */
}

static void sd_ready_wait(void) {
    uint8_t b;
    timer_on(1, 500);
    do { wdog_refresh(); ss_dummy_read(&b, 1); } while (b != 0xFF && timer_running(1));
}

static uint64_t sd_power_on(void) {
    ss_dummy_read_cslow(NULL, 10);          /* 80 clocks, card deselected */
    return send_cmd(C_GO_IDLE, 0);          /* CMD0 -> expect r0 == 1 (idle) */
}

/* =================== OSPI-tap (SoftSPI) init / read / write ============= */

/* Initialize a card on the SoftSPI/OSPI-tap bus. Returns 0 on success.
 * Always restores OSPI before returning (single exit). */
static int softspi_init(void) {
    int ret = -1;
    g_card_type = 0;
    g_is_sd_v2 = false;
    g_ccs = false;
    g_delay_us = 20; /* slow clock for the entire init handshake */

    switch_ospi_gpio(0);

    uint64_t c0 = sd_power_on();
    if (c0 != (1 << R1_IDLE))
        goto out;

    /* CMD8: 3.3V + 0xAA check pattern -> distinguishes SD v2 from v1. */
    uint8_t r8[8] = {0};
    send_cmd_payload(sd_cmds[C_IF_COND].cmd, 0x1AA, sd_cmds[C_IF_COND].crc);
    resp_CMD8(r8);
    g_is_sd_v2 = !(r8[4] == (1 << R1_ILLEGAL_COMMAND));
    g_card_type = g_is_sd_v2 ? CT_SD2 : CT_SD1;

    send_cmd(C_READ_OCR, 0); /* per ChaN's flow */

    int i;
    for (i = 0; i < 255; i++) {
        if (g_is_sd_v2) {
            uint64_t a = send_cmd(C_APP_CMD, 0);
            if (a && a != (1 << R1_IDLE)) continue;
            if (!send_cmd(C_OP_COND_ACMD, 0x40000000)) break; /* HCS; ready when 0 */
        } else {
            if (!send_cmd(C_SEND_OP_COND, 0)) break;
        }
    }
    if (i == 255)
        goto out;

    if (g_is_sd_v2) {
        uint64_t ocr = send_cmd(C_READ_OCR, 0);
        if (!(ocr & ((uint64_t)1 << R3_READY)))
            goto out;
        g_ccs = ocr & ((uint64_t)1 << R3_CCS);
        if (g_ccs) g_card_type |= CT_BLOCK;
    }

    if (g_card_type) {
        g_delay_us = 0; /* fast */
        ret = 0;
    }

out:
    switch_ospi_gpio(1);
    return ret;
}

static int softspi_read(uint32_t sector, uint8_t *buf, uint32_t count) {
    int ret = -1;
    uint8_t tok;

    if (!count || !g_card_type) return -1;
    if (!(g_card_type & CT_BLOCK)) sector *= 512; /* byte addressing for SDSC */

    switch_ospi_gpio(0);

    do {
        if (send_cmd(C_READ_SINGLE, sector))
            goto out;                              /* non-zero R1 = error */
        timer_on(0, 200);
        do { ss_dummy_read(&tok, 1); } while (tok == 0xFF && timer_running(0));
        if (tok != START_BLOCK_TOKEN)
            goto out;
        ss_dummy_read(buf, BLOCK_SIZE);
        finish_read();
        buf += BLOCK_SIZE;
        sector += (g_card_type & CT_BLOCK) ? 1 : 512;
    } while (--count);

    ret = 0;
out:
    sd_ready_wait();
    switch_ospi_gpio(1);
    return ret;
}

static int softspi_write(uint32_t sector, const uint8_t *buf, uint32_t count) {
    int ret = -1;
    const uint8_t tok = START_BLOCK_TOKEN;

    if (!count || !g_card_type) return -1;
    if (!(g_card_type & CT_BLOCK)) sector *= 512;

    switch_ospi_gpio(0);

    do {
        while (send_cmd(C_WRITE_SINGLE, sector)) { } /* wait until accepted */
        ss_dummy_read(NULL, 1);                      /* dummy pre-token byte */
        ss_write(&tok, 1);                           /* data start token */
        ss_write(buf, BLOCK_SIZE);
        if (!finish_write())
            goto out;
        buf += BLOCK_SIZE;
        sector += (g_card_type & CT_BLOCK) ? 1 : 512;
    } while (--count);

    ret = 0;
out:
    sd_ready_wait();
    switch_ospi_gpio(1);
    return ret;
}

#endif /* SDCARD_SOFTSPI */

#ifdef SDCARD_SPI1
/* ===================== SPI1 / dedicated-pin path ("Tim") ================ */
/* Faithful port of gw_sdcard.c sdcard_init_spi1() + porting/lib/FatFs/
 * user_diskio_spi.c. Hardware SPI1 on dedicated pins — NO OSPI multiplexing,
 * no flash interaction (independent of the boot-critical path):
 *   SCK  PB3 (AF5)   MOSI PD7 (AF5)   MISO PB4 (AF5)
 *   CS   PB9 (GPIO, standard active-low — no inverter on this mod)
 *   VCC  PA15 (GPIO, active-high power gate) */
#define SD_SPI1_CS_PORT   GPIOB
#define SD_SPI1_CS_PIN    GPIO_PIN_9
#define SD_SPI1_VCC_PORT  GPIOA
#define SD_SPI1_VCC_PIN   GPIO_PIN_15
#define SPI1_TIMEOUT      100u

/* MMC/SDC commands (host form, with the 0x40 framing bit). */
#define CMD0  (0x40+0)   /* GO_IDLE_STATE */
#define CMD1  (0x40+1)   /* SEND_OP_COND (MMC) */
#define CMD8  (0x40+8)   /* SEND_IF_COND */
#define CMD12 (0x40+12)  /* STOP_TRANSMISSION */
#define CMD16 (0x40+16)  /* SET_BLOCKLEN */
#define CMD17 (0x40+17)  /* READ_SINGLE_BLOCK */
#define CMD18 (0x40+18)  /* READ_MULTIPLE_BLOCK */
#define CMD24 (0x40+24)  /* WRITE_BLOCK */
#define CMD25 (0x40+25)  /* WRITE_MULTIPLE_BLOCK */
#define CMD41 (0x40+41)  /* SEND_OP_COND (ACMD) */
#define CMD55 (0x40+55)  /* APP_CMD */
#define CMD58 (0x40+58)  /* READ_OCR */
#define CT_SDC (CT_SD1 | CT_SD2)

static SPI_HandleTypeDef hspi1;

/* HAL weak callback override — configures the SPI1 AF pins (board.c has none). */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi) {
    if (hspi->Instance != SPI1) return;
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    board_gpio_init(GPIOD, GPIO_PIN_7,              GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF5_SPI1);
    board_gpio_init(GPIOB, GPIO_PIN_3 | GPIO_PIN_4, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_VERY_HIGH, GPIO_AF5_SPI1);
}

/* SPI1 config via HAL_SPI_Init (the bare-metal CFG1/CFG2/CR1 version broke SD
 * detection on hardware — H7 SPI is finicky; reverted for stability). The byte
 * transfer is still bare-metal (spi1_xchg, below). */
static void spi1_set_speed(uint32_t prescaler) {
    HAL_SPI_DeInit(&hspi1);
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;       /* mode 0 */
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;                   /* CS is GPIO PB9 */
    hspi1.Init.BaudRatePrescaler = prescaler;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 0;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    hspi1.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
    hspi1.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    hspi1.Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
    hspi1.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
    hspi1.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    hspi1.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
    hspi1.Init.IOSwap = SPI_IO_SWAP_DISABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void sdcard_init_spi1(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(SD_SPI1_VCC_PORT, SD_SPI1_VCC_PIN, GPIO_PIN_RESET); /* VCC off */
    HAL_GPIO_WritePin(SD_SPI1_CS_PORT,  SD_SPI1_CS_PIN,  GPIO_PIN_SET);   /* CS high */
    board_gpio_init(SD_SPI1_VCC_PORT, SD_SPI1_VCC_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0);
    board_gpio_init(SD_SPI1_CS_PORT,  SD_SPI1_CS_PIN,  GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0);

    HAL_Delay(5);                                                         /* reset pulse */
    HAL_GPIO_WritePin(SD_SPI1_VCC_PORT, SD_SPI1_VCC_PIN, GPIO_PIN_SET);   /* VCC on */

    spi1_set_speed(SPI_BAUDRATEPRESCALER_256);
}

static inline void spi1_select(void)   { HAL_GPIO_WritePin(SD_SPI1_CS_PORT, SD_SPI1_CS_PIN, GPIO_PIN_RESET); }
static inline void spi1_deselect(void) { HAL_GPIO_WritePin(SD_SPI1_CS_PORT, SD_SPI1_CS_PIN, GPIO_PIN_SET); }

/* Bare-metal H7 SPI1 full-duplex byte exchange (replaces HAL_SPI_Transmit /
 * HAL_SPI_TransmitReceive). The peripheral is configured by spi1_set_speed()'s
 * HAL_SPI_Init; here we drive one byte per transfer (TSIZE=1): SPE then CSTART,
 * push TXDR, wait EOT, pop RXDR, clear flags, SPE off (resets for the next
 * byte). A guard counter prevents a hang if EOT never asserts (mis-config fails
 * gracefully — the SD just won't detect — rather than freezing the menu loop). */
static uint8_t spi1_xchg(uint8_t out) {
    SPI1->CR2 = 1;                          /* TSIZE = 1 byte */
    SPI1->CR1 |= SPI_CR1_SPE;
    SPI1->CR1 |= SPI_CR1_CSTART;
    *(volatile uint8_t *)&SPI1->TXDR = out;
    uint32_t guard = 100000u;
    while (!(SPI1->SR & SPI_SR_EOT) && --guard) { }
    uint8_t in = *(volatile uint8_t *)&SPI1->RXDR;
    SPI1->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
    SPI1->CR1 &= ~SPI_CR1_SPE;
    return in;
}
static void spi1_txbyte(uint8_t d)                  { (void)spi1_xchg(d); }
static void spi1_txbuf(const uint8_t *b, uint16_t n){ while (n--) (void)spi1_xchg(*b++); }
static uint8_t spi1_rxbyte(void)                    { return spi1_xchg(0xFF); }

static uint8_t spi1_ready_wait(void) {
    uint8_t r; timer_on(1, 750);
    do { wdog_refresh(); r = spi1_rxbyte(); } while (r != 0xFF && timer_running(1));
    return r;
}

/* Returns true if the card answered CMD0 (entered idle) within the window. A
 * non-present card never answers, so this is the fast "is a card there?" gate:
 * spi1_init() bails immediately on false instead of pressing on into the 750 ms
 * ready-wait + 2 s ACMD41 timeouts (which made a no-card detect take ~1 s). A
 * present card answers CMD0 in well under this 200 ms budget. */
static bool spi1_power_on(void) {
    uint8_t args[6];
    bool idle = false;
    spi1_deselect();
    for (int i = 0; i < 10; i++) spi1_txbyte(0xFF);   /* 80 clocks, CS high */
    HAL_Delay(2);
    spi1_select();
    spi1_txbyte(0xFF); spi1_txbyte(0xFF);
    args[0] = CMD0; args[1] = 0; args[2] = 0; args[3] = 0; args[4] = 0; args[5] = 0x95;
    spi1_txbuf(args, sizeof(args));
    timer_on(1, 200);
    do { wdog_refresh(); if (spi1_rxbyte() == 0x01) { idle = true; break; } } while (timer_running(1));
    spi1_deselect();
    spi1_txbyte(0xFF);
    return idle;
}

static bool spi1_rx_block(uint8_t *buf, unsigned len) {
    uint8_t token;
    if (!len) return true;
    timer_on(0, 200);
    do { token = spi1_rxbyte(); } while (token == 0xFF && timer_running(0));
    if (token != 0xFE) return false;
    for (unsigned i = 0; i < len; i++) buf[i] = spi1_rxbyte();
    spi1_rxbyte(); spi1_rxbyte();   /* discard CRC */
    return true;
}

static bool spi1_tx_block(const uint8_t *buf, uint8_t token) {
    uint8_t resp = 0, i = 0;
    if (spi1_ready_wait() != 0xFF) return false;
    spi1_txbyte(token);
    if (token != 0xFD) {
        spi1_txbuf(buf, 512);
        spi1_rxbyte(); spi1_rxbyte();           /* CRC */
        while (i <= 64) { resp = spi1_rxbyte(); if ((resp & 0x1F) == 0x05) break; i++; }
        for (i = 0; i < 64 && spi1_rxbyte() == 0; i++) ; /* wait while busy */
    } else {
        return spi1_ready_wait() == 0xFF;
    }
    return (resp & 0x1F) == 0x05;
}

static uint8_t spi1_send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t crc, res;
    if (spi1_ready_wait() != 0xFF) return 0xFF;
    spi1_txbyte(cmd);
    spi1_txbyte((uint8_t)(arg >> 24));
    spi1_txbyte((uint8_t)(arg >> 16));
    spi1_txbyte((uint8_t)(arg >> 8));
    spi1_txbyte((uint8_t)arg);
    crc = (cmd == CMD0) ? 0x95 : (cmd == CMD8) ? 0x87 : 1;
    spi1_txbyte(crc);
    if (cmd == CMD12) spi1_rxbyte();            /* skip stuff byte */
    uint8_t n = 32;
    do { res = spi1_rxbyte(); } while ((res & 0x80) && --n);
    return res;
}

static int spi1_init(void) {
    uint8_t n, type = 0, ocr[4];

    sdcard_init_spi1();
    spi1_set_speed(SPI_BAUDRATEPRESCALER_128);  /* slow for init */
    if (!spi1_power_on()) {        /* no CMD0 response (~200 ms) => no card; bail fast */
        g_card_type = 0;
        spi1_deselect();
        return -1;
    }
    spi1_select();

    if (spi1_send_cmd(CMD0, 0) == 1) {
        if (spi1_send_cmd(CMD8, 0x1AA) == 1) {     /* SDv2 */
            for (n = 0; n < 4; n++) ocr[n] = spi1_rxbyte();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                timer_on(0, 2000);
                do {
                    wdog_refresh();
                    if (spi1_send_cmd(CMD55, 0) <= 1 && spi1_send_cmd(CMD41, 1UL << 30) == 0) break;
                } while (timer_running(0));
                if (timer_running(0) && spi1_send_cmd(CMD58, 0) == 0) {
                    for (n = 0; n < 4; n++) ocr[n] = spi1_rxbyte();
                    type = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
            }
        } else {                                   /* SDv1 / MMC */
            timer_on(0, 2000);
            type = (spi1_send_cmd(CMD55, 0) <= 1 && spi1_send_cmd(CMD41, 0) <= 1) ? CT_SD1 : CT_MMC;
            do {
                wdog_refresh();
                if (type == CT_SD1) { if (spi1_send_cmd(CMD55, 0) <= 1 && spi1_send_cmd(CMD41, 0) == 0) break; }
                else                { if (spi1_send_cmd(CMD1, 0) == 0) break; }
            } while (timer_running(0));
            if (!timer_running(0) || spi1_send_cmd(CMD16, 512) != 0) type = 0;
        }
    }

    g_card_type = type;
    spi1_deselect();
    spi1_rxbyte();

    if (type) { spi1_set_speed(SPI_BAUDRATEPRESCALER_4); return 0; }  /* fast */
    return -1;
}

static int spi1_read(uint32_t sector, uint8_t *buf, uint32_t count) {
    if (!count || !g_card_type) return -1;
    if (!(g_card_type & CT_BLOCK)) sector *= 512;
    spi1_select();
    if (count == 1) {
        if (spi1_send_cmd(CMD17, sector) == 0 && spi1_rx_block(buf, 512)) count = 0;
    } else {
        if (spi1_send_cmd(CMD18, sector) == 0) {
            do { if (!spi1_rx_block(buf, 512)) break; buf += 512; } while (--count);
            spi1_send_cmd(CMD12, 0);
        }
    }
    spi1_deselect();
    spi1_rxbyte();
    return count ? -1 : 0;
}

static int spi1_write(uint32_t sector, const uint8_t *buf, uint32_t count) {
    if (!count || !g_card_type) return -1;
    if (!(g_card_type & CT_BLOCK)) sector *= 512;
    spi1_select();
    if (count == 1) {
        if (spi1_send_cmd(CMD24, sector) == 0 && spi1_tx_block(buf, 0xFE)) count = 0;
    } else {
        if (g_card_type & CT_SDC) { spi1_send_cmd(CMD55, 0); spi1_send_cmd(0x40 + 23, count); }
        if (spi1_send_cmd(CMD25, sector) == 0) {
            do { if (!spi1_tx_block(buf, 0xFC)) break; buf += 512; } while (--count);
            spi1_tx_block(0, 0xFD);
        }
    }
    spi1_deselect();
    spi1_rxbyte();
    return count ? -1 : 0;
}

#endif /* SDCARD_SPI1 */

/* ============================ public API =============================== */

bool sdcard_detect(void) {
    g_hw = SDCARD_HW_UNDETECTED;

    /* Try the dedicated-pin SPI1 ("Tim") bus first, then the SoftSPI/OSPI-tap
     * ("Yota9") bus — mirrors the reference's sdcard_hw_detect() order. Only the
     * transport(s) compiled in (see SDCARD_SPI1 / SDCARD_SOFTSPI) are tried. */
#ifdef SDCARD_SPI1
    if (spi1_init() == 0) {
        g_hw = SDCARD_HW_SPI1;
        return true;
    }
#endif
#ifdef SDCARD_SOFTSPI
    if (softspi_init() == 0) {
        g_hw = SDCARD_HW_OSPI1;
        return true;
    }
#endif

    g_hw = SDCARD_HW_NONE;
    return false;
}

sdcard_hw_t sdcard_hw(void)   { return g_hw; }
bool sdcard_present(void)     { return g_card_type != 0; }

int sdcard_read(uint32_t sector, uint8_t *buf, uint32_t count) {
    switch (g_hw) {
#ifdef SDCARD_SPI1
        case SDCARD_HW_SPI1:  return spi1_read(sector, buf, count);
#endif
#ifdef SDCARD_SOFTSPI
        case SDCARD_HW_OSPI1: return softspi_read(sector, buf, count);
#endif
        default: return -1;
    }
}

int sdcard_write(uint32_t sector, const uint8_t *buf, uint32_t count) {
    switch (g_hw) {
#ifdef SDCARD_SPI1
        case SDCARD_HW_SPI1:  return spi1_write(sector, buf, count);
#endif
#ifdef SDCARD_SOFTSPI
        case SDCARD_HW_OSPI1: return softspi_write(sector, buf, count);
#endif
        default: return -1;
    }
}

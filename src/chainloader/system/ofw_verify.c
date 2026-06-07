#include "ofw_verify.h"
#include "stm32h7xx.h"
#include "board.h"
#include "../../common/memory_map.h"
#include "../../common/ofw_crc.h"
#include "../../common/retrogo_crc.h"

/* Detected external-flash size in bytes (storage/partition.c). Used to bound every
 * CRC read so a stale record / smaller chip can never read past the mapped region. */
extern uint32_t total_ext_flash_size;

uint32_t ofw_crc32(const void *addr, uint32_t len) {
    RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
    (void)RCC->AHB1ENR;                 /* ensure the clock-enable write lands */

    /* Force the reset-default config explicitly (matches gen_ofw_crc.py): 32-bit
     * poly 0x04C11DB7, init 0xFFFFFFFF, no input/output reversal. Writing CR with
     * only RESET set zeroes POLYSIZE/REV_IN/REV_OUT and reloads DR from INIT. */
    CRC->INIT = 0xFFFFFFFFu;
    CRC->POL  = 0x04C11DB7u;
    CRC->CR   = CRC_CR_RESET;

    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    for (uint32_t n = len >> 2; n; n--) {
        CRC->DR = *p++;
    }
    return CRC->DR;
}

bool retrogo_crc_ok(void) {
#if !defined(RETROGO_CRC_LEN) || (RETROGO_CRC_LEN == 0)
    return true;                         /* header not baked -> fail open (never brick) */
#else
    uint32_t len = (uint32_t)RETROGO_CRC_LEN;
    /* Defensive bound: never CRC past the end of Bank 1 (len is baked-correct, but a
     * stale/implausible value must not over-read). Fail open if it doesn't fit. */
    if (len == 0 || len > ((256u * 1024u) - (RETROGO_BASE - CHAINLOADER_BASE))) return true;
    return ofw_crc32((const void *)RETROGO_BASE, len) == (uint32_t)RETROGO_CRC;
#endif
}

bool retrogo_bootable(void) {
    return board_is_valid_app(RETROGO_BASE) && retrogo_crc_ok();
}

static const ofw_crc_record_t *lookup_spi(uint32_t spi_offset) {
    for (int i = 0; i < OFW_CRC_RECORD_COUNT; i++) {
        if (OFW_CRC_RECORDS[i].spi_offset == spi_offset) return &OFW_CRC_RECORDS[i];
    }
    return 0;
}

/* True if [offset, offset+len) lies within the detected external flash. */
static bool ext_in_range(uint32_t offset, uint32_t len) {
    return total_ext_flash_size != 0 &&
           offset <= total_ext_flash_size &&
           len <= total_ext_flash_size - offset;
}

bool ofw_verify_by_spi(uint32_t spi_offset) {
    const ofw_crc_record_t *r = lookup_spi(spi_offset);
    if (!r) return false;

    uint32_t asset_off = r->asset_offset + r->asset_win_start;
    if (!ext_in_range(r->spi_offset, r->internal_len) ||
        !ext_in_range(asset_off, r->asset_win_len)) {
        return false;
    }

    if (ofw_crc32((const void *)(EXTFLASH_BASE + r->spi_offset), r->internal_len) != r->internal_crc) {
        return false;
    }
    if (ofw_crc32((const void *)(EXTFLASH_BASE + asset_off), r->asset_win_len) != r->asset_crc) {
        return false;
    }
    return true;
}

ofw_verify_status_t ofw_verify_addr(uint32_t addr) {
    if (addr < EXTFLASH_BASE) return OFW_VERIFY_NA;
    uint32_t off = addr - EXTFLASH_BASE;

    for (int i = 0; i < OFW_CRC_RECORD_COUNT; i++) {
        const ofw_crc_record_t *r = &OFW_CRC_RECORDS[i];

        if (off == r->asset_offset) {
            uint32_t a = r->asset_offset + r->asset_win_start;
            if (!ext_in_range(a, r->asset_win_len)) return OFW_VERIFY_NA;
            return ofw_crc32((const void *)(EXTFLASH_BASE + a), r->asset_win_len) == r->asset_crc
                       ? OFW_VERIFY_OK : OFW_VERIFY_BAD;
        }
        if (off == r->spi_offset) {
            if (!ext_in_range(r->spi_offset, r->internal_len)) return OFW_VERIFY_NA;
            return ofw_crc32((const void *)(EXTFLASH_BASE + r->spi_offset), r->internal_len) == r->internal_crc
                       ? OFW_VERIFY_OK : OFW_VERIFY_BAD;
        }
    }
    return OFW_VERIFY_NA;
}

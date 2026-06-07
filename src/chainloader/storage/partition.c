#include "board.h"
#include "partition.h"
#include "utils.h"
#include "flash.h"
#include "gui.h"
#include "ui.h"
#include "assets.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define EXT_FLASH_BASE 0x90000000
#define BANK1_BASE     0x08000000
#define BANK2_BASE     0x08100000
#define BANK_SIZE      (256 * 1024)

#define MAX_PARTITIONS 24
static partition_info_t found_partitions[MAX_PARTITIONS];
static int partition_count = 0;
uint32_t total_ext_flash_size = 0;

const char *partition_current_phase = "";   /* shown on the scan progress screen */

typedef enum {
    STATE_IDLE,
    STATE_SCAN_INT1,
    STATE_GAP_INT1,
    STATE_SCAN_INT2,
    STATE_GAP_INT2,
    STATE_SCAN_EXT,
    STATE_GAP_EXT,
    STATE_COMPLETE
} scan_state_internal_t;

static struct {
    scan_state_internal_t state;
    uint32_t stride;
    uint32_t addr;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t min_stride;
    int progress_done;
    int progress_total;
} g_scan;

partition_scan_state_t partition_scan_get_state(void) {
    switch (g_scan.state) {
        case STATE_IDLE:     return PARTITION_SCAN_IDLE;
        case STATE_COMPLETE: return PARTITION_SCAN_COMPLETE;
        default:             return PARTITION_SCAN_IN_PROGRESS;
    }
}

void partition_scan_get_progress(int *done, int *total) {
    *done = g_scan.progress_done;
    *total = g_scan.progress_total;
}

static int count_check_points(uint32_t start, uint32_t end, uint32_t min_stride) {
    int count = 0;
    for (uint32_t stride = 1024 * 1024; stride >= min_stride; stride >>= 1) {
        for (uint32_t addr = start; addr < end; addr += stride) {
            if (stride < (1024 * 1024) && (addr % (stride << 1) == 0))
                continue;
            count++;
        }
    }
    return count;
}

static void sort_partitions(void) {
    for (int i = 0; i < partition_count - 1; i++) {
        for (int j = 0; j < partition_count - i - 1; j++) {
            if (found_partitions[j].address > found_partitions[j + 1].address) {
                partition_info_t temp = found_partitions[j];
                found_partitions[j] = found_partitions[j + 1];
                found_partitions[j + 1] = temp;
            }
        }
    }
}

static void add_partition(uint32_t addr, uint32_t size, const char* type, const char* details) {
    if (partition_count < MAX_PARTITIONS) {
        found_partitions[partition_count].address = addr;
        found_partitions[partition_count].size = size;
        found_partitions[partition_count].type = type;
        strncpy(found_partitions[partition_count].details, details, sizeof(found_partitions[partition_count].details) - 1);
        found_partitions[partition_count].details[sizeof(found_partitions[partition_count].details) - 1] = '\0';
        partition_count++;
    }
}

static bool is_covered(uint32_t addr) {
    for (int i = 0; i < partition_count; i++) {
        if (addr >= found_partitions[i].address &&
            addr <  found_partitions[i].address + found_partitions[i].size) {
            return true;
        }
    }
    return false;
}

static void find_free_space(uint32_t start, uint32_t end) {
    uint32_t current_addr = start;
    int initial_count = partition_count;
    for (int i = 0; i < initial_count; i++) {
        if (found_partitions[i].address < start || found_partitions[i].address >= end)
            continue;
        if (found_partitions[i].address > current_addr) {
            add_partition(current_addr, found_partitions[i].address - current_addr, "FREE", "Empty");
        }
        uint32_t next_addr = found_partitions[i].address + found_partitions[i].size;
        if (next_addr > current_addr) current_addr = next_addr;
    }
    if (current_addr < end) {
        add_partition(current_addr, end - current_addr, "FREE", "Empty");
    }
}

static const char* identify_firmware(uint32_t addr) {
    if (addr == 0x08008000) return "Retro-Go";
    
    /* Fast check: Stock reset vectors at offset 4 */
    uint32_t pc = *(volatile uint32_t *)(addr + 4);
    if (pc == 0x08018101UL) return "OFW Mario";
    if (pc == 0x0801B3E1UL) return "OFW Zelda";
    
    return "Firmware";
}

typedef struct {
    const char *type;
    const char *details;
    uint32_t size;        /* 0 for dynamic scan */
    uint32_t sig_offset;
    const uint8_t *sig;
    uint8_t sig_len;
} sig_probe_t;

static const sig_probe_t STATIC_PROBES[] = {
    {"Zelda",    "Assets",     4*1024*1024, 0,       (const uint8_t*)"\xBE\xBA\xAD\xFE", 4},
    {"Zelda",    "Assets",     4*1024*1024, 0x4FFEB, (const uint8_t*)"ZELDA", 5},
    {"Mario",    "Assets",     1*1024*1024, 0,       (const uint8_t*)"\x78\xD8\xA9\x10", 4},
    {"Mario",    "OFW Backup", 128*1024,    4,       (const uint8_t*)"\x01\x81\x01\x08", 4},
    {"Zelda",    "OFW Backup", 128*1024,    4,       (const uint8_t*)"\xE1\xB3\x01\x08", 4},
    {"Retro-Go", "APP BIN",    0,           0x400,   (const uint8_t*)"\x01\x00\x00\x00", 4},
};

static void check_address(uint32_t addr) {
    if (partition_count >= MAX_PARTITIONS) return;
    if (is_covered(addr)) return;

    const uint8_t* sector = (const uint8_t*)addr;
    uint32_t bank_end = 0;
    bool is_internal = false;

    if (addr >= BANK1_BASE && addr < BANK1_BASE + BANK_SIZE) {
        bank_end = BANK1_BASE + BANK_SIZE;
        is_internal = true;
    } else if (addr >= BANK2_BASE && addr < BANK2_BASE + BANK_SIZE) {
        bank_end = BANK2_BASE + BANK_SIZE;
        is_internal = true;
    } else if (addr >= EXT_FLASH_BASE && addr < EXT_FLASH_BASE + total_ext_flash_size) {
        bank_end = EXT_FLASH_BASE + total_ext_flash_size;
    } else {
        return;
    }

    for (size_t i = 0; i < sizeof(STATIC_PROBES)/sizeof(STATIC_PROBES[0]); i++) {
        const sig_probe_t *p = &STATIC_PROBES[i];
        if (addr + p->sig_offset + p->sig_len <= bank_end) {
            if (memcmp((const uint8_t*)(addr + p->sig_offset), p->sig, p->sig_len) == 0) {
                uint32_t size = p->size;
                const char *type = p->type;
                char details[32];
                strcpy(details, p->details);

                if (size == 0) {
                    if (strcmp(type, "Retro-Go") == 0) {
                        size = 16 * 1024;
                        while (addr + size + 16384 <= bank_end) {
                            const uint32_t *chunk = (const uint32_t *)(addr + size);
                            bool is_empty = true;
                            for (int j = 0; j < 1024; j++) {
                                if (chunk[j] != 0xFFFFFFFF) { is_empty = false; break; }
                            }
                            if (is_empty) break;
                            size += 16384;
                        }
                    }
                }
                if (addr + size <= bank_end) {
                    if (is_internal && strcmp(details, "OFW Backup") == 0) {
                        strcpy(details, "APP BIN");
                    }
                    add_partition(addr, size, type, details);
                    return;
                }
            }
        }
    }

    if (is_internal && board_is_valid_app(addr)) {
        if (addr == BANK1_BASE) {
            add_partition(addr, 0x8000, "Chainloader", "GNW-Chainloader");
        } else {
            add_partition(addr, 128*1024, identify_firmware(addr), "APP BIN");
        }
        return;
    }

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        if (sector[0] == 0xEB || sector[0] == 0xE9) {
            add_partition(addr, 1024 * 1024, "FAT", "Filesystem");
            return;
        }
    }

    if (memcmp(sector, "FROG", 4) == 0) {
        uint16_t num_entries = sector[6] | (sector[7] << 8);
        uint32_t bin_sz = sector[8] | (sector[9] << 8) | (sector[10] << 16) | (sector[11] << 24);
        char detail_buf[32];
        strcpy(detail_buf, "Entries: ");
        int_to_str(num_entries, detail_buf + strlen(detail_buf));
        add_partition(addr, bin_sz, "FrogFS", detail_buf);
        return;
    }

    for (int i = 0; i < 16; i++) {
        uint32_t block_addr = addr + (i * 4096);
        if (block_addr + 32 > bank_end) break;
        const uint8_t* block = (const uint8_t*)block_addr;
        if (memcmp(&block[8], "littlefs", 8) == 0) {
            uint32_t version = block[20] | (block[21] << 8) | (block[22] << 16) | (block[23] << 24);
            uint32_t block_size = block[24] | (block[25] << 8) | (block[26] << 16) | (block[27] << 24);
            uint32_t block_count = block[28] | (block[29] << 8) | (block[30] << 16) | (block[31] << 24);
            if ((version >> 16) == 2 && block_size >= 128 && block_size <= 8192 && block_count > 0) {
                uint32_t size = block_size * block_count;
                uint32_t phys_start = block_addr;
                if (block_addr + size > bank_end) {
                    if (block_addr >= bank_end - (2 * block_size)) phys_start = bank_end - size;
                }
                if (phys_start >= (is_internal ? 0x08000000 : EXT_FLASH_BASE) && phys_start + size <= bank_end) {
                    char detail_buf[32];
                    strcpy(detail_buf, "Blocks: ");
                    int_to_str(block_count, detail_buf + strlen(detail_buf));
                    add_partition(phys_start, size, "LittleFS", detail_buf);
                    return;
                }
            }
        }
    }
}

void partition_scan_start(void) {
    partition_count = 0;
    total_ext_flash_size = board_ospi_get_size();
    
    g_scan.progress_done = 0;
    g_scan.progress_total = count_check_points(BANK1_BASE, BANK1_BASE + BANK_SIZE, 16384)
                          + count_check_points(BANK2_BASE, BANK2_BASE + BANK_SIZE, 16384)
                          + count_check_points(EXT_FLASH_BASE, EXT_FLASH_BASE + total_ext_flash_size, 65536);
    
    g_scan.state = STATE_SCAN_INT1;
    g_scan.stride = 1024 * 1024;
    g_scan.addr = BANK1_BASE;
    g_scan.range_start = BANK1_BASE;
    g_scan.range_end = BANK1_BASE + BANK_SIZE;
    g_scan.min_stride = 16384;
    partition_current_phase = "Internal Flash";
}

void partition_scan_update(void) {
    if (g_scan.state == STATE_IDLE || g_scan.state == STATE_COMPLETE) return;

    // Process a few points per update to avoid blocking too long
    for (int step = 0; step < 4; step++) {
        switch (g_scan.state) {
            case STATE_SCAN_INT1:
            case STATE_SCAN_INT2:
            case STATE_SCAN_EXT: {
                if (g_scan.stride < g_scan.min_stride) {
                    g_scan.state++; // Move to GAP state
                    return;
                }
                
                if (g_scan.addr >= g_scan.range_end) {
                    g_scan.stride >>= 1;
                    g_scan.addr = g_scan.range_start;
                    return;
                }

                if (g_scan.stride == 1024 * 1024 || (g_scan.addr % (g_scan.stride << 1) != 0)) {
                    check_address(g_scan.addr);
                    g_scan.progress_done++;
                }
                g_scan.addr += g_scan.stride;
                break;
            }

            case STATE_GAP_INT1:
                sort_partitions();
                find_free_space(BANK1_BASE, BANK1_BASE + BANK_SIZE);
                g_scan.state = STATE_SCAN_INT2;
                g_scan.stride = 1024 * 1024;
                g_scan.addr = BANK2_BASE;
                g_scan.range_start = BANK2_BASE;
                g_scan.range_end = BANK2_BASE + BANK_SIZE;
                g_scan.min_stride = 16384;
                return;

            case STATE_GAP_INT2:
                sort_partitions();
                find_free_space(BANK2_BASE, BANK2_BASE + BANK_SIZE);
                if (total_ext_flash_size > 0) {
                    g_scan.state = STATE_SCAN_EXT;
                    g_scan.stride = 1024 * 1024;
                    g_scan.addr = EXT_FLASH_BASE;
                    g_scan.range_start = EXT_FLASH_BASE;
                    g_scan.range_end = EXT_FLASH_BASE + total_ext_flash_size;
                    g_scan.min_stride = 65536;
                    partition_current_phase = "External Flash";
                } else {
                    g_scan.state = STATE_COMPLETE;
                    sort_partitions();
                }
                return;

            case STATE_GAP_EXT:
                sort_partitions();
                find_free_space(EXT_FLASH_BASE, EXT_FLASH_BASE + total_ext_flash_size);
                g_scan.state = STATE_COMPLETE;
                sort_partitions();
                return;

            default:
                return;
        }
    }
}

int partition_get_count(void) { return partition_count; }
partition_info_t* partition_get_info(int index) {
    if (index >= 0 && index < partition_count) return &found_partitions[index];
    return NULL;
}

/* --- Partition Operations --- */

void update_progress_ui(int pct, const char *title, const char *status) {
    ui_draw_background();
    ui_draw_header(title);
    gui_draw_progress_bar(20, 110, 280, 20, pct, COLOR_BORDER, COLOR_ACCENT);
    gui_draw_text(20, 140, status, COLOR_FG);
    gui_refresh();
    wdog_refresh();
}

void partition_erase(uint32_t addr, uint32_t size) {
    if (addr >= 0x90000000) {
        uint32_t ext_addr = addr - 0x90000000;
        uint32_t ext_size = size;
        uint32_t total = size;
        OSPI_DisableMemoryMappedMode();
        while (ext_size > 0) {
            int pct = ((total - ext_size) * 100) / total;
            update_progress_ui(pct, "ERASING EXT FLASH...", "PLEASE WAIT...");
            OSPI_Erase(&ext_addr, &ext_size, true);
        }
        OSPI_EnableMemoryMappedMode();
    } else {
        HAL_FLASH_Unlock();
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1 | FLASH_FLAG_ALL_ERRORS_BANK2);
        uint32_t bank = (addr >= 0x08100000) ? FLASH_BANK_2 : FLASH_BANK_1;
        uint32_t base = (bank == FLASH_BANK_2) ? 0x08100000 : 0x08000000;
        uint32_t start_sector = (addr - base) / 8192;
        uint32_t nb_sectors = (size + 8191) / 8192;
        for (uint32_t i = 0; i < nb_sectors; i++) {
            int pct = (i * 100) / nb_sectors;
            update_progress_ui(pct, "ERASING INT FLASH...", "PLEASE WAIT...");
            FLASH_EraseInitTypeDef erase;
            uint32_t sector_error = 0;
            erase.TypeErase = FLASH_TYPEERASE_SECTORS;
            erase.Banks = bank;
            erase.Sector = start_sector + i;
            erase.NbSectors = 1;
            HAL_FLASHEx_Erase(&erase, &sector_error);
        }
        HAL_FLASH_Lock();
    }
    update_progress_ui(100, "ERASE COMPLETE", "DONE");
}

void partition_flash_ofw(const char *name, uint32_t spi_offset, uint32_t size) {
    char status_msg[64];
    strcpy(status_msg, "PREPARING "); strcat(status_msg, name); strcat(status_msg, "...");
    update_progress_ui(0, "FLASHING...", status_msg);

    board_flash_erase();
    
    uint8_t buffer[4096];
    uint32_t total = size, written = 0;
    while (written < total) {
        uint32_t chunk = total - written;
        if (chunk > 4096) chunk = 4096;
        
        memcpy(buffer, (const void *)(EXT_FLASH_BASE + spi_offset + written), chunk);
        if (!board_flash_write(0x08100000 + written, buffer, chunk)) {
            int err_pct = (written * 100 / total);
            update_progress_ui(err_pct, "FLASH FAILED!", "WRITE ERROR");
            HAL_Delay(2000);
            return;
        }
        written += chunk;
        
        int pct = (written * 100 / total);
        char kb[16]; int_to_str(written / 1024, kb);
        strcpy(status_msg, "WRITING: "); strcat(status_msg, kb); strcat(status_msg, "KB...");
        update_progress_ui(pct, "FLASHING...", status_msg);
    }
    
    HAL_Delay(15); 
    board_detect_console_type();
    board_load_dynamic_assets();
    update_progress_ui(100, "FLASH SUCCESSFUL", "DONE");
    HAL_Delay(50);
}

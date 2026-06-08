#include "board.h"
#include "partition.h"
#include "sdcard.h"
#include "vfs.h"
#include "utils.h"
#include "flash.h"
#include "gui.h"
#include "ui.h"
#include "ui/theme.h"
#include "ui/strings.h"
#include "assets.h"
#include "system/bench.h"
#include "system/ofw_verify.h"
#include "../../common/memory_map.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* In-core RO FatFs (fatfs_ro.c): size of the mounted volume in 512-byte sectors. */
uint32_t fat_ro_total_sectors(void);

/* Scanner-local aliases for the authoritative addresses in memory_map.h (single
 * source of truth; kept as short local names for readability in the scanner). */
#define EXT_FLASH_BASE EXTFLASH_BASE
#define BANK1_BASE     CHAINLOADER_BASE
#define BANK2_BASE     OFW_INTERNAL_BASE
#define BANK_SIZE      (256 * 1024)

#define MAX_PARTITIONS 24
static partition_info_t found_partitions[MAX_PARTITIONS];
static int partition_count = 0;
uint32_t total_ext_flash_size = 0;

string_id_t partition_current_phase = STR_PHASE_MODULES;   /* shown (tr()'d) on the scan progress screen */

typedef enum {
    STATE_IDLE,
    STATE_PROBE_SD,     /* front-loaded SD module-source probe (so the theme can load early) */
    STATE_PROBE_LFS,    /* front-loaded LittleFS probe at known offsets (magic-validated) */
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

/* Set once the module sources (SD + the extflash LittleFS) are registered, so the
 * theme module can load. The front-loaded probe sets it well before the full
 * sweep finishes (fast LittleFS-probe hit); STATE_COMPLETE sets it as a fallback
 * if the probe missed (unusual offset / smaller chip). */
static bool g_modules_ready = false;

/* Upper bound (extflash offset) of the external-flash partition sweep. On the fixed
 * SD layout the region above RETROGO_CACHE_OFFSET is Retro-Go's raw ROM cache
 * (registered as a single labeled partition); the sweep stops there since raw ROM
 * bytes can false-match FS magics. Set in partition_scan_start; full chip otherwise. */
static uint32_t g_ext_scan_off_end = 0;

bool partition_modules_ready(void) { return g_modules_ready; }

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

static void add_partition(uint32_t addr, uint32_t size, const char* type, string_id_t detail_id, uint32_t detail_num) {
    if (partition_count < MAX_PARTITIONS) {
        found_partitions[partition_count].address = addr;
        found_partitions[partition_count].size = size;
        found_partitions[partition_count].type = type;
        found_partitions[partition_count].detail_id = detail_id;
        found_partitions[partition_count].detail_num = detail_num;
        partition_count++;
    }
}

/* Mount the SD via whichever FAT driver is registered and read its size in
 * 512-byte SECTORS — statfs (the RW module reports sectors), falling back to the
 * in-core RO total when statfs is unavailable. Returns true if a FAT volume
 * mounted. Shared by the boot scan (STATE_SCAN_SD) and the on-entry re-detect. */
static bool sd_probe(uint32_t *out_sectors) {
    *out_sectors = 0;
    vfs_driver_t *fat = vfs_get_driver("FAT");
    if (!fat || !fat->mount) return false;
    if (fat->mount(SDCARD_SENTINEL_ADDR, 0) != 0) return false;
    BENCH_MARK(11);   /* after FAT mount */
    uint32_t tot = 0, fre = 0;
    if (fat->statfs && fat->statfs(&tot, &fre) == 0) *out_sectors = tot;
    else                                             *out_sectors = fat_ro_total_sectors();
    BENCH_MARK(12);   /* after statfs */
    if (fat->unmount) fat->unmount();
    BENCH_MARK(13);   /* after unmount */
    return true;
}

/* True if any registered partition is a LittleFS (type starts with 'L'). The
 * front-loaded probe uses this to decide whether the module-source LittleFS was
 * found at a known offset. */
static bool have_lfs_partition(void) {
    for (int i = 0; i < partition_count; i++)
        if (found_partitions[i].type && found_partitions[i].type[0] == 'L') return true;
    return false;
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

/* If `addr` falls inside an already-found partition, return the farthest end of
 * a covering partition (so the scan can jump past the whole region in one step
 * instead of throttling through every grid point inside it); else return addr. */
static uint32_t covered_end(uint32_t addr) {
    uint32_t end = addr;
    for (int i = 0; i < partition_count; i++) {
        uint32_t pa = found_partitions[i].address;
        uint32_t pe = pa + found_partitions[i].size;
        if (addr >= pa && addr < pe && pe > end) end = pe;
    }
    return end;
}

static void find_free_space(uint32_t start, uint32_t end) {
    uint32_t current_addr = start;
    int initial_count = partition_count;
    for (int i = 0; i < initial_count; i++) {
        if (found_partitions[i].address < start || found_partitions[i].address >= end)
            continue;
        if (found_partitions[i].address > current_addr) {
            add_partition(current_addr, found_partitions[i].address - current_addr, "FREE", STR_DETAIL_EMPTY, 0);
        }
        uint32_t next_addr = found_partitions[i].address + found_partitions[i].size;
        if (next_addr > current_addr) current_addr = next_addr;
    }
    if (current_addr < end) {
        add_partition(current_addr, end - current_addr, "FREE", STR_DETAIL_EMPTY, 0);
    }
}

static const char* identify_firmware(uint32_t addr) {
    if (addr == RETROGO_BASE) return "Retro-Go";   /* scanner probe hint; tracks the 40K ceiling via the define, never a literal */
    
    /* Fast check: Stock reset vectors at offset 4 */
    uint32_t pc = *(volatile uint32_t *)(addr + 4);
    if (pc == 0x08018101UL) return "OFW Mario";
    if (pc == 0x0801B3E1UL) return "OFW Zelda";
    
    return "Firmware";
}

typedef struct {
    const char *type;
    string_id_t detail_id;
    uint32_t size;        /* 0 for dynamic scan */
    uint32_t sig_offset;
    const uint8_t *sig;
    uint8_t sig_len;
} sig_probe_t;

static const sig_probe_t STATIC_PROBES[] = {
    {"Zelda",    STR_DETAIL_ASSETS,     4*1024*1024, 0,       (const uint8_t*)"\xBE\xBA\xAD\xFE", 4},
    {"Zelda",    STR_DETAIL_ASSETS,     4*1024*1024, 0x4FFEB, (const uint8_t*)"ZELDA", 5},
    {"Mario",    STR_DETAIL_ASSETS,     1*1024*1024, 0,       (const uint8_t*)"\x78\xD8\xA9\x10", 4},
    {"Mario",    STR_DETAIL_OFW_BACKUP, 128*1024,    4,       (const uint8_t*)"\x01\x81\x01\x08", 4},
    {"Zelda",    STR_DETAIL_OFW_BACKUP, 128*1024,    4,       (const uint8_t*)"\xE1\xB3\x01\x08", 4},
    /* Retro-Go is NOT probed here: its base (RETROGO_BASE) is off the 16 KiB scan
     * grid and a fixed-offset signature is fragile. It is registered deterministically
     * by probe_retrogo() below, keyed off the version banner. */
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
                string_id_t detail_id = p->detail_id;

                if (addr + size <= bank_end) {
                    if (is_internal && detail_id == STR_DETAIL_OFW_BACKUP) {
                        detail_id = STR_DETAIL_APP_BIN;
                    }
                    add_partition(addr, size, type, detail_id, 0);
                    return;
                }
            }
        }
    }

    if (is_internal && board_is_valid_app(addr)) {
        if (addr == BANK1_BASE) {
            /* Chainloader occupies Bank 1 up to the Retro-Go payload base; derive the
             * size from the define so it tracks the ceiling (was a stale 0x8000/32 KiB
             * literal after the 32K->40K raise). */
            add_partition(addr, RETROGO_BASE - BANK1_BASE, "Chainloader", STR_DETAIL_CHAINLOADER, 0);
        } else {
            add_partition(addr, 128*1024, identify_firmware(addr), STR_DETAIL_APP_BIN, 0);
        }
        return;
    }

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        if (sector[0] == 0xEB || sector[0] == 0xE9) {
            /* Size from the BPB: bytes-per-sector (0x0B) times the total sector count (the 16-bit
             * field at 0x13, or the 32-bit field at 0x20 when that is zero). A flash FAT partition
             * (the module store) can be any size, so never assume 1 MiB; fall back to 1 MiB only if
             * the BPB looks implausible or the computed size would run past the bank. */
            uint32_t bps   = sector[0x0B] | (sector[0x0C] << 8);
            uint32_t tot16 = sector[0x13] | (sector[0x14] << 8);
            uint32_t tot32 = sector[0x20] | (sector[0x21] << 8) | (sector[0x22] << 16) | (sector[0x23] << 24);
            uint32_t total_sectors = tot16 ? tot16 : tot32;
            uint32_t fat_size = 1024 * 1024;
            if (bps >= 512 && bps <= 4096 && total_sectors &&
                (uint64_t)total_sectors * bps <= (uint64_t)(bank_end - addr)) {
                fat_size = total_sectors * bps;
            }
            add_partition(addr, fat_size, "FAT", STR_DETAIL_FS, 0);
            return;
        }
    }

    if (memcmp(sector, "FROG", 4) == 0) {
        uint16_t num_entries = sector[6] | (sector[7] << 8);
        uint32_t bin_sz = sector[8] | (sector[9] << 8) | (sector[10] << 16) | (sector[11] << 24);
        add_partition(addr, bin_sz, "FrogFS", STR_FROG_ENTRIES, num_entries);
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
                if (phys_start >= (is_internal ? BANK1_BASE : EXT_FLASH_BASE) && phys_start + size <= bank_end) {
                    add_partition(phys_start, size, "LittleFS", STR_LFS_BLOCKS, block_count);
                    return;
                }
            }
        }
    }
}

const char *partition_retrogo_version(uint32_t addr, uint32_t size) {
    static const char sig[] = "Retro-Go SD v";
    const uint32_t siglen = sizeof(sig) - 1;   /* 13 bytes, excludes NUL */
    if (size < siglen) return NULL;
    const char *base = (const char *)addr;
    for (uint32_t i = 0; i + siglen <= size; i++) {
        if (base[i] == 'R' && memcmp(base + i, sig, siglen) == 0)
            return base + i + (siglen - 1);   /* the "v..." token, NUL-terminated in flash */
    }
    return NULL;
}

/* Register the Retro-Go launcher payload at its fixed base (RETROGO_BASE) when its
 * version banner is present. Deterministic (the base is a build-time constant, not a
 * scan-grid hit) and version-proof (matches the moving banner, not a stale fixed
 * offset). Retro-Go is always the last occupant of Bank 1, so it owns everything from
 * its base to the end of the bank (no grow-scan; that left an 8 KiB tail gap). */
static void probe_retrogo(void) {
    uint32_t base = RETROGO_BASE;
    uint32_t bank_end = BANK1_BASE + BANK_SIZE;
    if (base >= bank_end || is_covered(base)) return;
    if (!partition_retrogo_version(base, bank_end - base)) return;
    add_partition(base, bank_end - base, "Retro-Go", STR_DETAIL_APP_BIN, 0);
}

/* Fixed SD layout: the module-source LittleFS is a bounded partition butted directly
 * against the FAT store, occupying [MODULE_LFS_OFFSET_SD, RETROGO_CACHE_OFFSET). It is
 * stored INVERTED, so its superblock sits in the TOP block(s) of the region (just below
 * RETROGO_CACHE_OFFSET), not at the (erased) start — the generic grid sweep would miss
 * it. Validate that superblock and register the partition at its back-computed start
 * (size + block_count come from the superblock so the mount geometry is exact). Caller
 * guarantees total_ext_flash_size >= RETROGO_CACHE_OFFSET. */
static void probe_fixed_sd_lfs(void) {
    uint32_t region_end = EXT_FLASH_BASE + RETROGO_CACHE_OFFSET;   /* LFS top == ROM-cache base */
    for (uint32_t a = region_end - MODULE_LFS_END_WINDOW; a + 32 <= region_end; a += 4096) {
        const uint8_t *blk = (const uint8_t *)a;
        if (memcmp(&blk[8], "littlefs", 8) != 0) continue;
        uint32_t version     = blk[20] | (blk[21] << 8) | (blk[22] << 16) | (blk[23] << 24);
        uint32_t block_size  = blk[24] | (blk[25] << 8) | (blk[26] << 16) | (blk[27] << 24);
        uint32_t block_count = blk[28] | (blk[29] << 8) | (blk[30] << 16) | (blk[31] << 24);
        if ((version >> 16) != 2 || block_size < 128 || block_size > 8192 || block_count == 0) continue;
        uint32_t size = block_size * block_count;          /* inverted: FS grows down from region_end */
        uint32_t phys_start = region_end - size;
        if (phys_start >= EXT_FLASH_BASE + MODULE_LFS_OFFSET_SD) {
            add_partition(phys_start, size, "LittleFS", STR_LFS_BLOCKS, block_count);
            return;
        }
    }
}

void partition_scan_start(void) {
    partition_count = 0;
    total_ext_flash_size = board_ospi_get_size();
    /* Fixed SD layout: don't sweep the Retro-Go ROM cache (raw ROM bytes can false-match
     * FS magics); it's registered as one labeled partition instead. Full chip otherwise. */
    g_ext_scan_off_end = (total_ext_flash_size >= RETROGO_CACHE_OFFSET)
                       ? RETROGO_CACHE_OFFSET : total_ext_flash_size;

    g_scan.progress_done = 0;
    g_scan.progress_total = count_check_points(BANK1_BASE, BANK1_BASE + BANK_SIZE, 16384)
                          + count_check_points(BANK2_BASE, BANK2_BASE + BANK_SIZE, 16384)
                          + count_check_points(EXT_FLASH_BASE, EXT_FLASH_BASE + g_ext_scan_off_end, 65536);
    
    g_modules_ready = false;
    /* Probe module sources (SD, then the LittleFS at its known offset) FIRST so
     * the theme can load without waiting for the full sweep. The INT1 params are
     * staged here and consumed once the two probe states finish. */
    g_scan.state = STATE_PROBE_SD;
    g_scan.stride = 1024 * 1024;
    g_scan.addr = BANK1_BASE;
    g_scan.range_start = BANK1_BASE;
    g_scan.range_end = BANK1_BASE + BANK_SIZE;
    g_scan.min_stride = 16384;
    partition_current_phase = STR_PHASE_MODULES;
}

void partition_scan_update(void) {
    BENCH_SCAN_ENTER(g_scan.state);   /* per-stage entry tick (no-op without BOOT_BENCH) */
    if (g_scan.state == STATE_IDLE || g_scan.state == STATE_COMPLETE) return;

    // Probe a batch of points per UI frame. Each probe is a handful of small
    // memcmps, so a generous batch keeps the scan snappy (the menu is idle while
    // scanning) without a visible hitch — covered regions are jumped, not stepped.
    for (int step = 0; step < 32; step++) {
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

                /* Jump past an already-identified partition (rounded up to the
                 * current grid) rather than stepping through every covered point
                 * — those are throttled and would otherwise burn many frames on
                 * large filesystems. Nothing new lives inside a found FS. */
                uint32_t cend = covered_end(g_scan.addr);
                if (cend > g_scan.addr) {
                    g_scan.addr = (cend + g_scan.stride - 1) & ~(g_scan.stride - 1);
                    break;
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

            case STATE_PROBE_SD: {
                /* Detect an SD card (off the boot-critical path — this runs from
                 * the menu loop) and, if it carries a mountable FAT volume, add
                 * it as a synthetic partition. Front-loaded so a module on the
                 * card (/modules/...) is reachable before the theme loads. */
                /* NOTE: for the SD partition only, `size` is the volume size in
                 * 512-byte SECTORS (not bytes) — bytes overflow uint32_t past
                 * 4 GB. Display code keys off partition_is_sd to format it via
                 * format_size_sectors(). */
                BENCH_MARK(9);                  /* SD probe start */
                bool present = sdcard_detect();
                BENCH_MARK(10);                 /* after detect */
                if (present) {
                    uint32_t sectors = 0;
                    if (sd_probe(&sectors))
                        add_partition(SDCARD_SENTINEL_ADDR, sectors, "FAT", STR_DIV_SDCARD, 0);
                }
                g_scan.state = STATE_PROBE_LFS;
                return;
            }

            case STATE_PROBE_LFS:
                /* Register the module-source LittleFS (and, on the fixed SD layout, the
                 * Retro-Go ROM cache) before the full sweep so the theme can load early.
                 * The superblock magic is validated, so a wrong/absent layout registers
                 * nothing and we fall back to ready-at-STATE_COMPLETE. */
                if (total_ext_flash_size >= RETROGO_CACHE_OFFSET) {
                    /* Fixed SD layout (DESIGN.md §2): the LittleFS is a bounded, inverted
                     * partition butted against the FAT store, and everything above
                     * RETROGO_CACHE_OFFSET is Retro-Go's raw ROM cache. Validate + register
                     * the LFS at its known span, then register the cache region (hardcoded;
                     * already excluded from the sweep in partition_scan_start). */
                    probe_fixed_sd_lfs();
                    add_partition(EXT_FLASH_BASE + RETROGO_CACHE_OFFSET,
                                  total_ext_flash_size - RETROGO_CACHE_OFFSET,
                                  "Retro-Go Cache", STR_DETAIL_ROM_CACHE, 0);
                } else {
                    /* Legacy non-SD / small-chip layouts: LittleFS inverted at the very end
                     * of flash (primary), or non-inverted at a standard patcher offset
                     * (secondary). check_address() validates the superblock either way. */
                    if (total_ext_flash_size >= MODULE_LFS_END_WINDOW)
                        check_address(EXT_FLASH_BASE + total_ext_flash_size - MODULE_LFS_END_WINDOW);
                    check_address(EXT_FLASH_BASE + MODULE_LFS_OFFSET_SD);
                    check_address(EXT_FLASH_BASE + MODULE_LFS_OFFSET_FLASH);
                }
                if (have_lfs_partition() || total_ext_flash_size == 0)
                    g_modules_ready = true;
                /* Register the Retro-Go payload at its fixed base before the Bank-1
                 * grid sweep, so is_covered() skips its region and the free-space
                 * pass accounts for it. */
                probe_retrogo();
                g_scan.state = STATE_SCAN_INT1;
                partition_current_phase = STR_PHASE_INT_FLASH;
                return;

            case STATE_GAP_INT2:
                sort_partitions();
                find_free_space(BANK2_BASE, BANK2_BASE + BANK_SIZE);
                /* SD was probed up front (STATE_PROBE_SD); go straight to the
                 * external-flash sweep, or finish if there's no extflash. */
                if (total_ext_flash_size > 0) {
                    g_scan.state = STATE_SCAN_EXT;
                    g_scan.stride = 1024 * 1024;
                    g_scan.addr = EXT_FLASH_BASE;
                    g_scan.range_start = EXT_FLASH_BASE;
                    g_scan.range_end = EXT_FLASH_BASE + g_ext_scan_off_end;   /* skip the Retro-Go ROM cache (registered separately) */
                    g_scan.min_stride = 65536;
                    partition_current_phase = STR_PHASE_EXT_FLASH;
                } else {
                    g_scan.state = STATE_COMPLETE;
                    sort_partitions();
                    g_modules_ready = true;
                }
                return;

            case STATE_GAP_EXT:
                sort_partitions();
                find_free_space(EXT_FLASH_BASE, EXT_FLASH_BASE + total_ext_flash_size);
                g_scan.state = STATE_COMPLETE;
                sort_partitions();
                g_modules_ready = true;
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

/*
 * Re-probe ONLY the SD card and reconcile the synthetic SD partition, leaving
 * the flash partitions found by the full boot scan untouched. There's no
 * card-detect line, so this is how an insert/remove/swap after boot is noticed:
 * the File Browser / Partition Viewer call it on entry. Returns true if the
 * partition set changed (card appeared, vanished, or a swap changed its size),
 * so the caller can refresh any state holding partition pointers.
 */
bool partition_redetect_sd(void) {
    int sd_idx = -1;
    for (int i = 0; i < partition_count; i++)
        if (partition_is_sd(&found_partitions[i])) { sd_idx = i; break; }

    uint32_t sectors = 0;
    bool present = sdcard_detect() && sd_probe(&sectors);

    if (present) {
        if (sd_idx < 0) {                      /* card appeared */
            add_partition(SDCARD_SENTINEL_ADDR, sectors, "FAT", STR_DIV_SDCARD, 0);
            sort_partitions();
            return true;
        }
        bool changed = (found_partitions[sd_idx].size != sectors);  /* card swapped? */
        found_partitions[sd_idx].size = sectors;
        return changed;
    }

    if (sd_idx >= 0) {                          /* card removed */
        for (int i = sd_idx; i < partition_count - 1; i++)
            found_partitions[i] = found_partitions[i + 1];
        partition_count--;
        return true;
    }
    return false;
}

/* --- Shared partition classification (loader + Partition Viewer + File Browser) --- */
bool partition_is_sd(const partition_info_t *p) {
    return p && p->address == SDCARD_SENTINEL_ADDR;
}

const char *partition_driver_name(const partition_info_t *p) {
    if (!p || !p->type) return NULL;
    char t0 = p->type[0];
    if (t0 == 'L') return "LFS";
    if (t0 == 'F') {
        if (p->type[1] == 'r') return "FROGFS";
        if (p->type[1] == 'A') return "FAT";
    }
    return NULL;
}

const char *partition_fs_code(const partition_info_t *p) {
    const char *dn = partition_driver_name(p);
    if (dn && dn[0] == 'F' && dn[1] == 'R') return "FROG";
    return dn;
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
    if (addr >= EXT_FLASH_BASE) {
        uint32_t ext_addr = addr - EXT_FLASH_BASE;
        uint32_t ext_size = size;
        uint32_t total = size;
        OSPI_DisableMemoryMappedMode();
        while (ext_size > 0) {
            int pct = ((total - ext_size) * 100) / total;
            update_progress_ui(pct, tr(STR_ERASING_EXT), tr(STR_PLEASE_WAIT));
            OSPI_Erase(&ext_addr, &ext_size, true);
        }
        OSPI_EnableMemoryMappedMode();
    } else {
        HAL_FLASH_Unlock();
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1 | FLASH_FLAG_ALL_ERRORS_BANK2);
        uint32_t bank = (addr >= BANK2_BASE) ? FLASH_BANK_2 : FLASH_BANK_1;
        uint32_t base = (bank == FLASH_BANK_2) ? BANK2_BASE : BANK1_BASE;
        uint32_t start_sector = (addr - base) / 8192;
        uint32_t nb_sectors = (size + 8191) / 8192;
        for (uint32_t i = 0; i < nb_sectors; i++) {
            int pct = (i * 100) / nb_sectors;
            update_progress_ui(pct, tr(STR_ERASING_INT), tr(STR_PLEASE_WAIT));
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
    update_progress_ui(100, tr(STR_ERASE_COMPLETE), tr(STR_DONE));
}

bool partition_flash_ofw(const char *name, uint32_t spi_offset, uint32_t size) {
    char status_msg[64];
    str_fmt1_str(status_msg, sizeof(status_msg), tr(STR_PREPARING), name);
    update_progress_ui(0, tr(STR_FLASHING), status_msg);

    /* Verify the backup image AND its paired asset blob against the baked CRC
     * signatures BEFORE erasing Bank 2. A recognized-but-mismatched/corrupt OFW
     * is refused here, so we never swap into an OFW we cannot boot (STABILITY IS
     * LAW). The "PREPARING" screen above stays up during the CRC sweep. */
    if (!ofw_verify_by_spi(spi_offset)) {
        update_progress_ui(0, tr(STR_FLASH_FAILED), tr(STR_ERROR));
        HAL_Delay(2000);
        return false;
    }

    if (!board_flash_erase()) {
        update_progress_ui(0, tr(STR_FLASH_FAILED), tr(STR_ERROR));
        HAL_Delay(2000);
        return false;
    }
    
    uint8_t buffer[4096];
    uint32_t total = size, written = 0;
    while (written < total) {
        uint32_t chunk = total - written;
        if (chunk > 4096) chunk = 4096;
        
        memcpy(buffer, (const void *)(EXT_FLASH_BASE + spi_offset + written), chunk);
        if (!board_flash_write(BANK2_BASE + written, buffer, chunk)) {
            int err_pct = (written * 100 / total);
            update_progress_ui(err_pct, tr(STR_FLASH_FAILED), tr(STR_WRITE_ERROR));
            HAL_Delay(2000);
            return false;
        }
        written += chunk;
        
        int pct = (written * 100 / total);
        str_fmt1_int(status_msg, sizeof(status_msg), tr(STR_WRITING), (int)(written / 1024));
        update_progress_ui(pct, tr(STR_FLASHING), status_msg);
    }
    
    HAL_Delay(15);
    board_detect_console_type();
    board_load_dynamic_assets();
    /* OFW changed in bank2 -> re-register the theme module for the new OFW
     * (clears the stale Zelda/Mario themes) and re-apply the persisted slot, so
     * the theme updates live without a reboot. */
    theme_modules_init();
    update_progress_ui(100, tr(STR_FLASH_OK), tr(STR_DONE));
    HAL_Delay(50);
    return true;
}

#include "vfs.h"
#include "flash.h"
#include "board.h"
#include "lfs.h"
#include "frogfs_reader.h"
#include "partition.h"
#include <string.h>
#include "stm32h7xx.h"

#define DRIVER_RAM_ADDR 0x240A0000UL

static vfs_driver_t *g_drivers[8];
static int g_driver_count = 0;
static host_api_t g_host_api;

static int host_flash_read(uint32_t addr, void *buf, size_t size) {
    memcpy(buf, (const void *)(0x90000000UL + addr), size);
    return 0;
}

static inline int dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0;
}

static inline void dcache_before_write(uint32_t addr, uint32_t size) {
    if (dcache_enabled()) {
        SCB_CleanDCache_by_Addr((uint32_t *)addr, size);
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size);
        SCB_DisableDCache();
    }
}

static inline void dcache_after_write(uint32_t addr, uint32_t size) {
    if (dcache_enabled()) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size);
        SCB_EnableDCache();
    }
}

static int host_flash_write(uint32_t addr, const void *buf, size_t size) {
    uint32_t mem_addr = 0x90000000UL + addr;
    dcache_before_write(mem_addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_Program(addr, buf, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(mem_addr, size);
    return 0;
}

static int host_flash_erase(uint32_t addr, uint32_t size) {
    uint32_t mem_addr = 0x90000000UL + addr;
    dcache_before_write(mem_addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_EraseSync(addr, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(mem_addr, size);
    return 0;
}

extern void lfs_vfs_init(void);
extern void frogfs_vfs_init(void);

void vfs_init(void) {
    g_driver_count = 0;
    memset(g_drivers, 0, sizeof(g_drivers));
    
    lfs_vfs_init();
    frogfs_vfs_init();
    
    g_host_api.get_tick = HAL_GetTick;
    g_host_api.flash_read = host_flash_read;
    g_host_api.flash_write = host_flash_write;
    g_host_api.flash_erase = host_flash_erase;
    g_host_api.memcpy = memcpy;
    g_host_api.memset = memset;
    g_host_api.memcmp = memcmp;
    g_host_api.strlen = strlen;
    g_host_api.strcpy = strcpy;
    g_host_api.strncpy = strncpy;
    g_host_api.strcmp = strcmp;
    g_host_api.strcat = strcat;
    g_host_api.get_fattime = board_rtc_get_fattime;
}

static bool g_lfs_rw_loaded = false;

bool vfs_is_lfs_rw_loaded(void) {
    return g_lfs_rw_loaded;
}

int vfs_register_driver(vfs_driver_t *driver) {
    if (g_driver_count >= 8) return -1;
    g_drivers[g_driver_count++] = driver;
    return 0;
}

vfs_driver_t* vfs_get_driver(const char *name) {
    for (int i = g_driver_count - 1; i >= 0; i--) {
        if (strcmp(g_drivers[i]->name, name) == 0) {
            return g_drivers[i];
        }
    }
    return NULL;
}

int vfs_load_dynamic_driver(const char *name, const char *bin_path) {
    if (strcmp(name, "LFS") == 0) {
        if (g_lfs_rw_loaded) return 0;
    } else {
        if (vfs_get_driver(name) != NULL) return 0;
    }
    
    uint32_t load_addr = DRIVER_RAM_ADDR;
    if (strcmp(name, "LFS") == 0) {
        load_addr = 0x240C0000UL;
    }
    
    int p_count = partition_get_count();
    for (int i = 0; i < p_count; i++) {
        partition_info_t *part = partition_get_info(i);
        if (!part) continue;
        
        char t0 = part->type[0];
        char t1 = part->type[1];
        
        if (t0 == 'L') {
            extern int lfs_mount_at(uint32_t base_addr, uint32_t block_count);
            extern lfs_t lfs;
            if (lfs_mount_at(part->address, part->size / 4096) >= 0) {
                lfs_file_t file;
                struct lfs_file_config f_cfg = {0};
                static uint8_t file_buf[256];
                f_cfg.buffer = file_buf;
                bool read_ok = false;
                lfs_soff_t sz = 0;
                if (lfs_file_opencfg(&lfs, &file, bin_path, LFS_O_RDONLY, &f_cfg) >= 0) {
                    sz = lfs_file_size(&lfs, &file);
                    if (sz > 0 && sz <= 128 * 1024) {
                        lfs_ssize_t read_bytes = lfs_file_read(&lfs, &file, (void *)load_addr, sz);
                        lfs_file_close(&lfs, &file);
                        if (read_bytes == sz) {
                            read_ok = true;
                        }
                    } else {
                        lfs_file_close(&lfs, &file);
                    }
                }
                
                lfs_unmount(&lfs);
                
                if (read_ok) {
                    SCB_CleanDCache_by_Addr((uint32_t *)load_addr, sz);
                    SCB_InvalidateICache();
                    
                    typedef void (*driver_init_fn)(vfs_driver_t *drv, const host_api_t *api);
                    driver_init_fn init = (driver_init_fn)(load_addr | 1);
                    
                    static vfs_driver_t dyn_drv_fat;
                    static vfs_driver_t dyn_drv_lfs;
                    vfs_driver_t *dyn_drv = (strcmp(name, "LFS") == 0) ? &dyn_drv_lfs : &dyn_drv_fat;
                    memset(dyn_drv, 0, sizeof(vfs_driver_t));
                    init(dyn_drv, &g_host_api);
                    
                    if (vfs_register_driver(dyn_drv) == 0) {
                        if (strcmp(name, "LFS") == 0) {
                            g_lfs_rw_loaded = true;
                        }
                        return 0;
                    }
                }
            }
        } else if (t0 == 'F' && t1 == 'r') {
            uint32_t sz = 0;
            const void *src_ptr = frogfs_get_file_data((const void*)part->address, bin_path, &sz);
            if (src_ptr && sz > 0 && sz <= 128 * 1024) {
                memcpy((void *)load_addr, src_ptr, sz);
                SCB_CleanDCache_by_Addr((uint32_t *)load_addr, sz);
                SCB_InvalidateICache();
                
                typedef void (*driver_init_fn)(vfs_driver_t *drv, const host_api_t *api);
                driver_init_fn init = (driver_init_fn)(load_addr | 1);
                
                static vfs_driver_t dyn_drv_fat;
                static vfs_driver_t dyn_drv_lfs;
                vfs_driver_t *dyn_drv = (strcmp(name, "LFS") == 0) ? &dyn_drv_lfs : &dyn_drv_fat;
                memset(dyn_drv, 0, sizeof(vfs_driver_t));
                init(dyn_drv, &g_host_api);
                
                if (vfs_register_driver(dyn_drv) == 0) {
                    if (strcmp(name, "LFS") == 0) {
                        g_lfs_rw_loaded = true;
                    }
                    return 0;
                }
            }
        }
    }
    
    return -1;
}

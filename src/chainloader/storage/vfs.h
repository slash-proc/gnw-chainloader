#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // for size_t

#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR  2

typedef struct {
    char name[64];
    uint8_t type;
    uint32_t size;
} vfs_dirent_t;

typedef struct {
    const char *name; // e.g. "FAT", "LFS", "FROGFS"
    
    int (*mount)(uint32_t base_addr, uint32_t size);
    int (*unmount)(void);
    
    int (*opendir)(const char *path, void **dir_ctx);
    int (*readdir)(void *dir_ctx, vfs_dirent_t *ent);
    int (*closedir)(void *dir_ctx);
    
    int (*open)(const char *path, int mode, void **file_ctx);
    int (*close)(void *file_ctx);
    int (*read)(void *file_ctx, void *buf, size_t len, size_t *read_len);
    int (*write)(void *file_ctx, const void *buf, size_t len, size_t *written_len);
    int (*unlink)(const char *path);
    int (*statfs)(uint32_t *total_bytes, uint32_t *free_bytes);
} vfs_driver_t;

// API functions that the host exposes to dynamic drivers
typedef struct {
    uint32_t (*get_tick)(void);
    int (*flash_read)(uint32_t addr, void *buf, size_t size);
    int (*flash_write)(uint32_t addr, const void *buf, size_t size);
    int (*flash_erase)(uint32_t addr, uint32_t size);
    void* (*memcpy)(void *dst, const void *src, size_t n);
    void* (*memset)(void *s, int c, size_t n);
    int (*memcmp)(const void *s1, const void *s2, size_t n);
    size_t (*strlen)(const char *s);
    char* (*strcpy)(char *dst, const char *src);
    char* (*strncpy)(char *dst, const char *src, size_t n);
    int (*strcmp)(const char *s1, const char *s2);
    char* (*strcat)(char *dst, const char *src);
    uint32_t (*get_fattime)(void);
} host_api_t;

// Host VFS Manager functions
void vfs_init(void);
int vfs_register_driver(vfs_driver_t *driver);
vfs_driver_t* vfs_get_driver(const char *name);

// Loader helper
int vfs_load_dynamic_driver(const char *name, const char *bin_path);
bool vfs_is_lfs_rw_loaded(void);

#endif // VFS_H

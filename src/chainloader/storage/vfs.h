#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // for size_t

#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR  2

typedef struct {
    char name[256];   /* full LFN (FatFs FF_LFN_BUF max 255 + NUL) */
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
    /* Create one directory (parent must exist). 0 on success. RW only — the
     * in-core RO drivers leave this NULL; the RW PIE modules provide it. */
    int (*mkdir)(const char *path);
    /* Capacity in 512-byte SECTORS (not bytes), so volumes > 4 GB don't overflow
     * a uint32_t. Returns 0 on success, non-zero if free space is unavailable. */
    int (*statfs)(uint32_t *total_sectors, uint32_t *free_sectors);
} vfs_driver_t;

/* Streaming-copy result codes (vfs_copy_open_file). */
#define VFS_COPY_OK       0
#define VFS_COPY_OPEN    -1
#define VFS_COPY_READ    -2
#define VFS_COPY_WRITE   -3
#define VFS_COPY_FULL    -4
#define VFS_COPY_CANCEL  -5

/* Optional progress/cancel callback for vfs_copy_open_file: gets the running
 * percentage (0 when the size is unknown); return non-zero to cancel. */
typedef int (*vfs_copy_cb)(int pct, void *user);

// API functions that the host exposes to dynamic drivers
typedef struct {
    uint32_t (*get_tick)(void);
    int (*flash_read)(uint32_t addr, void *buf, size_t size);
    int (*flash_write)(uint32_t addr, const void *buf, size_t size);
    int (*flash_erase)(uint32_t addr, uint32_t size);
    /* 512-byte SD sector I/O (in-core transport; the RW FAT module reaches the
     * card through these). Return 0 on success. */
    int (*sd_read)(uint32_t sector, void *buf, uint32_t count);
    int (*sd_write)(uint32_t sector, const void *buf, uint32_t count);
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
bool vfs_is_fat_rw_loaded(void);

// True if a module .bin at `path` can be found on any read-capable filesystem
// (SD first). Cheap existence probe (open/close, no full read) used to decide
// whether a partition can be made RW and as the first half of versioned loading.
bool vfs_module_available(const char *path);

// Fetch a PIE module .bin by path for the loader, selecting the HIGHEST-version
// copy across all read-capable filesystems (SD card first on ties). Returns 0
// on success. See vfs.c for the two-phase (peek-headers then load-winner) scan.
int vfs_read_module(const char *path, void *dst, uint32_t max, uint32_t *out_size);

// Read an arbitrary file by path (SD first, then other read-capable FSes) into
// dst, up to `max` bytes. No magic/version check — for i18n .fnt/.lang assets.
// Returns 0 on success (sets *out_size), -1 if absent/unreadable.
int vfs_read_file(const char *path, void *dst, uint32_t max, uint32_t *out_size);

// Read the canonical .lang pack at `path` from the main LittleFS ONLY (no SD
// scan) whose ABI == want_abi. The hot path for language switching. Returns 0 on
// success (sets *out_size), -1 if absent/incompatible.
int vfs_read_lang_lfs(const char *path, void *dst, uint32_t max, uint32_t *out_size,
                      uint16_t want_abi);

// Version of the .lang pack at `path` on the main LittleFS (0 if absent / wrong
// magic / ABI mismatch). Used by the SD install to decide what is newer.
uint32_t vfs_lfs_lang_version(const char *path, uint16_t want_abi);

// True (1) if `path` exists on the main LittleFS (cheap 1-byte probe).
int vfs_lfs_has(const char *path);

// Enumerate /i18n/<code>.lang on the main LittleFS; for each pack with a valid magic
// and ABI == want_abi, invoke cb with its self-described code/endonym/script
// (pointers into a transient header buffer — copy in the callback).
void vfs_lfs_enum_langs(uint16_t want_abi,
                        void (*cb)(const char *code, const char *endonym, const char *script));

// True (1) if directory `dir` exists on the SD card, probed with whatever FAT
// driver is currently loaded (the in-core 8.3 one suffices for a dir name) — so a
// caller can skip loading the LFN FAT-RW module on a card with no such folder.
int vfs_sd_dir_exists(const char *dir);

// Collect the basenames of /i18n/<code>.lang on the SD card into names[max][stride]
// (needs the LFN FAT-RW driver loaded). Returns the count.
int vfs_sd_list_langs(char *names, int stride, int max);

// Free space on the main (on-flash) LittleFS in KiB, or -1 if unavailable.
int vfs_lfs_free_kb(void);
// Write a file to the main LittleFS (loads the RW module). 0 ok, -1 on failure.
int vfs_lfs_write_file(const char *path, const void *data, uint32_t len);

/* Stream-copy `sp` (opened via src) to `dp` (opened via dst) in 4 KiB chunks; both
 * drivers must already be mounted. The one in-core copy loop, shared by the file
 * browser and (via vfs_copy_sd_to_lfs) by modules. Returns VFS_COPY_OK or a
 * VFS_COPY_* error; removes a partial destination on failure. */
int vfs_copy_open_file(vfs_driver_t *src, const char *sp,
                       vfs_driver_t *dst, const char *dp,
                       uint32_t size_hint, vfs_copy_cb cb, void *user);

/* Stream-copy an SD-card file to the main LittleFS (loads FAT + lfs_rw as needed,
 * creates the immediate parent dir). 0 on success. The module-facing seam. */
int vfs_copy_sd_to_lfs(const char *sd_path, const char *lfs_path);

/* Read the first `n` bytes (header only) of an SD / LittleFS file. 0 on success. */
int vfs_sd_read_header(const char *path, void *dst, uint32_t n);
int vfs_lfs_read_header(const char *path, void *dst, uint32_t n);

#endif // VFS_H

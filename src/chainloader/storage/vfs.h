#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // for size_t
#include "partition.h"   // partition_info_t for vfs_enum_dir

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
    /* Seek the open file to a byte offset (media scrubbing). Streaming RW modules provide
     * it; in-core RO drivers may leave it NULL. 0 on success. */
    int (*seek)(void *file_ctx, uint32_t offset);
    /* Create a file CONTIGUOUS via f_expand(size) so it is XIP-able in place. RW FAT on memory-mapped
     * flash only; NULL on other drivers (the caller falls back to open). 0 on success. */
    int (*open_expand)(const char *path, uint32_t size, void **file_ctx);
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
void vfs_ensure_rw(const char *name);   /* load the RW driver for a filesystem on demand */
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

// For execute-in-place: like vfs_read_module's selection, but if the winning copy is a
// CONTIGUOUS file on a memory-mapped FAT partition, return its mapped-flash address + size
// (no copy) so the loader can run .text in place. Returns -1 when the winner is not XIP-able
// (SD, LittleFS, non-FAT, or fragmented); the loader then falls back to vfs_read_module.
int vfs_map_module(const char *path, uint32_t *out_addr, uint32_t *out_size);

// Map an arbitrary file (e.g. a .fnt) to its contiguous mapped-flash address for read-in-place,
// no version scan or header check. Returns -1 if it is not a contiguous file on a memory-mapped
// FAT partition; the caller falls back to a RAM read. Backs fonts-XIP.
int vfs_map_file(const char *path, uint32_t *out_addr, uint32_t *out_size);

// Read an arbitrary file by path (SD first, then other read-capable FSes) into
// dst, up to `max` bytes. No magic/version check — for i18n .fnt/.lang assets.
// Returns 0 on success (sets *out_size), -1 if absent/unreadable.
int vfs_read_file(const char *path, void *dst, uint32_t max, uint32_t *out_size);

// Generic LFS-only read (no SD scan): the hot path for language packs, which must
// never touch the slow SD card. The CALLER validates the file format (the language
// module owns the .lang magic/ABI/header knowledge). 0 on success (sets *out_size).
int vfs_lfs_read(const char *path, void *dst, uint32_t max, uint32_t *out_size);

// Version of the .lang pack at `path` on the main LittleFS (0 if absent / wrong
// magic / ABI mismatch). Used by the SD install to decide what is newer.
uint32_t vfs_lfs_lang_version(const char *path, uint16_t want_abi);

// True (1) if `path` exists on the main LittleFS (cheap 1-byte probe).
int vfs_lfs_has(const char *path);

// Enumerate the FILE entries of `dir` on the main on-flash LittleFS, calling cb with
// each name (transient pointer -- copy in the callback). Used by feature-module
// discovery to scan /modules/features. Mounts/unmounts the LFS itself.
void vfs_lfs_enum_dir(const char *dir, void (*cb)(const char *name));

// Enumerate the FILE entries of `dir` on a SPECIFIC partition (any driver), so feature discovery
// can scan the EXT-FAT module store and not just the LFS. Mounts/unmounts that partition itself.
void vfs_enum_dir(partition_info_t *p, const char *dir, void (*cb)(const char *name));

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

/* Delete a file from the SD card (loads FAT-RW if needed). 0 on success, -1 if absent
 * or RW unavailable. The installer removes an SD source after it is installed to LFS. */
int vfs_sd_unlink(const char *path);

/* Read the first `n` bytes (header only) of an SD / LittleFS file. 0 on success. */
int vfs_sd_read_header(const char *path, void *dst, uint32_t n);
int vfs_lfs_read_header(const char *path, void *dst, uint32_t n);

/* Generic install primitives (any source/dest partition): the installer module scans SD/LFS/FAT,
 * copies to FAT (contiguous via size -> f_expand) or LFS, and deletes an RW source. */
int vfs_read_header(partition_info_t *part, const char *path, void *dst, uint32_t n);
int vfs_install_copy(partition_info_t *sp_part, const char *sp,
                     partition_info_t *dp_part, const char *dp, uint32_t size);
int vfs_install_unlink(partition_info_t *part, const char *path);

/* --- Streaming file reads (handle-based) for on-demand glyph PAGING -----------------
 * A held-open file handle so a caller can seek+read pieces of a large file (e.g. one
 * glyph bitmap at a time from a complete CJK font) instead of loading the whole thing.
 * HANDLE-BASED so independent users never clash: the feature/MP3 path and the font
 * pager each own a vfs_stream_t. (A single global handle would collide -- the MP3 holds
 * its .mp3 stream open while its playlist pages CJK glyphs through the same mechanism.) */
typedef struct { vfs_driver_t *drv; void *ctx; } vfs_stream_t;
/* Open `path` for streaming from LittleFS ONLY (fonts are LFS-authoritative; never the
 * slow SD). 0 on success, -1 if absent / not seekable. */
int  vfs_open_stream(vfs_stream_t *s, const char *path);
/* Open `path` on an already-resolved driver + partition (the feature path pins its
 * source partition this way; re-mounts before opening). 0 on success. */
int  vfs_stream_open_drv(vfs_stream_t *s, vfs_driver_t *drv, uint32_t addr, uint32_t size, const char *path);
int  vfs_stream_read(vfs_stream_t *s, void *buf, uint32_t n);   /* bytes read; <0 on error */
int  vfs_stream_seek(vfs_stream_t *s, uint32_t off);            /* 0 / <0 */
void vfs_stream_close(vfs_stream_t *s);                          /* idempotent */

#endif // VFS_H

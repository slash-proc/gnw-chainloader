#ifndef SYSTEM_FILEOPS_H
#define SYSTEM_FILEOPS_H

#include <stdint.h>
#include <stdbool.h>
#include "storage/vfs.h"

/*
 * Transient file-operations module interface.
 *
 * The in-core file browser keeps only read-only traversal + a basic SINGLE-file
 * copy (the shared in-core vfs_copy_open_file loop). Every heavy operation --
 * recursive folder copy (with its self-copy guard + free-space pre-flight),
 * recursive delete, and the tree-size walk -- lives in /modules/fileops.bin
 * (MOD_FLAG_TRANSIENT): the browser loads it on demand, runs one op, and reclaims
 * its pool slot (mod_pool_mark/reset), so this code is absent from the 40 KiB core
 * for the rest of the session. Same split as LFS-RO in-core vs LFS-RW as a module.
 *
 * The CORE owns the driver-load + mount/unmount lifecycle and hands the module
 * ALREADY-MOUNTED drivers; the module only runs the FS-tree algorithms. The RW
 * driver(s) a copy/delete writes through MUST be loaded RESIDENT *below* the
 * caller's mod_pool_mark (see menu_do_install) so the transient reclaim can't free
 * a driver the vfs still points at.
 */

/* Result codes: the module returns these; the core maps them to a translated
 * string (see fileops_result_msg in ui_file_browser.c). Shared so both agree. */
#define FILEOPS_OK         0
#define FILEOPS_CANCEL     1   /* user pressed B/PWR; no modal */
#define FILEOPS_OPEN_ERR   2
#define FILEOPS_READ_ERR   3
#define FILEOPS_WRITE_ERR  4
#define FILEOPS_DISK_FULL  5   /* ran out mid-write */
#define FILEOPS_TOO_DEEP   6   /* recursion past MAX depth */
#define FILEOPS_PATH_LONG  7   /* a joined path didn't fit */
#define FILEOPS_NO_SPACE   8   /* pre-flight: destination too small */
#define FILEOPS_COPY_SELF  9   /* folder paste into itself / a descendant */
#define FILEOPS_SRC_READ  10   /* pre-flight source walk failed */

/* Progress phases -- the module passes a phase (it can't translate); the core
 * picks the title string and formats the "File N" counter. */
#define FILEOPS_PHASE_COPY   0
#define FILEOPS_PHASE_CALC   1   /* counting the tree before a copy */
#define FILEOPS_PHASE_DELETE 2

/* Host callbacks the module needs. The module reaches the filesystems through the
 * already-mounted vfs_driver_t pointers it is handed (their opendir/readdir/mkdir/
 * unlink/statfs function pointers are callable directly from the module). */
typedef struct {
    uint32_t (*get_tick)(void);
    /* The shared in-core single-file copy loop (one 4 KiB buffer in the core). */
    int (*copy_open_file)(vfs_driver_t *src, const char *sp,
                          vfs_driver_t *dst, const char *dp,
                          uint32_t size_hint, vfs_copy_cb cb, void *user);
    /* Refresh input + latch a B/PWR cancel; returns non-zero once cancelled. Call
     * every iteration of a long op. */
    int  (*poll)(void);
    /* Throttled progress paint. phase = FILEOPS_PHASE_*; name = the current file
     * (may be NULL); count = running file number (used by the CALC phase). */
    void (*progress)(int pct, int phase, const char *name, int count);
} fileops_host_t;

typedef struct {
    /* Copy `sp` on `src` to `dp` on `dst`. is_dir => recursive tree copy with a
     * self-copy guard (when same_vol) + a free-space pre-flight; else a single
     * file. Both drivers are already mounted. Returns a FILEOPS_* code. */
    int (*copy)(const fileops_host_t *h, vfs_driver_t *src, const char *sp,
                vfs_driver_t *dst, const char *dp, int is_dir, uint32_t size,
                int same_vol);
    /* Delete `path` on `drv` (recursive when is_dir, single unlink otherwise).
     * Returns a FILEOPS_* code. */
    int (*del)(const fileops_host_t *h, vfs_driver_t *drv, const char *path, int is_dir);
} fileops_api_t;

/* loader.c: load /modules/fileops.bin transiently and run its
 * init_module(host, out_api). Returns false if missing/invalid. The CALLER owns
 * the pool slot (mod_pool_mark before, mod_pool_reset after). */
bool mod_load_fileops(const char *path, const fileops_host_t *host, fileops_api_t *out);

#endif /* SYSTEM_FILEOPS_H */

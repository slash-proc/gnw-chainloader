#ifndef CHAINLOADER_SYSTEM_LOADER_H
#define CHAINLOADER_SYSTEM_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include "storage/vfs.h"

/*
 * Generic position-independent module loader (decoupled from the filesystem
 * VFS). Reads a PIE module .bin via an in-core read-only FS driver, places it
 * at a bump-allocated RAM address, patches its R_ARM_RELATIVE table by that
 * address, zeroes .bss, and calls its init_module(drv, host).
 *
 * The module format is defined in system/module.h; the link layout in
 * src/modules/module.ld.
 */

/* Load the module at `path`, populating the caller-owned `out_drv` via the
 * module's init_module. `host` is the API vtable passed to the module.
 * Returns true on success. The module image stays resident (functions in
 * `out_drv` point into it) until its pool slot is reclaimed (see below). */
bool mod_load(const char *path, vfs_driver_t *out_drv, const host_api_t *host);

/* Transient-module slot reclaim. Capture the pool top with mod_pool_mark()
 * before loading a transient module (one that registers no permanent callbacks,
 * e.g. the installer), then mod_pool_reset(mark) once finished with it to free
 * the slot. Safe only when nothing resident was loaded above the mark. */
uint32_t mod_pool_mark(void);
void mod_pool_reset(uint32_t mark);

/* Why the most recent transient module load failed (0 = ok), so a failed launch can
 * surface an error to the user instead of silently doing nothing (see feature.c). */
enum { MOD_ERR_OK = 0, MOD_ERR_READ, MOD_ERR_ABI, MOD_ERR_NOMEM };
int mod_load_last_error(void);

/* Invoke fn(a0, a1) with r9 set to the live r9-pic module's GOT base (a no-op wrt r9 for an -fPIC
 * module). The core uses this for every entry into an r9-pic module: its init_module and its
 * feature_api.run. */
void mod_invoke_r9(const void *a0, const void *a1, void *fn);

/* Loader-filled module registry, read by the Module Overview view (ui/module_overview.c).
 * One entry per distinct loaded module (deduped by path, latest load wins). */
#define MOD_REC_MAX 16
typedef struct {
    uint32_t base;     /* pool slot address (the module's RAM) */
    uint32_t flash;    /* XIP .text mapped-flash address, 0 if full-copied into the pool */
    uint32_t ram;      /* bytes the module consumes in the pool */
    uint32_t flags;    /* module header flags (bit0 = transient) */
    char     path[44]; /* where it was loaded from */
    uint8_t  reason;   /* load decision: 1=XIP from FAT store; 2=RAM copy, NOT contiguous in FAT (came
                        * from LFS/SD); 3=RAM copy, mapped in FAT but XIP declined (not r9-pic / a flash
                        * relocation / no pool room). So: reason 1 = the XIP win, 3 = a XIP-app that fell
                        * back to RAM, 2 = a plain RAM app not in the store. */
    uint8_t  _pad[3];
} mod_rec_t;
extern mod_rec_t g_mod_recs[MOD_REC_MAX];
extern uint32_t  g_mod_recs_n;
extern uint32_t  g_pool_next;              /* current pool top (bump allocator) */
extern const uint32_t g_pool_lo, g_pool_hi; /* pool bounds */

#endif /* CHAINLOADER_SYSTEM_LOADER_H */

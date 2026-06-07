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

#endif /* CHAINLOADER_SYSTEM_LOADER_H */

#include "system/loader.h"
#include "system/module.h"
#include "../../common/abi.h"
#include "system/language.h"
#include "system/installer.h"
#include "storage/vfs.h"
#include "ui/theme.h"
#include <string.h>
#include "stm32h7xx.h"

/*
 * Module RAM pool (AXI-SRAM). A simple bump allocator hands out aligned slots to
 * PIE modules, which the loader relocates to the slot base — so the pool base is
 * free to choose (this superseded the old fixed per-module addresses). The app +
 * theme/assets live in the low part of AXI-SRAM; this 256 KiB pool sits above
 * them, leaving a ~260 KiB free gap below it (headroom for a larger lzma_heap).
 * Reset frees everything at once (modules are short-lived around a file op).
 */
#define MODULE_POOL_BASE   0x240C0000UL   /* 256 KiB pool to _estack (was 0x240A0000 / 384 KiB) */
#define MODULE_POOL_END    0x24100000UL   /* end of 1 MiB AXI-SRAM == _estack */
/* The app/UI stack descends from _estack into the top of this pool. Keep a
 * guard band clear so a module load can never climb into the live stack — if it
 * would, the load fails and the caller falls back gracefully. */
#define MODULE_STACK_GUARD 0x10000UL      /* 64 KiB */

static uint32_t g_pool_next = MODULE_POOL_BASE;

/* Transient-module support: the core captures the pool top before loading a
 * transient module (and any deps it needs), then resets to that mark once done,
 * reclaiming the slot. Safe only when nothing resident was loaded above the mark
 * (a transient module registers no permanent callbacks). Replaces the old
 * blanket mod_reset(), which was unsafe while any module's callbacks were live. */
uint32_t mod_pool_mark(void) {
    return g_pool_next;
}
void mod_pool_reset(uint32_t mark) {
    if (mark >= MODULE_POOL_BASE && mark <= g_pool_next) g_pool_next = mark;
}

/* Load + relocate the highest-version copy of the module at `path` into the pool
 * (the fetch scans every read-capable FS, SD first; see vfs_read_module), commit
 * the slot, and return its entry point (slot + entry_offset, Thumb bit set).
 * Returns NULL on any failure (read error, bad magic, no room) with the pool
 * pointer untouched. The module image stays resident (append-only allocator). */
static void *mod_load_image(const char *path) {
    uint8_t *slot = (uint8_t *)g_pool_next;
    uint32_t avail = MODULE_POOL_END - g_pool_next;
    if (avail < sizeof(module_header_t)) return NULL;

    uint32_t img_size = 0;
    if (vfs_read_module(path, slot, avail, &img_size) != 0) return NULL;
    if (img_size <= sizeof(module_header_t)) return NULL;

    module_header_t *h = (module_header_t *)slot;
    /* Reject a module built for a different firmware (magic + framework ABI). */
    if (!module_abi_ok((const uint8_t *)h, MODULE_MAGIC, MODULE_ABI_VERSION)) return NULL;

    /* Total RAM footprint includes the (NOLOAD) .bss past the image. */
    uint32_t footprint = h->bss_offset + h->bss_size;
    if (footprint > avail) return NULL;
    if (g_pool_next + footprint > MODULE_POOL_END - MODULE_STACK_GUARD) return NULL;

    /* Patch the address table by the load base. Entries in the header region
     * (its *_offset fields) are consumed as load-relative offsets below, so
     * skip them here to avoid double-relocating. */
    if (h->reloc_offset && h->reloc_count) {
        module_rel_t *rel = (module_rel_t *)(slot + h->reloc_offset);
        for (uint32_t i = 0; i < h->reloc_count; i++) {
            if ((rel[i].r_info & 0xFFu) != R_ARM_RELATIVE) continue;
            if (rel[i].r_offset < sizeof(module_header_t)) continue;
            *(uint32_t *)(slot + rel[i].r_offset) += (uint32_t)slot;
        }
    }

    /* Zero .bss (not present in the image). */
    if (h->bss_size) {
        memset(slot + h->bss_offset, 0, h->bss_size);
    }

    /* Make the freshly written code visible to the I-side. */
    SCB_CleanDCache_by_Addr((uint32_t *)slot, footprint);
    SCB_InvalidateICache();
    __DSB();
    __ISB();

    void *entry = slot + h->entry_offset;   /* Thumb bit set by the linker */
    /* Commit the slot (init can't fail — it returns void). */
    g_pool_next += (footprint + 31u) & ~31u;   /* 32-byte (cache-line) aligned */
    return entry;
}

bool mod_load(const char *path, vfs_driver_t *out_drv, const host_api_t *host) {
    if (!out_drv || !host) return false;
    void *entry = mod_load_image(path);
    if (!entry) return false;
    ((void (*)(vfs_driver_t *, const host_api_t *))entry)(out_drv, host);
    return true;
}

bool mod_load_theme(const char *path, const theme_host_api_t *host) {
    if (!host) return false;
    void *entry = mod_load_image(path);
    if (!entry) return false;
    ((void (*)(const theme_host_api_t *))entry)(host);
    return true;
}

bool mod_load_language(const char *path, const lang_host_t *host, lang_api_t *out_api) {
    if (!host || !out_api) return false;
    void *entry = mod_load_image(path);
    if (!entry) return false;
    ((void (*)(const lang_host_t *, lang_api_t *))entry)(host, out_api);
    return true;
}

bool mod_load_installer(const char *path, const installer_host_t *host, installer_api_t *out_api) {
    if (!host || !out_api) return false;
    void *entry = mod_load_image(path);   /* transient: caller brackets with mod_pool_mark/reset */
    if (!entry) return false;
    ((void (*)(const installer_host_t *, installer_api_t *))entry)(host, out_api);
    return true;
}

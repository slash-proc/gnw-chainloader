#include "system/loader.h"
#include "system/module.h"
#include "../../common/abi.h"
#include "system/language.h"
#include "system/installer.h"
#include "system/fileops.h"
#include "system/feature.h"
#include "storage/vfs.h"
#include "ui/theme.h"
#include <string.h>
#include "stm32h7xx.h"

/*
 * Module RAM pool (AXI-SRAM). A simple bump allocator hands out aligned slots to
 * PIE modules, which the loader relocates to the slot base — so the pool base is
 * free to choose (this superseded the old fixed per-module addresses). The app +
 * theme/assets live in the low part of AXI-SRAM (.bss ends ~0x2407EC54, ~507 KiB,
 * including the 49 KiB lzma_heap and the framebuffers); this pool sits above them.
 * Reset frees everything at once (modules are short-lived around a file op).
 */
#define MODULE_POOL_BASE   0x24090000UL   /* 384 KiB usable pool to the stack guard. Lowered from
                                           * 0x240C0000 (192 KiB) into the free AXI gap below it so the
                                           * ~104 KiB picture module fits alongside the resident modules;
                                           * ~69 KiB app/lzma headroom remains above .bss. */
#define MODULE_POOL_END    0x24100000UL   /* end of 1 MiB AXI-SRAM == _estack */
/* The app/UI stack descends from _estack into the top of this pool. Keep a
 * guard band clear so a module load can never climb into the live stack — if it
 * would, the load fails and the caller falls back gracefully. */
#define MODULE_STACK_GUARD 0x10000UL      /* 64 KiB */

uint32_t g_pool_next = MODULE_POOL_BASE;   /* non-static: scripts/debug/modstat.py reads it */

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

/* Why the last mod_load_image() failed (MOD_ERR_*, 0 = ok): the feature launcher surfaces
 * this so a failed transient load shows an error instead of silently doing nothing. */
static int g_mod_load_err;
int mod_load_last_error(void) { return g_mod_load_err; }

/* SWD-observable load-path counters (read by symbol from a debug script): how many module loads
 * executed in place from flash (XIP) vs were full-copied into the pool. Proves XIP actually
 * fires and shows the split at a glance. */
uint32_t g_mod_xip_loads = 0;
uint32_t g_mod_ram_loads = 0;

/* Module registry (mod_rec_t + MOD_REC_MAX live in loader.h, shared with the Module Overview UI):
 * one entry per distinct module, deduped by path. Lets the view list what is loaded, from where
 * (XIP flash address vs full RAM copy), its pool footprint, and whether it is still resident
 * ("active" = base+ram <= g_pool_next). g_pool_lo/hi expose the pool bounds. */
mod_rec_t g_mod_recs[MOD_REC_MAX];
uint32_t  g_mod_recs_n = 0;
const uint32_t g_pool_lo = MODULE_POOL_BASE;
const uint32_t g_pool_hi = MODULE_POOL_END;

static void mod_rec_add(const char *path, uint32_t base, uint32_t flash, uint32_t ram, uint32_t flags, uint8_t reason) {
    mod_rec_t *r = 0;
    for (uint32_t i = 0; i < g_mod_recs_n; i++)
        if (strncmp(g_mod_recs[i].path, path, sizeof(g_mod_recs[i].path)) == 0) { r = &g_mod_recs[i]; break; }
    if (!r) { if (g_mod_recs_n >= MOD_REC_MAX) return; r = &g_mod_recs[g_mod_recs_n++]; }
    r->base = base; r->flash = flash; r->ram = ram; r->flags = flags; r->reason = reason;
    uint32_t i = 0;
    for (; path[i] && i < sizeof(r->path) - 1; i++) r->path[i] = path[i];
    r->path[i] = 0;
}

/* r9 base for the last module load: data_base for an r9-pic (-msingle-pic-base) module, else 0. */
uint32_t g_mod_r9 = 0;

/* Call fn(a0, a1) with r9 = r9base (the module's GOT base), preserving the caller's r9. Naked so
 * nothing the compiler emits between the set and the call clobbers r9. The r9-pic module keeps r9
 * callee-saved through its run + host-API calls, so deeper core->module callbacks stay addressable;
 * only the core->module crossing itself needs r9 re-established. */
__attribute__((naked, noinline)) static void
call_feat_r9(const void *a0, const void *a1, void *fn, uint32_t r9base) {
    __asm__ volatile(
        "push {r9, lr}\n"   /* save caller's r9 + return addr */
        "mov  r9, r3\n"     /* r9 = r9base (4th arg) */
        "blx  r2\n"         /* fn(a0=r0, a1=r1) */
        "pop  {r9, pc}\n"   /* restore caller's r9 + return */
    );
}

/* Re-establish r9 for EVERY entry into the live r9-pic module (its init_module AND its
 * feature_api.run): control crossing from the core into module code must restore the module's GOT
 * base. g_mod_r9 == 0 (an -fPIC module, or none loaded) calls fn directly. */
void mod_invoke_r9(const void *a0, const void *a1, void *fn) {
    if (g_mod_r9) call_feat_r9(a0, a1, fn, g_mod_r9);
    else ((void (*)(const void *, const void *))fn)(a0, a1);
}

/* XIP gate. r9-pic modules (MOD_FLAG_R9_PIC) built -msingle-pic-base -mno-pic-data-is-text-relative
 * reach BOTH code and data via r9/GOT (no text-relative data access), required because XIP puts
 * .text in flash and .data/.bss in the RAM slot (non-contiguous). The loader relocates the GOT
 * two-base and re-establishes r9 at each core->module entry (mod_invoke_r9); the app is -ffixed-r9
 * so it never clobbers r9. -fPIC modules always full-copy. SWD-writable. */
bool g_xip_enabled = true;

/* XIP load: the module is a CONTIGUOUS file mapped at `flash_addr` (vfs_map_module verified the
 * contiguity). Execute .text/.rodata in place from flash and copy only .data/.got/.rel.dyn + .bss
 * into the pool -- which is the whole point: the pool no longer holds the (usually dominant) code.
 * Returns the entry point, or NULL to fall back to a full RAM copy (the NO_XIP flag, a relocation
 * that targets read-only flash, or no room). On NULL nothing is committed (g_pool_next untouched),
 * so the caller's full-copy reuses the slot. */
static void *mod_load_image_xip(const char *path, uint32_t flash_addr, uint32_t fsize) {
    const module_header_t *h = (const module_header_t *)flash_addr;   /* header XIP'd from flash */
    if (!module_abi_ok((const uint8_t *)h, MODULE_MAGIC, MODULE_ABI_VERSION)) return NULL;
    if (h->flags & MOD_FLAG_NO_XIP) return NULL;
    if (!(h->flags & MOD_FLAG_R9_PIC)) return NULL;   /* an -fPIC module's PC-relative GOT can't XIP */
    uint32_t T = h->data_offset;                    /* .text/.rodata end == .data start */
    if (T < sizeof(module_header_t) || T > fsize) return NULL;

    uint32_t data_img  = fsize - T;                 /* .data/.got/.rel.dyn bytes to copy to RAM */
    uint32_t footprint = data_img + h->bss_size;    /* + .bss (zeroed in RAM) */
    uint8_t *slot = (uint8_t *)g_pool_next;
    if (footprint > (MODULE_POOL_END - g_pool_next) ||
        g_pool_next + footprint > MODULE_POOL_END - MODULE_STACK_GUARD) return NULL;

    /* .text/.rodata stay in flash at flash_addr; copy only the writable tail into the slot. */
    memcpy(slot, (const void *)(flash_addr + T), data_img);

    /* Two bases: original offset 0 -> flash (text_base), offset T -> the slot (data_base).
     * ram_origin maps any original offset to its RAM address, valid only for offsets >= T
     * (everything below lives in flash, not RAM). */
    uint32_t text_base  = flash_addr;
    uint32_t data_base  = (uint32_t)slot;
    uint32_t ram_origin = data_base - T;

    if (h->reloc_offset && h->reloc_count) {
        const module_rel_t *rel = (const module_rel_t *)(flash_addr + h->reloc_offset);
        for (uint32_t i = 0; i < h->reloc_count; i++) {
            if ((rel[i].r_info & 0xFFu) != R_ARM_RELATIVE) continue;
            if (rel[i].r_offset < sizeof(module_header_t)) continue;  /* header offsets: consumed, not relocated */
            if (rel[i].r_offset < T) return NULL;     /* a relocation into read-only flash .text: can't XIP */
            uint32_t *p = (uint32_t *)(ram_origin + rel[i].r_offset);  /* = slot + (r_offset - T) */
            *p += (*p < T) ? text_base : (data_base - T);
        }
    }

    if (h->bss_size) memset((void *)(ram_origin + h->bss_offset), 0, h->bss_size);

    /* Make our .data/.got writes visible; .text is read-only flash (I-side coherent), but
     * invalidate I-cache defensively in case a reinstalled module reused this flash address. */
    SCB_CleanDCache_by_Addr((uint32_t *)slot, footprint);
    SCB_InvalidateICache();
    __DSB();
    __ISB();

    void *entry = (void *)(text_base + h->entry_offset);   /* entry in flash; Thumb bit set by the linker */
    g_pool_next += (footprint + 31u) & ~31u;
    g_mod_xip_loads++;
    g_mod_r9 = (h->flags & MOD_FLAG_R9_PIC) ? data_base : 0;   /* GOT at data_offset == data_base */
    mod_rec_add(path, (uint32_t)slot, flash_addr, footprint, h->flags, 1);   /* 1 = XIP in place from FAT */
    return entry;
}

/* Load + relocate the highest-version copy of the module at `path` into the pool
 * (the fetch scans every read-capable FS, SD first; see vfs_read_module), commit
 * the slot, and return its entry point (slot + entry_offset, Thumb bit set).
 * Returns NULL on any failure (read error, bad magic, no room) with the pool
 * pointer untouched. The module image stays resident (append-only allocator). */
static void *mod_load_image(const char *path) {
    g_mod_load_err = MOD_ERR_OK;
    g_mod_r9 = 0;

    /* XIP first: if the winning copy is a contiguous file in mapped flash, run .text in place and
     * keep only .data/.bss in the pool. Any decline (not in FAT, fragmented, NO_XIP, a flash
     * relocation, no room) falls through to the full RAM copy below. */
    uint8_t xip_reason = 2;   /* 2 = not contiguous in the FAT store -> RAM copy (from LFS/SD) */
    uint32_t xip_addr = 0, xip_size = 0;
    if (g_xip_enabled && vfs_map_module(path, &xip_addr, &xip_size) == 0) {
        void *e = mod_load_image_xip(path, xip_addr, xip_size);
        if (e) return e;
        xip_reason = 3;   /* 3 = mapped in FAT but XIP declined (not r9-pic / flash reloc / no room) -> RAM */
    }

    uint8_t *slot = (uint8_t *)g_pool_next;
    uint32_t avail = MODULE_POOL_END - g_pool_next;
    if (avail < sizeof(module_header_t)) { g_mod_load_err = MOD_ERR_NOMEM; return NULL; }

    uint32_t img_size = 0;
    if (vfs_read_module(path, slot, avail, &img_size) != 0) { g_mod_load_err = MOD_ERR_READ; return NULL; }
    if (img_size <= sizeof(module_header_t)) { g_mod_load_err = MOD_ERR_READ; return NULL; }

    module_header_t *h = (module_header_t *)slot;
    /* Reject a module built for a different firmware (magic + framework ABI). */
    if (!module_abi_ok((const uint8_t *)h, MODULE_MAGIC, MODULE_ABI_VERSION)) { g_mod_load_err = MOD_ERR_ABI; return NULL; }

    /* Total RAM footprint includes the (NOLOAD) .bss past the image. */
    uint32_t footprint = h->bss_offset + h->bss_size;
    if (footprint > avail) { g_mod_load_err = MOD_ERR_NOMEM; return NULL; }
    if (g_pool_next + footprint > MODULE_POOL_END - MODULE_STACK_GUARD) { g_mod_load_err = MOD_ERR_NOMEM; return NULL; }

    /* Relocate by load base, split into two bases so a future XIP build can keep
     * .text/.rodata in flash while .data/.bss live in RAM. A R_ARM_RELATIVE whose
     * target lies below data_offset points into .text/.rodata (text_base); at/above,
     * into .data/.got/.bss (data_base). Today both segments load contiguously into
     * the pool slot (text_base = slot, data_base = slot + data_offset), so this is
     * identical to the old single-base += slot; Stage 3c repoints text_base at the
     * module's mapped flash address. Header-region entries are consumed as
     * load-relative offsets below, so skip them to avoid double-relocating. */
    if (h->reloc_offset && h->reloc_count) {
        uint32_t T = h->data_offset;
        uint32_t text_base = (uint32_t)slot;
        uint32_t data_base = (uint32_t)slot + T;
        module_rel_t *rel = (module_rel_t *)(slot + h->reloc_offset);
        for (uint32_t i = 0; i < h->reloc_count; i++) {
            if ((rel[i].r_info & 0xFFu) != R_ARM_RELATIVE) continue;
            if (rel[i].r_offset < sizeof(module_header_t)) continue;
            uint32_t *p = (uint32_t *)(slot + rel[i].r_offset);
            *p += (*p < T) ? text_base : (data_base - T);
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
    g_mod_ram_loads++;
    g_mod_r9 = (h->flags & MOD_FLAG_R9_PIC) ? ((uint32_t)slot + h->data_offset) : 0;   /* GOT base */
    mod_rec_add(path, (uint32_t)slot, 0, footprint, h->flags, xip_reason);
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

bool mod_load_fileops(const char *path, const fileops_host_t *host, fileops_api_t *out_api) {
    if (!host || !out_api) return false;
    void *entry = mod_load_image(path);   /* transient: caller brackets with mod_pool_mark/reset */
    if (!entry) return false;
    ((void (*)(const fileops_host_t *, fileops_api_t *))entry)(host, out_api);
    return true;
}

bool mod_load_feature(const char *path, const feature_host_t *host, feature_api_t *out_api) {
    if (!host || !out_api) return false;
    void *entry = mod_load_image(path);   /* sets g_mod_r9 for an r9-pic module */
    if (!entry) return false;
    mod_invoke_r9(host, out_api, entry);   /* init_module(host, out_api), r9 set for an r9-pic module */
    return true;
}

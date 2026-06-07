#ifndef CHAINLOADER_SYSTEM_MODULE_H
#define CHAINLOADER_SYSTEM_MODULE_H

#include <stdint.h>

/*
 * PIE module format.
 *
 * Modules are compiled -fPIC (NOT -msingle-pic-base, so no r9 to maintain) and
 * linked -pie with src/modules/module.ld. The linker emits R_ARM_RELATIVE
 * entries into .rel.dyn for the GOT and any absolute pointers; the loader
 * patches those by the load address at load time, so a module runs at whatever
 * RAM address the loader places it.
 *
 * Layout of a module .bin (see module.ld):
 *   [module_header_t][.text/.rodata/.data/.got][.rel.dyn][.bss (not in image)]
 *
 * The header is at offset 0. Its *_offset fields are offsets from the module's
 * load base; the loader adds the load base itself (and therefore SKIPS any
 * relocation that targets the header region).
 */

#define MODULE_MAGIC 0x444F4D47UL   /* 'GMOD' little-endian */

/* Module-framework ABI: the contract between a PIE module and the loader + the
 * host-API structs it is handed. The core REJECTS any module whose header abi !=
 * MODULE_ABI_VERSION (a module built for a different firmware would call into a
 * changed contract). Bump this whenever that contract changes. One source of
 * truth: modules inherit this default; the test/dummy module overrides it with
 * -DMODULE_ABI_VERSION to prove rejection. */
#ifndef MODULE_ABI_VERSION
#define MODULE_ABI_VERSION 1u
#endif

/* module_header_t.flags bits (load model). */
#define MOD_FLAG_TRANSIENT 0x1u     /* load on demand, run, then reclaim the pool
                                     * slot; 0 = resident (kept in the pool). */

typedef struct {
    uint32_t magic;          /* MODULE_MAGIC */
    uint32_t entry_offset;   /* -> init_module(vfs_driver_t*, const host_api_t*) */
    uint32_t reloc_offset;   /* -> Elf32_Rel[] (.rel.dyn), kept in the image */
    uint32_t reloc_count;    /* number of Elf32_Rel entries */
    uint32_t bss_offset;     /* -> .bss (loader zeroes it; not present in image) */
    uint32_t bss_size;
    uint32_t version;        /* module version; higher wins when duplicates exist
                              * across filesystems. Missing/old modules read 0. */
    uint32_t abi;            /* module-framework ABI; must equal MODULE_ABI_VERSION
                              * or the core rejects the module. */
    uint32_t flags;          /* load-model bits (MOD_FLAG_TRANSIENT); 0 = resident. */
} module_header_t;

/* One ARM ELF32 REL entry: { r_offset, r_info }. R_ARM_RELATIVE == 23. */
typedef struct {
    uint32_t r_offset;
    uint32_t r_info;         /* type = r_info & 0xFF */
} module_rel_t;

#define R_ARM_RELATIVE 23

#ifdef MODULE_BUILD
/* Symbols defined by module.ld. Declared as arrays so taking the name yields
 * the symbol's value (entry: an offset / size). */
extern uint8_t _module_entry_offset[];
extern uint8_t _module_reloc_offset[];
extern uint8_t _module_reloc_count[];
extern uint8_t _module_bss_offset[];
extern uint8_t _module_bss_size[];

/* Each module declares its version via -DMODULE_VERSION=N at build time; modules
 * built before this field existed (or that omit the flag) report 0. */
#ifndef MODULE_VERSION
#define MODULE_VERSION 0
#endif

/* Load model: a module passes -DMODULE_FLAGS=MOD_FLAG_TRANSIENT to be loaded on
 * demand and freed after use; default is resident (kept in the pool). */
#ifndef MODULE_FLAGS
#define MODULE_FLAGS 0
#endif

/* Place at file scope in exactly one translation unit of each module. */
#define MODULE_HEADER \
    __attribute__((section(".module_header"), used)) \
    const module_header_t g_module_header = { \
        .magic        = MODULE_MAGIC, \
        .entry_offset = (uint32_t)_module_entry_offset, \
        .reloc_offset = (uint32_t)_module_reloc_offset, \
        .reloc_count  = (uint32_t)_module_reloc_count, \
        .bss_offset   = (uint32_t)_module_bss_offset, \
        .bss_size     = (uint32_t)_module_bss_size, \
        .version      = MODULE_VERSION, \
        .abi          = MODULE_ABI_VERSION, \
        .flags        = MODULE_FLAGS, \
    }
#endif /* MODULE_BUILD */

#endif /* CHAINLOADER_SYSTEM_MODULE_H */

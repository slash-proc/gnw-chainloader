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
#define MODULE_ABI_VERSION 4u   /* v4: widened file_ext 16->24 (fit "jpg,jpeg,png,bmp");
                                 * v3: two-base split-segment loader (header gains data_offset);
                                 * v2 widened file_ext to a comma-separated extension list */
#endif

/* module_header_t.flags bits (load model). */
#define MOD_FLAG_TRANSIENT 0x1u     /* load on demand, run, then reclaim the pool
                                     * slot; 0 = resident (kept in the pool). */
#define MOD_FLAG_NO_XIP    0x2u     /* keep in RAM; never execute-in-place from flash. Set on
                                     * latency-sensitive modules (e.g. the JPEG/MP3 inner loops)
                                     * where a flash fetch costs too much. 0 = XIP-able. */
#define MOD_FLAG_R9_PIC    0x4u     /* built -msingle-pic-base: addresses its GOT via r9, so the
                                     * loader sets r9 = data_base (the GOT sits at data_offset)
                                     * before entry. Required for XIP -- a non-contiguous .text/.got
                                     * defeats the default -fPIC PC-relative GOT. */

/* Feature-module menu placement, declared in the header so the core can discover a
 * module's menu entry by PEEKING the header (cheap, no code run, nothing loaded).
 * 0 = not a feature module. See system/feature.h + docs/module-menu-registration.md. */
#define MODULE_MENU_NONE     0u
#define MODULE_MENU_TOOLS    1u
#define MODULE_MENU_SETTINGS 2u
#define MODULE_MENU_XLAT_MAX 512u  /* bytes of packed per-language menu title in the header */

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
    uint32_t data_offset;    /* offset where .data/.got/.bss begin (end of .text/.rodata).
                              * The two-base loader relocates targets below this against the
                              * text base (flash/XIP) and at/above against the data base (RAM). */
    uint32_t flags;          /* load-model bits (MOD_FLAG_TRANSIENT); 0 = resident. */
    /* Feature-module manifest -- the core's feature discovery peeks which menu + label +
     * file type(s) a module provides WITHOUT loading it. Zeroed/empty on non-feature modules.
     * The loader and the installer gate read only magic/abi/version (the first 32 bytes), so
     * they are unaffected by this region; ONLY feature_discover reads it (via this struct). A
     * change to this region's LAYOUT therefore bumps MODULE_ABI_VERSION (v2 widened file_ext)
     * so an old-layout module is rejected rather than misread into a garbled manifest. */
    uint8_t  menu_id;        /* MODULE_MENU_NONE / _TOOLS / _SETTINGS */
    uint8_t  _pad[3];
    char     menu_label[24]; /* English label shown in the menu ("" = none) */
    char     file_ext[24];   /* comma-separated lowercase extensions this module handles, e.g.
                              * "jpg,jpeg,png,bmp" ("" = none); matched by ext_list_match() (utils.h) */
    /* Packed per-language menu title "code\0title\0...\0" (cooked from the module's i18n JSONs
     * by cook_modstrings), so the core localizes the menu entry at DISCOVERY without loading
     * the module. "" when the module has no translations -> the core shows menu_label. */
    char     menu_label_xlat[MODULE_MENU_XLAT_MAX];
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
extern uint8_t _module_data_offset[];

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

/* Feature-module manifest, set via -D at build time; default = not a feature module.
 * e.g. -DMODULE_MENU_ID=MODULE_MENU_TOOLS -DMODULE_MENU_LABEL='"MP3 Player"'
 *      -DMODULE_FILE_EXT='"mp3"' */
#ifndef MODULE_MENU_ID
#define MODULE_MENU_ID MODULE_MENU_NONE
#endif
#ifndef MODULE_MENU_LABEL
#define MODULE_MENU_LABEL ""
#endif
#ifndef MODULE_FILE_EXT
#define MODULE_FILE_EXT ""
#endif
/* Per-language menu title, normally #defined by the module's <module>_strings_gen.h
 * (emitted by cook_modstrings); "" if the module has no compiled-in translations. */
#ifndef MODULE_MENU_LABEL_XLAT
#define MODULE_MENU_LABEL_XLAT ""
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
        .data_offset  = (uint32_t)_module_data_offset, \
        .flags        = MODULE_FLAGS, \
        .menu_id      = MODULE_MENU_ID, \
        .menu_label   = MODULE_MENU_LABEL, \
        .file_ext     = MODULE_FILE_EXT, \
        .menu_label_xlat = MODULE_MENU_LABEL_XLAT, \
    }
#endif /* MODULE_BUILD */

#endif /* CHAINLOADER_SYSTEM_MODULE_H */

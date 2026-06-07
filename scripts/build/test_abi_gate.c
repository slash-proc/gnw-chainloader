/*
 * Host unit test for the ABI gate (src/common/abi.h) — the exact helpers the
 * firmware calls in system/loader.c and storage/vfs.c, so this tests the real
 * check, not a copy. A dynamically loaded artifact is honored only when BOTH its
 * magic and its ABI match the running firmware; a mismatch in EITHER direction
 * (newer or older) means it was built for a different firmware and must be
 * rejected. Builds + runs under `make test_host`; no device needed.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/abi.h"        /* module_abi_ok / pack_abi_ok (via -Isrc) */
#include "system/module.h"     /* MODULE_MAGIC, MODULE_ABI_VERSION */
#include "strings.h"           /* STRINGS_ABI_VERSION (via -Isrc/chainloader/ui) */

#define LANG_PACK_MAGIC 0x32474E4Cu   /* 'LNG2', mirrors storage/vfs.c */

static int g_fails = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fails++;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

int main(void) {
    /* ---- PIE module gate (GMOD magic @0, framework abi @28) ---- */
    uint8_t m[40];
    memset(m, 0, sizeof(m));
    put32(m + 0,  MODULE_MAGIC);
    put32(m + 28, MODULE_ABI_VERSION);
    check(module_abi_ok(m, MODULE_MAGIC, MODULE_ABI_VERSION),
          "module: matching magic + abi accepted");

    put32(m + 28, MODULE_ABI_VERSION + 1u);
    check(!module_abi_ok(m, MODULE_MAGIC, MODULE_ABI_VERSION),
          "module: abi+1 (newer) rejected");

    put32(m + 28, MODULE_ABI_VERSION - 1u);
    check(!module_abi_ok(m, MODULE_MAGIC, MODULE_ABI_VERSION),
          "module: abi-1 (older) rejected");

    put32(m + 28, MODULE_ABI_VERSION);   /* abi good again, break the magic */
    put32(m + 0,  MODULE_MAGIC ^ 0xFFu);
    check(!module_abi_ok(m, MODULE_MAGIC, MODULE_ABI_VERSION),
          "module: bad magic rejected (even with matching abi)");

    /* ---- .lang pack gate (LNG2 magic @0, strings abi @4, u16) ---- */
    uint8_t p[16];
    memset(p, 0, sizeof(p));
    put32(p + 0, LANG_PACK_MAGIC);
    put16(p + 4, (uint16_t)STRINGS_ABI_VERSION);
    check(pack_abi_ok(p, LANG_PACK_MAGIC, (uint16_t)STRINGS_ABI_VERSION),
          "pack: matching magic + abi accepted");

    put16(p + 4, (uint16_t)(STRINGS_ABI_VERSION + 1));
    check(!pack_abi_ok(p, LANG_PACK_MAGIC, (uint16_t)STRINGS_ABI_VERSION),
          "pack: abi+1 (newer) rejected");

    put16(p + 4, (uint16_t)(STRINGS_ABI_VERSION - 1));
    check(!pack_abi_ok(p, LANG_PACK_MAGIC, (uint16_t)STRINGS_ABI_VERSION),
          "pack: abi-1 (older) rejected");

    put16(p + 4, (uint16_t)STRINGS_ABI_VERSION);   /* abi good again, break the magic */
    put32(p + 0, LANG_PACK_MAGIC ^ 0xFFu);
    check(!pack_abi_ok(p, LANG_PACK_MAGIC, (uint16_t)STRINGS_ABI_VERSION),
          "pack: bad magic rejected (even with matching abi)");

    printf(g_fails ? "\nABI GATE: FAILED (%d)\n" : "\nABI GATE: OK\n", g_fails);
    return g_fails ? 1 : 0;
}

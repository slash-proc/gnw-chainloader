#ifndef COMMON_ABI_H
#define COMMON_ABI_H

/*
 * Shared ABI-gate helpers. A dynamically loaded artifact (a PIE module or a
 * .lang language pack) is honored only when BOTH its magic and its ABI version
 * match what the running firmware expects; a mismatch in either direction means
 * it was built for a different firmware and must be rejected (don't load
 * something built for a different firmware). The firmware gates
 * (system/loader.c, storage/vfs.c) and the host unit test
 * (scripts/build/test_abi_gate.c) both call these, so the test exercises the
 * real check rather than a copy.
 *
 * The helpers read the on-wire header bytes (little-endian) so the same code
 * works against an in-RAM module image, a memory-mapped pack, or a host-built
 * test buffer.
 */
#include <stdint.h>
#include <stdbool.h>

static inline uint32_t abi_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* PIE module image (module_header_t): GMOD magic at byte 0, framework abi at
 * byte 28 (after magic,entry,reloc_off,reloc_cnt,bss_off,bss_sz,version). */
static inline bool module_abi_ok(const uint8_t *hdr, uint32_t want_magic, uint32_t want_abi) {
    return abi_rd32(hdr) == want_magic && abi_rd32(hdr + 28) == want_abi;
}

/* .lang pack (LNG2 header): magic at byte 0 (u32), strings abi at byte 4 (u16). */
static inline bool pack_abi_ok(const uint8_t *hdr, uint32_t want_magic, uint16_t want_abi) {
    uint16_t abi = (uint16_t)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8));
    return abi_rd32(hdr) == want_magic && abi == want_abi;
}

#endif /* COMMON_ABI_H */

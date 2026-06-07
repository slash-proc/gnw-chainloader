#ifndef COMMON_STUB_SERVICES_H
#define COMMON_STUB_SERVICES_H

#include <stdint.h>
#include "LzmaDec.h"   /* SRes, Byte, SizeT, ELzmaFinishMode, ELzmaStatus, ISzAllocPtr */

/*
 * The stub (flash @ 0x08000000) decompresses the RAM app at boot with its own
 * LzmaDecode, then sits dead in flash. We reclaim that decoder: the stub
 * publishes a tiny services table at a FIXED flash address (right after its
 * 64-byte vector table) so the RAM app — and modules — reuse the SAME LZMA
 * decoder instead of carrying a second ~2 KB copy. The magic word makes a
 * stale/relocated table fail safe: a caller that doesn't see it simply skips
 * (no jump into garbage). The stub's own boot decode is unchanged; this is an
 * additional read-only export. Layout is enforced by an ASSERT in the stub
 * linker script, so STUB_SERVICES_ADDR and the section can't silently drift.
 */
#define STUB_SERVICES_ADDR  0x08000040UL
#define STUB_SERVICES_MAGIC 0x53565453UL   /* 'STSV' */

typedef SRes (*stub_lzma_decode_fn)(
    Byte *dest, SizeT *destLen, const Byte *src, SizeT *srcLen,
    const Byte *propData, unsigned propSize, ELzmaFinishMode finishMode,
    ELzmaStatus *status, ISzAllocPtr alloc);

typedef struct {
    uint32_t magic;
    stub_lzma_decode_fn lzma_decode;
} stub_services_t;

#define STUB_SERVICES ((const stub_services_t *)STUB_SERVICES_ADDR)

#endif /* COMMON_STUB_SERVICES_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The flash stage's relocator: apply the image's self-relocation fixup table
 * to the RAM-top shadow.  Runs in place from the flash window, before the
 * image is fixed up, so it must not name any absolute address of its own:
 * no globals, no string literals, no calls -- every pointer here derives
 * from its arguments.  fw-fixups.py builds the table (and proves it) at
 * build time; its layout is the one the machine's loader used to apply.
 */

#include "fw-base.h"

#define FW_FIXUP_FOOTER_MAGIC 0x5055584634364149ULL   /* "IA64FXUP" */
#define FW_FIXUP_MAGIC        0x50555846u             /* "FXUP" */

static UINT64 fw_ld8(const UINT8 *p)
{
    UINT64 v = 0;
    UINTN i;

    for (i = 0; i < 8; i++) {
        v |= (UINT64)p[i] << (i * 8);
    }
    return v;
}

static UINT32 fw_ld4(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}

static void fw_st8(UINT8 *p, UINT64 v)
{
    UINTN i;

    for (i = 0; i < 8; i++) {
        p[i] = (UINT8)(v >> (i * 8));
    }
}

static void fw_st4(UINT8 *p, UINT32 v)
{
    UINTN i;

    for (i = 0; i < 4; i++) {
        p[i] = (UINT8)(v >> (i * 8));
    }
}

/* The movl imm64 of an MLX bundle (SDM vol. 3, X2 format), read and written. */
static UINT64 fw_bundle_imm64_get(const UINT8 *bundle)
{
    UINT64 lo = fw_ld8(bundle);
    UINT64 hi = fw_ld8(bundle + 8);
    UINT64 slot1 = ((lo >> 46) | (hi << 18)) & ((1ULL << 41) - 1);
    UINT64 slot2 = (hi >> 23) & ((1ULL << 41) - 1);
    UINT64 imm7b = (slot2 >> 6) & 0x7f;
    UINT64 imm9d = (slot2 >> 27) & 0x1ff;
    UINT64 imm5c = (slot2 >> 22) & 0x1f;
    UINT64 ic = (slot2 >> 21) & 0x1;
    UINT64 i = (slot2 >> 36) & 0x1;

    return (i << 63) | (slot1 << 22) | (ic << 21) | (imm5c << 16) |
           (imm9d << 7) | imm7b;
}

static void fw_bundle_imm64_set(UINT8 *bundle, UINT64 imm64)
{
    UINT64 lo = fw_ld8(bundle);
    UINT64 hi = fw_ld8(bundle + 8);
    UINT64 slot2 = (hi >> 23) & ((1ULL << 41) - 1);
    UINT64 slot1 = (imm64 >> 22) & ((1ULL << 41) - 1);

    slot2 &= ~((1ULL << 36) | (0x1ffULL << 27) | (0x1fULL << 22) |
               (1ULL << 21) | (0x7fULL << 6));
    slot2 |= (((imm64 >> 63) & 1) << 36) | (((imm64 >> 7) & 0x1ff) << 27) |
             (((imm64 >> 16) & 0x1f) << 22) | (((imm64 >> 21) & 1) << 21) |
             ((imm64 & 0x7f) << 6);
    lo = (lo & ((1ULL << 46) - 1)) | (slot1 << 46);
    hi = (slot1 >> 18) | (slot2 << 23);
    fw_st8(bundle, lo);
    fw_st8(bundle + 8, hi);
}

/*
 * @shadow is the copied image body (its last 16 bytes the fixup footer),
 * @delta = shadow base - link base.  Returns the number of sites patched,
 * or 0 when the table is missing or malformed -- a shadow that then runs
 * with link-base addresses faults at once, which is the better failure.
 */
UINT64 fw_flash_relocate(UINT8 *shadow, UINT64 delta, UINT64 body_size)
{
    const UINT8 *table;
    UINT64 fixups_off;
    UINT32 n64, n32, nimm, i;
    UINT64 sites = 0;

    if (body_size < 32 ||
        fw_ld8(shadow + body_size - 16) != FW_FIXUP_FOOTER_MAGIC) {
        return 0;
    }
    fixups_off = fw_ld8(shadow + body_size - 8);
    if (fixups_off > body_size - 32) {
        return 0;
    }
    table = shadow + fixups_off;
    if (fw_ld4(table) != FW_FIXUP_MAGIC || fw_ld4(table + 4) != 1) {
        return 0;
    }
    n64 = fw_ld4(table + 8);
    n32 = fw_ld4(table + 12);
    nimm = fw_ld4(table + 16);
    if (fixups_off + 32 + ((UINT64)n64 + n32 + nimm) * 8 > body_size) {
        return 0;
    }
    table += 32;
    for (i = 0; i < n64; i++, table += 8) {
        UINT64 off = fw_ld8(table);

        if (off <= body_size - 8) {
            fw_st8(shadow + off, fw_ld8(shadow + off) + delta);
            sites++;
        }
    }
    for (i = 0; i < n32; i++, table += 8) {
        UINT64 off = fw_ld8(table);

        if (off <= body_size - 4) {
            fw_st4(shadow + off, fw_ld4(shadow + off) + (UINT32)delta);
            sites++;
        }
    }
    for (i = 0; i < nimm; i++, table += 8) {
        UINT64 off = fw_ld8(table);

        if (off <= body_size - 16) {
            fw_bundle_imm64_set(shadow + off,
                                fw_bundle_imm64_get(shadow + off) + delta);
            sites++;
        }
    }
    return sites;
}

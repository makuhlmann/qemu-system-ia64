/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ATI Radeon R100: where a pixel of a tiled surface sits in memory.
 *
 * AMD R5xx Acceleration guide v1.5, chapters 2 and 4: a micro tile is 32
 * consecutive bytes, a macro tile 2 KiB of 8x8 micro tiles, and the R100's
 * layouts are the same (Mesa's R100-R500 span code below uses one set of
 * formulas).  The reorder of micro tiles inside a macro tile is Mesa 7.11
 * radeon_span.c radeon_ptr_4byte and radeon_ptr_2byte_8x2; the depth buffer,
 * which the R100 always tiles, is XFree86 4.3 radeon_span.c radeon_mba_z16
 * and radeon_mba_z32.
 */

#include "qemu/osdep.h"
#include "ati_int.h"

/*
 * Colour and texture surfaces, in byte coordinates: 32-bit micro tiles are
 * 4x2 pixels and 16-bit ones 8x2, both 16 bytes by 2 lines, so the two Mesa
 * functions agree once x is counted in bytes.  radeon_ptr_2byte_8x2's lowest
 * terms, (x & 15) << 2 and (x & 3) << 2, would overlap the next field or drop
 * x bit 2; the guide's 128x8 and 64x16 macro tiles need x & 31 and x & 15 in
 * bytes, as here.  8-bit micro tiles are 8x4 and have no known reorder.
 */
bool ati_r100_tile_offset(unsigned int tile, unsigned int cpp, uint32_t pitch,
                          uint32_t xbyte, uint32_t y, uint64_t *offset)
{
    uint64_t a;

    if (cpp != 1 && cpp != 2 && cpp != 4) {
        return false;
    }
    switch (tile & ATI_R100_TILE_BOTH) {
    case ATI_R100_TILE_LINEAR:
        a = (uint64_t)y * pitch + xbyte;
        break;
    case ATI_R100_TILE_MICRO:
        if (cpp == 1) {
            return false;
        }
        a = (uint64_t)(y >> 1) * pitch * 2 + (uint64_t)(xbyte >> 4) * 32 +
            (y & 1) * 16 + (xbyte & 15);
        break;
    case ATI_R100_TILE_MACRO:
        a = ((uint64_t)(y >> 3) * (pitch >> 8) + (xbyte >> 8)) << 11;
        a |= (((y >> 2) ^ (xbyte >> 8)) & 1U) << 10;
        a |= (((y >> 3) ^ (xbyte >> 7)) & 1U) << 9;
        a |= (((y >> 1) ^ (xbyte >> 7)) & 1U) << 8;
        a |= (((y >> 2) ^ (xbyte >> 6)) & 1U) << 7;
        a |= (y & 1U) << 6;
        a |= xbyte & 63;
        break;
    default:
        if (cpp == 1) {
            return false;
        }
        a = ((uint64_t)(y >> 4) * (pitch >> 7) + (xbyte >> 7)) << 11;
        a |= (((y >> 3) ^ (xbyte >> 7)) & 1U) << 10;
        a |= (((y >> 4) ^ (xbyte >> 6)) & 1U) << 9;
        a |= (((y >> 2) ^ (xbyte >> 6)) & 1U) << 8;
        a |= (((y >> 3) ^ (xbyte >> 5)) & 1U) << 7;
        a |= ((y >> 1) & 1U) << 6;
        a |= ((xbyte >> 4) & 1U) << 5;
        a |= (y & 1U) << 4;
        a |= xbyte & 15;
        break;
    }
    *offset = a;
    return true;
}

/* The depth buffer; pitch in pixels, as RB3D_DEPTHPITCH holds it. */
uint64_t ati_r100_depth_offset(unsigned int cpp, uint32_t pitch,
                               uint32_t x, uint32_t y)
{
    uint64_t ba;
    uint64_t a;

    if (cpp == 2) {
        ba = (uint64_t)(y / 16) * (pitch / 32) + x / 32;
        a = (x & 7) << 1;
        a |= (y & 7) << 4;
        a |= (x & 8) << 4;
        a |= (ba & 3) << 8;
        a |= (y & 8) << 7;
        a |= ((x & 0x10) ^ (y & 0x10)) << 7;
    } else {
        ba = (uint64_t)(y / 16) * (pitch / 16) + x / 16;
        a = (x & 7) << 2;
        a |= (y & 3) << 5;
        a |= (((x & 0x10) >> 2) ^ (y & 4)) << 5;
        a |= (ba & 3) << 8;
        a |= (y & 8) << 7;
        a |= (((x & 8) << 1) ^ (y & 0x10)) << 7;
    }
    return a | (ba & ~3ULL) << 10;
}

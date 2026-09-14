#ifndef TOM_THUMB_3X5_H
#define TOM_THUMB_3X5_H

#include <stdint.h>

/*
 * Compact subset of Tom Thumb by Robey Pointer: a 3x5-pixel bitmap font.
 * Source: https://opengameart.org/content/tom-thumb-tiny-ascii-font-3x5
 * License: CC0 1.0 Universal (public-domain dedication).
 *
 * Each uint16_t stores five 3-bit rows, top row first, in bits 14..0.
 * Glyph order is given by TOM_THUMB_3X5_CHARS. All data is read-only.
 */
#define TOM_THUMB_3X5_CHARS " !+-./0123456789:?ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define TOM_THUMB_3X5_COUNT 44u

static const uint16_t tom_thumb_3x5[TOM_THUMB_3X5_COUNT] = {
    0x0000, 0x4904, 0x2E80, 0x7000, 0x4000, 0x12A4, 0x3B6E, 0x2C92,
    0x62A7, 0x628E, 0x5BC9, 0x798E, 0x39EF, 0x72A4, 0x7BEF, 0x7BCE,
    0x4100, 0x7282, 0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79E7, 0x79E4,
    0x39EB, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED, 0x5FFD,
    0x2B6A, 0x6BA4, 0x2B7B, 0x6BF5, 0x388E, 0x7492, 0x5B6B, 0x5B52,
    0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
};

_Static_assert(sizeof tom_thumb_3x5 == 88, "unexpected Tom Thumb subset size");

#endif

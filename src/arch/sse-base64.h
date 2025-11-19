/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * x86 SSSE3 optimized base64 encoding
 *
 * Copyright (C) 2016-2023 powturbo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Based on Turbo-Base64 by powturbo:
 * https://github.com/powturbo/Turbo-Base64
 *
 * Core algorithm from turbob64_.h:
 * - bitmap128v8_6: Translates 6-bit indices to base64 characters
 * - bitunpack128v8_6: Unpacks 12 input bytes to 16 x 6-bit indices
 * - Uses SSSE3 pshufb for efficient table lookup and bit manipulation
 */

#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)

#include <emmintrin.h> /* SSE2 */
#include <stddef.h>
#include <stdint.h>

#ifdef __SSSE3__
#include <tmmintrin.h> /* SSSE3 for _mm_shuffle_epi8 */

/* bitmap128v8_6: Map 6-bit indices to base64 ASCII characters
 *
 * Algorithm:
 * 1. Saturating subtraction: vidx = v - 51 (with saturation to 0)
 * 2. Range adjustment: vidx -= (v > 25) ? 1 : 0
 * 3. Lookup offset from 16-byte LUT using pshufb
 * 4. Add offset to original value to get final character
 *
 * LUT mappings:
 *   offsets[0-1]  = 65, 71   (uppercase A-Z, lowercase a-z)
 *   offsets[2-11] = -4       (digits 0-9)
 *   offsets[12]   = -19      ('+')
 *   offsets[13]   = -16      ('/')
 */
static inline __m128i bitmap128v8_6(const __m128i v)
{
    const __m128i offsets =
        _mm_set_epi8(0, 0, -16, -19, /* Special chars: '/', '+' */
                     -4, -4, -4, -4, /* Digits '0'-'9' */
                     -4, -4, -4, -4, -4, -4, 71,
                     65 /* Lowercase 'a'-'z', Uppercase 'A'-'Z' */
        );

    /* Step 1: Saturating subtraction to compute LUT index
     * - For v in [0, 25]:  vidx = 0 (saturates)
     * - For v in [26, 51]: vidx = v - 51 (negative, wraps to high values)
     * - For v >= 52:       vidx = v - 51
     */
    __m128i vidx = _mm_subs_epu8(v, _mm_set1_epi8(51));

    /* Step 2: Refine index based on range [0, 25]
     * If v > 25, subtract 1 from vidx (adjusts for lowercase range)
     * _mm_cmpgt_epi8 returns 0xFF if v > 25, else 0x00
     */
    vidx = _mm_sub_epi8(vidx, _mm_cmpgt_epi8(v, _mm_set1_epi8(25)));

    /* Step 3: Lookup offset and add to original value */
    return _mm_add_epi8(v, _mm_shuffle_epi8(offsets, vidx));
}

/* bitunpack128v8_6: Unpack 12 input bytes to 16 x 6-bit indices
 *
 * Input format (after shuffle): 12 bytes packed as 4 x 24-bit groups
 * Output: 16 bytes, each containing a 6-bit value (0-63)
 *
 * Uses multiply-high and multiply-low to extract 6-bit fields:
 * - mulhi extracts bits [14:8] and [30:24] (indices 1, 3)
 * - mullo extracts bits [5:0] and [21:16] (indices 0, 2)
 */
static inline __m128i bitunpack128v8_6(__m128i v)
{
    __m128i va = _mm_mulhi_epu16(_mm_and_si128(v, _mm_set1_epi32(0x0fc0fc00)),
                                 _mm_set1_epi32(0x04000040));
    __m128i vb = _mm_mullo_epi16(_mm_and_si128(v, _mm_set1_epi32(0x003f03f0)),
                                 _mm_set1_epi32(0x01000010));
    return _mm_or_si128(va, vb);
}

/* Base64 encoding using SSSE3 SIMD optimization
 *
 * Processes 12 input bytes -> 16 base64 characters per iteration
 */
static inline size_t base64_encode_sse(const uint8_t *restrict in,
                                       size_t inlen,
                                       uint8_t *restrict out)
{
    /* Base64 lookup table for scalar fallback */
    static const char base64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const uint8_t *ip = in;
    uint8_t *op = out;

    /* Shuffle mask from Turbo-Base64
     * Reorders 12 input bytes for bitunpack128v8_6
     * Creates overlapping 4-byte groups for 6-bit extraction
     */
    const __m128i shuf = _mm_set_epi8(10, 11, 9, 10, /* Fourth group */
                                      7, 8, 6, 7,    /* Third group */
                                      4, 5, 3, 4,    /* Second group */
                                      1, 2, 0, 1     /* First group */
    );

    /* Use non-temporal stores for large buffers (>1MB) to reduce cache
     * pollution. For typical DOOM frames (~256KB), regular stores are more
     * efficient. _mm_stream_si128 requires 16-byte alignment; check at runtime.
     */
    const bool use_streaming = (inlen > 1024 * 1024);

    /* Main loop: process 12 bytes -> 16 base64 chars */
    while (ip + 12 <= in + inlen) {
        /* Load 12 bytes */
        __m128i v = _mm_loadu_si128((const __m128i *) ip);

        /* Reshuffle for optimal bit extraction (Turbo-Base64 algorithm) */
        v = _mm_shuffle_epi8(v, shuf);

        /* Extract 16 x 6-bit indices */
        v = bitunpack128v8_6(v);

        /* Translate 6-bit indices to base64 characters */
        v = bitmap128v8_6(v);

        /* Store 16 base64 characters
         * Use streaming store only if buffer is large AND pointer is aligned
         */
        if (use_streaming && (((uintptr_t) op & 15) == 0)) {
            _mm_stream_si128((__m128i *) op, v);
        } else {
            _mm_storeu_si128((__m128i *) op, v);
        }
        op += 16;
        ip += 12;
    }

    /* Ensure all non-temporal stores are visible before continuing */
    if (use_streaming)
        _mm_sfence();

    /* Fallback to scalar for remainder */
    while (ip + 2 < in + inlen) {
        uint32_t val = ((uint32_t) ip[0] << 16) | ((uint32_t) ip[1] << 8) |
                       ((uint32_t) ip[2]);

        *op++ = base64_table[(val >> 18) & 0x3f];
        *op++ = base64_table[(val >> 12) & 0x3f];
        *op++ = base64_table[(val >> 6) & 0x3f];
        *op++ = base64_table[val & 0x3f];

        ip += 3;
    }

    /* Handle padding */
    if (ip < in + inlen) {
        uint32_t val = (uint32_t) ip[0] << 16;
        if (ip + 1 < in + inlen)
            val |= (uint32_t) ip[1] << 8;

        *op++ = base64_table[(val >> 18) & 0x3f];
        *op++ = base64_table[(val >> 12) & 0x3f];

        if (ip + 1 < in + inlen) {
            *op++ = base64_table[(val >> 6) & 0x3f];
        } else {
            *op++ = '=';
        }
        *op++ = '=';
    }

    return op - out;
}

#else /* !__SSSE3__ */

/* SSE2 without SSSE3: fall back to scalar implementation */
static inline size_t base64_encode_sse(const uint8_t *restrict in,
                                       size_t inlen,
                                       uint8_t *restrict out)
{
    extern size_t base64_encode_scalar(const uint8_t *restrict input,
                                       size_t input_len,
                                       uint8_t *restrict output);
    return base64_encode_scalar(in, inlen, out);
}

#endif /* __SSSE3__ */

#endif /* __x86_64__ || _M_X64 || __i386__ || _M_IX86 */

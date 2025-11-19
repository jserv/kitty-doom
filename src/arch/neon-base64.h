/* SPDX-License-Identifier: MIT */
/*
 * ARM NEON optimized base64 encoding
 *
 * Copyright (c) 2025 National Cheng Kung University, Taiwan
 * Copyright (c) 2021 The simdutf authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Original algorithm by Wojciech Muła and Daniel Lemire
 *
 * References:
 * - "Base64 encoding and decoding at almost the speed of a memory copy"
 *   Wojciech Muła, Daniel Lemire (2020)
 *   https://arxiv.org/abs/1910.05109
 *
 * Key optimizations:
 * - vsliq_n_u8 for efficient bit merging (single instruction)
 * - vqtbl4q_u8 for single-instruction table lookup
 * - Interleaved 4x16 table organization
 */

#pragma once

#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

/* Base64 encoding using NEON SIMD optimization
 *
 * Processes 48 input bytes -> 64 base64 characters per iteration
 * Uses vqtbl4q_u8 for table lookup and vsliq_n_u8 for bit merging
 */
static inline size_t base64_encode_neon(const uint8_t *restrict input,
                                        size_t input_len,
                                        uint8_t *restrict output)
{
    /* Base64 table organized for vqtbl4q_u8 (4x16 interleaved)
     * Credit: Wojciech Muła */
    static const uint8_t source_table[64] = {
        'A', 'Q', 'g', 'w', 'B', 'R', 'h', 'x', 'C', 'S', 'i', 'y', 'D',
        'T', 'j', 'z', 'E', 'U', 'k', '0', 'F', 'V', 'l', '1', 'G', 'W',
        'm', '2', 'H', 'X', 'n', '3', 'I', 'Y', 'o', '4', 'J', 'Z', 'p',
        '5', 'K', 'a', 'q', '6', 'L', 'b', 'r', '7', 'M', 'c', 's', '8',
        'N', 'd', 't', '9', 'O', 'e', 'u', '+', 'P', 'f', 'v', '/',
    };

    const uint8x16x4_t table = vld4q_u8(source_table);
    const uint8x16_t v3f = vdupq_n_u8(0x3f);

    size_t i = 0;
    size_t output_len = 0;

    /* Use non-temporal stores for large buffers (>1MB) to reduce cache
     * pollution. For typical DOOM frames (~256KB), regular stores are more
     * efficient. STNP is part of ARMv8.0-A base instruction set.
     */
#if defined(__aarch64__)
    const bool use_streaming = (input_len > 1024 * 1024);
#endif

    /* Main loop: process 48 bytes -> 64 base64 chars (16x parallelism) */
    for (; i + 48 <= input_len; i += 48) {
        /* Load 48 bytes as 3x16 interleaved vectors */
        const uint8x16x3_t in = vld3q_u8(input + i);

        /* Extract 4 x 6-bit fields using vsliq_n_u8 (shift left and insert)
         *
         * in.val[0]: aaaa aaaa
         * in.val[1]: bbbb bbbb
         * in.val[2]: cccc cccc
         *
         * result[0]: 00aa aaaa (bits 7-2 of byte 0)
         * result[1]: 00aa bbbb (bits 1-0 of byte 0, bits 7-4 of byte 1)
         * result[2]: 00bb bbcc (bits 3-0 of byte 1, bits 7-6 of byte 2)
         * result[3]: 00cc cccc (bits 5-0 of byte 2)
         */
        uint8x16x4_t result;

        /* Field 0: Top 6 bits of byte 0 */
        result.val[0] = vshrq_n_u8(in.val[0], 2);

        /* Field 1: Bottom 2 bits of byte 0 + top 4 bits of byte 1
         * vsliq_n_u8(a, b, n): (a & ~(0xff << n)) | (b << n) */
        result.val[1] =
            vandq_u8(vsliq_n_u8(vshrq_n_u8(in.val[1], 4), in.val[0], 4), v3f);

        /* Field 2: Bottom 4 bits of byte 1 + top 2 bits of byte 2 */
        result.val[2] =
            vandq_u8(vsliq_n_u8(vshrq_n_u8(in.val[2], 6), in.val[1], 2), v3f);

        /* Field 3: Bottom 6 bits of byte 2 */
        result.val[3] = vandq_u8(in.val[2], v3f);

        /* Convert 6-bit indices to base64 characters using table lookup */
        result.val[0] = vqtbl4q_u8(table, result.val[0]);
        result.val[1] = vqtbl4q_u8(table, result.val[1]);
        result.val[2] = vqtbl4q_u8(table, result.val[2]);
        result.val[3] = vqtbl4q_u8(table, result.val[3]);

        /* Store 64 bytes as 4x16 interleaved vectors
         * AArch64: STNP for large buffers, vst4q_u8 for small/typical workloads
         * ARMv7: Always uses vst4q_u8 (STNP not available on 32-bit ARM)
         */
#if defined(__aarch64__)
        if (use_streaming) {
            /* STNP: Store Pair of Q registers with Non-temporal hint
             * Part of ARMv8.0-A base instruction set (available on all AArch64)
             * Uses inline assembly (no portable intrinsic exists)
             * Constraints: "r" = general-purpose register (base address)
             *              "w" = FP/SIMD register, %qN = 128-bit vector view
             * Note: Non-temporal hint is implementation-defined; some cores may
             *       treat as regular cached stores (no performance gain).
             */
            __asm__ __volatile__(
                "stnp %q1, %q2, [%0]\n"      /* Store result.val[0,1] */
                "stnp %q3, %q4, [%0, #32]\n" /* Store result.val[2,3] */
                :
                : "r"(output + output_len), "w"(result.val[0]),
                  "w"(result.val[1]), "w"(result.val[2]), "w"(result.val[3])
                : "memory");
        } else {
            vst4q_u8(output + output_len, result);
        }
#else
        /* ARMv7 NEON: Regular stores only (32-bit ARM, no STNP) */
        vst4q_u8(output + output_len, result);
#endif
        output_len += 64;
    }

    /* Ensure all non-temporal stores are visible before continuing
     * DMB (Data Memory Barrier) ensures ordering on AArch64
     *
     * Conservative barrier: ensures all STNP stores complete before return.
     * Not strictly required for single-threaded use (C abstract machine handles
     * ordering), but provides x86-like sfence semantics for multi-core safety.
     * Analogous to x86 _mm_sfence() after _mm_stream_si128.
     */
#if defined(__aarch64__)
    if (use_streaming)
        __asm__ __volatile__("dmb ishst" ::: "memory");
#endif

    /* Fallback to scalar for remainder */
    extern size_t base64_encode_scalar(const uint8_t *restrict input,
                                       size_t input_len,
                                       uint8_t *restrict output);
    output_len +=
        base64_encode_scalar(input + i, input_len - i, output + output_len);

    return output_len;
}

#endif /* __aarch64__ || __ARM_NEON */

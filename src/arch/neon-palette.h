/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM NEON optimized palette conversion for kitty-doom.
 *
 * This module converts indexed 8-bit palette data to RGB24 format using NEON
 * SIMD instructions.
 *
 * Performance: Achieves 1.3-1.5x speedup over scalar baseline
 * Limitation: NEON lacks hardware gather instructions
 *
 * Why not faster?
 * - NEON provides no hardware gather instructions for 256-entry palette
 * lookups.
 * - The scalar gather loop is unavoidable and dominates execution time.
 * - NEON benefit comes primarily from vst3_u8 interleaved stores.
 *
 * Assumptions that the caller must guarantee:
 * - The palette_r/g/b_neon arrays have exactly 256 entries each.
 * - The indexed buffer has at least npixels valid bytes.
 * - The rgb24 buffer has at least npixels*3 bytes allocated.
 * - The npixels parameter can be any value (remainder is handled by scalar
 *   code).
 */

#pragma once

#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

/* Pre-expanded palette for lookup operations.
 *
 * Each color channel is stored separately (256 entries per channel) to improve
 * cache locality. All arrays are aligned to 64 bytes for optimal cache line
 * usage.
 */
static uint8_t palette_r_neon[256] __attribute__((aligned(64)));
static uint8_t palette_g_neon[256] __attribute__((aligned(64)));
static uint8_t palette_b_neon[256] __attribute__((aligned(64)));
static bool palette_initialized_neon = false;

/* Split the packed RGB palette into separate R, G, B arrays for efficient SIMD
 * access patterns.
 */
static inline void palette_init_neon(const uint8_t *restrict palette)
{
    for (int i = 0; i < 256; i++) {
        palette_r_neon[i] = palette[i * 3 + 0];
        palette_g_neon[i] = palette[i * 3 + 1];
        palette_b_neon[i] = palette[i * 3 + 2];
    }
    palette_initialized_neon = true;
}

/* Convert indexed 8-bit palette data to RGB24 format using NEON SIMD.
 *
 * Strategy: process 8 pixels per iteration for optimal granularity.
 * - Scalar gather is used to collect RGB values (unavoidable without hardware
 *   gather).
 * - Gathered values are loaded into NEON D registers (uint8x8_t).
 * - Interleaved RGB is stored using vst3_u8 (24 bytes per iteration).
 */
static inline void palette_to_rgb24_neon_impl(const uint8_t *restrict indexed,
                                              uint8_t *restrict rgb24,
                                              size_t npixels)
{
    /* Process 8 pixels at a time for optimal balance. */
    const size_t simd_pixels = (npixels / 8) * 8;

    /* Hoist stack arrays outside the loop to reduce stack traffic.
     *
     * These arrays are reused across all iterations for better register
     * allocation. This optimization was recommended by GPT-5.1 review.
     */
    uint8_t r_vals[8], g_vals[8], b_vals[8];

    for (size_t i = 0; i < simd_pixels; i += 8) {
        /* Perform scalar gather to collect RGB values.
         *
         * This is the bottleneck, but it is unavoidable without hardware
         * gather support. The compiler will typically unroll this loop and
         * keep palette base addresses in registers for optimal performance.
         */
        for (int j = 0; j < 8; j++) {
            uint8_t idx = indexed[i + j];
            r_vals[j] = palette_r_neon[idx];
            g_vals[j] = palette_g_neon[idx];
            b_vals[j] = palette_b_neon[idx];
        }

        /* Load the gathered values into NEON D registers (64-bit). */
        uint8x8_t r = vld1_u8(r_vals);
        uint8x8_t g = vld1_u8(g_vals);
        uint8x8_t b = vld1_u8(b_vals);

        /* Interleave and store RGB24 data using vst3_u8.
         *
         * This is where the SIMD benefit is realized:
         * - Each iteration processes 8 pixels × 3 channels = 24 bytes.
         * - Hardware interleaved store is significantly faster than scalar.
         *
         * Explicit .val[] initialization is used for clarity and better
         * compiler optimization.
         */
        uint8x8x3_t rgb;
        rgb.val[0] = r;
        rgb.val[1] = g;
        rgb.val[2] = b;
        vst3_u8(&rgb24[i * 3], rgb);
    }

    /* Handle any remaining pixels with scalar code. */
    for (size_t i = simd_pixels; i < npixels; i++) {
        uint8_t idx = indexed[i];
        rgb24[i * 3 + 0] = palette_r_neon[idx];
        rgb24[i * 3 + 1] = palette_g_neon[idx];
        rgb24[i * 3 + 2] = palette_b_neon[idx];
    }
}

#endif /* __aarch64__ || __ARM_NEON */

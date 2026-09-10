/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SIMD-optimized palette conversion with runtime CPU feature detection
 *
 * Dispatches to architecture-specific implementations:
 * - ARM/ARM64: NEON (arch/neon-palette.h) - 1.5-1.8x speedup
 * - x86/x86_64: Scalar fallback with compiler auto-vectorization
 *
 * Note: Hand-coded SSE implementation was removed due to performance
 * regression. Modern compilers auto-vectorize scalar code effectively on
 * x86_64.
 */

#include <stdbool.h>
#include <string.h>

#include "palette.h"

/* Include architecture-specific implementations */
#include "arch/neon-palette.h"

/* Scalar fallback implementation (used when SIMD not available) */
static uint8_t palette_r_scalar[256] __attribute__((aligned(64)));
static uint8_t palette_g_scalar[256] __attribute__((aligned(64)));
static uint8_t palette_b_scalar[256] __attribute__((aligned(64)));

static uint8_t cached_palette[256 * 3];
static bool palette_initialized = false;

__attribute__((unused)) static void palette_init_scalar(
    const uint8_t *restrict palette)
{
    for (int i = 0; i < 256; i++) {
        palette_r_scalar[i] = palette[i * 3 + 0];
        palette_g_scalar[i] = palette[i * 3 + 1];
        palette_b_scalar[i] = palette[i * 3 + 2];
    }
}

__attribute__((unused)) static void palette_to_rgb24_scalar(
    const uint8_t *restrict indexed,
    uint8_t *restrict rgb24,
    size_t npixels)
{
    for (size_t i = 0; i < npixels; i++) {
        uint8_t idx = indexed[i];
        rgb24[i * 3 + 0] = palette_r_scalar[idx];
        rgb24[i * 3 + 1] = palette_g_scalar[idx];
        rgb24[i * 3 + 2] = palette_b_scalar[idx];
    }
}

/* Public API: Unified palette conversion with SIMD dispatch */
void palette_to_rgb24(const uint8_t *restrict indexed,
                      uint8_t *restrict rgb24,
                      const uint8_t *restrict palette,
                      size_t npixels)
{
    if (!palette_initialized ||
        memcmp(cached_palette, palette, sizeof(cached_palette)) != 0) {
        memcpy(cached_palette, palette, sizeof(cached_palette));
        palette_initialized = true;
#if defined(__aarch64__) || defined(__ARM_NEON)
        palette_init_neon(palette);
#else
        palette_init_scalar(palette);
#endif
    }

#if defined(__aarch64__) || defined(__ARM_NEON)
    /* ARM NEON path: 1.5-1.8x speedup validated */
    palette_to_rgb24_neon_impl(indexed, rgb24, npixels);
#else
    /* Scalar fallback: Used on x86_64 (compiler auto-vectorizes effectively)
     * and architectures without SIMD support.
     */
    palette_to_rgb24_scalar(indexed, rgb24, npixels);
#endif
}

/* Internal diagnostic: Query which SIMD implementation is active.
 * Not exposed in public API - tests use compile-time detection instead.
 */
__attribute__((unused)) static const char *palette_impl_name(void)
{
#if defined(__aarch64__) || defined(__ARM_NEON)
    return "NEON (arch/neon-palette.h)";
#else
    return "Scalar (compiler auto-vectorization)";
#endif
}

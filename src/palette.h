/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SIMD-optimized palette conversion for kitty-doom.
 *
 * This module provides architecture-specific SIMD implementations for converting
 * indexed 8-bit palette data to RGB24 format. It bypasses PureDOOM's scalar
 * conversion for improved performance.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Convert indexed 8-bit palette data to RGB24 format using SIMD.
 *
 * This function dispatches to the appropriate architecture-specific SIMD
 * implementation at compile time based on available CPU features.
 *
 * Parameters:
 *   indexed: Pointer to 8-bit indexed pixel data (typically 320x200 = 64000 pixels).
 *   rgb24: Pointer to output RGB24 buffer (typically 64000 * 3 = 192000 bytes).
 *   palette: Pointer to 256-color palette (768 bytes: R,G,B for each color).
 *   npixels: Number of pixels to convert (can be any value; SIMD processes in
 *            chunks with scalar fallback for remainder).
 *
 * Implementation details:
 *   - ARM NEON: Processes 8 pixels per iteration using vst3_u8 interleaved stores.
 *   - x86 SSE/SSSE3: Processes 16 pixels per iteration with scalar interleaving.
 *   - Scalar fallback: Uses simple loop for platforms without SIMD support.
 *
 * Performance: Achieves 1.3-1.5x speedup over scalar baseline on ARM NEON.
 * Limitation: SIMD benefit is moderate due to lack of hardware gather instructions.
 */
void palette_to_rgb24(const uint8_t *restrict indexed,
                      uint8_t *restrict rgb24,
                      const uint8_t *restrict palette,
                      size_t npixels);

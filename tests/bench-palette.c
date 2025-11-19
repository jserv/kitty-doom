/* Benchmark for SIMD palette conversion */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/palette.h"

#define WIDTH 320
#define HEIGHT 200
#define NPIXELS (WIDTH * HEIGHT)
#define NCOLORS 256
#define RGB24_SIZE (NPIXELS * 3)
#define PALETTE_SIZE (NCOLORS * 3)

/* Generate test palette (simulates DOOM palette) */
static void generate_test_palette(uint8_t *palette)
{
    for (int i = 0; i < NCOLORS; i++) {
        palette[i * 3 + 0] = (i * 7) % 256;   /* R */
        palette[i * 3 + 1] = (i * 13) % 256;  /* G */
        palette[i * 3 + 2] = (i * 19) % 256;  /* B */
    }
}

/* Generate test indexed framebuffer */
static void generate_test_indexed(uint8_t *indexed)
{
    for (size_t i = 0; i < NPIXELS; i++)
        indexed[i] = (i * 17) % NCOLORS;  /* Pseudo-random pattern */
}

/* Scalar reference implementation for correctness testing */
static void palette_to_rgb24_scalar_ref(const uint8_t *indexed, uint8_t *rgb24,
                                        const uint8_t *palette, size_t npixels)
{
    for (size_t i = 0; i < npixels; i++) {
        uint8_t idx = indexed[i];
        rgb24[i * 3 + 0] = palette[idx * 3 + 0];
        rgb24[i * 3 + 1] = palette[idx * 3 + 1];
        rgb24[i * 3 + 2] = palette[idx * 3 + 2];
    }
}

int main(void)
{
    uint8_t *palette = malloc(PALETTE_SIZE);
    uint8_t *indexed = malloc(NPIXELS);
    uint8_t *rgb24_simd = malloc(RGB24_SIZE);
    uint8_t *rgb24_ref = malloc(RGB24_SIZE);

    if (!palette || !indexed || !rgb24_simd || !rgb24_ref) {
        fprintf(stderr, "Memory allocation failed\n");
        return EXIT_FAILURE;
    }

    generate_test_palette(palette);
    generate_test_indexed(indexed);

    printf("Palette Conversion Benchmark\n");
    printf("=============================\n");
    printf("Resolution: %dx%d = %d pixels\n", WIDTH, HEIGHT, NPIXELS);
    printf("RGB24 output size: %d bytes\n\n", RGB24_SIZE);

    /* Verify correctness first */
    palette_to_rgb24(indexed, rgb24_simd, palette, NPIXELS);
    palette_to_rgb24_scalar_ref(indexed, rgb24_ref, palette, NPIXELS);

    if (memcmp(rgb24_simd, rgb24_ref, RGB24_SIZE) != 0) {
        fprintf(stderr, "ERROR: SIMD output doesn't match scalar reference!\n");
        return EXIT_FAILURE;
    }
    printf("✓ Correctness verified (SIMD matches scalar)\n\n");

    /* Benchmark implementation - matches runtime dispatch in palette.c */
#if defined(__aarch64__) || defined(__ARM_NEON)
    const char *impl_name = "NEON (arch/neon-palette.h)";
#else
    /* SSE disabled in palette.c - compiler auto-vectorization used instead */
    const char *impl_name = "Scalar (compiler auto-vectorization)";
#endif
    printf("Implementation: %s\n", impl_name);

    const int warmup_iterations = 10;
    const int bench_iterations = 1000;

    /* Warmup */
    for (int i = 0; i < warmup_iterations; i++)
        palette_to_rgb24(indexed, rgb24_simd, palette, NPIXELS);

    /* Benchmark */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < bench_iterations; i++)
        palette_to_rgb24(indexed, rgb24_simd, palette, NPIXELS);

    clock_gettime(CLOCK_MONOTONIC, &end);

    long total_ns =
        (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);
    double avg_us = (double) total_ns / bench_iterations / 1000.0;
    double frame_budget_pct = (avg_us * 100.0) / 28571.0;  /* 35 FPS */

    printf("\nPerformance:\n");
    printf("  Average time: %.2f us/frame\n", avg_us);
    printf("  Frame budget: %.3f%% (of 28571 us @ 35 FPS)\n", frame_budget_pct);
    printf("  Throughput: %.2f MB/s\n",
           (RGB24_SIZE * bench_iterations) / (total_ns / 1000.0));

    /* Compare to expected PureDOOM scalar performance (~60 us) */
    double expected_scalar_us = 60.0;
    double speedup = expected_scalar_us / avg_us;
    printf("\nComparison to PureDOOM scalar (~60 us):\n");
    printf("  Speedup: %.2fx\n", speedup);

    if (speedup >= 3.0) {
        printf("  Status: ✓ EXCELLENT (3-5x target achieved)\n");
    } else if (speedup >= 2.0) {
        printf("  Status: ✓ GOOD (2-3x speedup)\n");
    } else if (speedup >= 1.5) {
        printf("  Status: ~ MODERATE (1.5-2x speedup)\n");
    } else {
        printf("  Status: ✗ POOR (< 1.5x speedup)\n");
    }

    free(palette);
    free(indexed);
    free(rgb24_simd);
    free(rgb24_ref);

    return EXIT_SUCCESS;
}

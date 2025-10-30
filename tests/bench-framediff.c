/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Frame differencing benchmark
 *
 * Measures the performance of NEON-accelerated frame difference detection
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include "../src/arch/neon-framediff.h"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include "../src/arch/sse-framediff.h"
#endif

#define WIDTH 320
#define HEIGHT 200
#define PIXEL_COUNT (WIDTH * HEIGHT)
#define FRAME_SIZE (PIXEL_COUNT * 3)

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static void fill_random_frame(uint8_t *frame, size_t size)
{
    for (size_t i = 0; i < size; i++)
        frame[i] = rand() % 256;
}

static void modify_frame(uint8_t *dest,
                         const uint8_t *src,
                         size_t size,
                         int change_percent)
{
    memcpy(dest, src, size);

    /* Modify change_percent of pixels */
    size_t pixels_to_change = (PIXEL_COUNT * change_percent) / 100;
    for (size_t i = 0; i < pixels_to_change; i++) {
        size_t pixel_idx = (rand() % PIXEL_COUNT) * 3;
        dest[pixel_idx] = rand() % 256;
        dest[pixel_idx + 1] = rand() % 256;
        dest[pixel_idx + 2] = rand() % 256;
    }
}

static void bench_framediff(const char *impl_name,
                            int change_percent,
                            uint8_t *frame1,
                            uint8_t *frame2)
{
    const int iterations = 1000;
    uint64_t min_time = UINT64_MAX;
    uint64_t total_time = 0;

    int detected_percent = 0;

    for (int i = 0; i < iterations; i++) {
        uint64_t start = get_time_ns();

#if defined(__aarch64__) || defined(__ARM_NEON)
        detected_percent =
            framediff_percentage_neon(frame1, frame2, PIXEL_COUNT);
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
        detected_percent =
            framediff_percentage_sse(frame1, frame2, PIXEL_COUNT);
#else
        /* Scalar fallback */
        size_t diff_pixels = 0;
        for (size_t j = 0; j < FRAME_SIZE; j += 3) {
            if (frame1[j] != frame2[j] || frame1[j + 1] != frame2[j + 1] ||
                frame1[j + 2] != frame2[j + 2]) {
                diff_pixels++;
            }
        }
        detected_percent = (int) ((diff_pixels * 100) / PIXEL_COUNT);
#endif

        uint64_t elapsed = get_time_ns() - start;
        if (elapsed < min_time)
            min_time = elapsed;
        total_time += elapsed;
    }

    double avg_time = (double) total_time / iterations / 1000.0; /* us */
    double min_time_us = (double) min_time / 1000.0;

    printf("%s - %d%% change:\n", impl_name, change_percent);
    printf("  Detected: %d%% changed pixels\n", detected_percent);
    printf("  Min time: %.2f us\n", min_time_us);
    printf("  Avg time: %.2f us\n", avg_time);
    printf("  Throughput: %.1f frames/sec\n", 1000000.0 / avg_time);
    printf("\n");
}

int main(void)
{
    srand(time(NULL));

    uint8_t *frame1 = malloc(FRAME_SIZE);
    uint8_t *frame2 = malloc(FRAME_SIZE);

    if (!frame1 || !frame2) {
        fprintf(stderr, "Failed to allocate frame buffers\n");
        return 1;
    }

    printf("========================================\n");
    printf("  Frame Differencing Benchmark\n");
    printf("========================================\n");
    printf("Frame size: %dx%d (%zu bytes)\n\n", WIDTH, HEIGHT,
           (size_t) FRAME_SIZE);

#if defined(__aarch64__) || defined(__ARM_NEON)
    const char *impl = "NEON";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
    const char *impl = "SSE2";
#else
    const char *impl = "Scalar";
#endif

    /* Test various change percentages */
    fill_random_frame(frame1, FRAME_SIZE);

    /* 0% change (identical frames) */
    memcpy(frame2, frame1, FRAME_SIZE);
    bench_framediff(impl, 0, frame1, frame2);

    /* 1% change (typical menu/idle) */
    modify_frame(frame2, frame1, FRAME_SIZE, 1);
    bench_framediff(impl, 1, frame1, frame2);

    /* 5% change (slow movement) */
    modify_frame(frame2, frame1, FRAME_SIZE, 5);
    bench_framediff(impl, 5, frame1, frame2);

    /* 20% change (active gameplay) */
    modify_frame(frame2, frame1, FRAME_SIZE, 20);
    bench_framediff(impl, 20, frame1, frame2);

    /* 50% change (intense action) */
    modify_frame(frame2, frame1, FRAME_SIZE, 50);
    bench_framediff(impl, 50, frame1, frame2);

    /* 100% change (scene transition) */
    fill_random_frame(frame2, FRAME_SIZE);
    bench_framediff(impl, 100, frame1, frame2);

    free(frame1);
    free(frame2);

    printf("\nFrame skip threshold: 5%%\n");
    printf("Frames with < 5%% change will be skipped, saving bandwidth.\n");
    printf("========================================\n");

    return 0;
}

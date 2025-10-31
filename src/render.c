/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kitty-doom is freely redistributable under the GNU GPL. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base64.h"
#include "kitty-doom.h"
#include "profiling.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#include "arch/neon-framediff.h"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include "arch/sse-framediff.h"
#endif

#define WIDTH 320
#define HEIGHT 200
/* Disable frame skipping for immediate menu response */
#define FRAME_SKIP_THRESHOLD 0

struct renderer {
    int screen_rows, screen_cols;
    long kitty_id;
    int frame_number;
    size_t encoded_buffer_size;
    uint8_t *prev_frame; /* Previous frame for diff detection */
    char encoded_buffer[];
};

renderer_t *renderer_create(int screen_rows, int screen_cols)
{
    /* Calculate base64 encoded size (4 * ceil(input_size / 3)) */
    const size_t bitmap_size = WIDTH * HEIGHT * 3;
    const size_t encoded_buffer_size = 4 * ((bitmap_size + 2) / 3) + 1;

    renderer_t *r = malloc(sizeof(renderer_t) + encoded_buffer_size);
    if (!r)
        return NULL;

    /* Allocate previous frame buffer for diff detection */
    r->prev_frame = malloc(bitmap_size);
    if (!r->prev_frame) {
        free(r);
        return NULL;
    }
    memset(r->prev_frame, 0, bitmap_size);

    *r = (renderer_t) {
        .screen_rows = screen_rows,
        .screen_cols = screen_cols,
        .frame_number = 0,
        .encoded_buffer_size = encoded_buffer_size,
        .kitty_id = 0,               /* Will be set below */
        .prev_frame = r->prev_frame, /* Already allocated above */
    };

    /* Generate random image ID for Kitty protocol */
    srand(time(NULL));
    r->kitty_id = rand();

    /* Set the window title */
    printf("\033]21;Kitty DOOM\033\\");

    /* Clear the screen and move cursor to home */
    printf("\033[2J\033[H");
    fflush(stdout);

    /* Log the active base64 implementation */
    fprintf(stderr, "Base64 implementation: %s\n", base64_get_impl_name());

    return r;
}

void renderer_destroy(renderer_t *restrict r)
{
    if (!r)
        return;

    /* Delete the Kitty graphics image */
    printf("\033_Ga=d,i=%ld;\033\\", r->kitty_id);
    fflush(stdout);

    /* Move cursor to home and clear screen */
    printf("\033[H\033[2J");
    fflush(stdout);

    /* Reset the window title */
    printf("\033]21\033\\");
    fflush(stdout);

    /* Free allocated buffers */
    free(r->prev_frame);
    free(r);
}

void renderer_render_frame(renderer_t *restrict r,
                           const unsigned char *restrict rgb24_frame)
{
    if (!r || !rgb24_frame)
        return;

    PROFILE_START(); /* Total render time */

    /* On first frame, ensure cursor is at home position */
    if (r->frame_number == 0) {
        printf("\033[H");
        fflush(stdout);
    }

    /* rgb24_frame is already in RGB24 format from doom_get_framebuffer(3) */
    const size_t bitmap_size = WIDTH * HEIGHT * 3;
    const size_t pixel_count = WIDTH * HEIGHT;

    /* Frame differencing: skip update if changes are minimal */
    if (r->frame_number > 0) {
        int diff_percentage = 0;

        PROFILE_START(); /* Frame diff time */
#if defined(__aarch64__) || defined(__ARM_NEON)
        /* Use NEON-accelerated diff detection */
        diff_percentage = framediff_percentage_neon(
            r->prev_frame, (const uint8_t *) rgb24_frame, pixel_count);
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
        /* Use SSE2-accelerated diff detection */
        diff_percentage = framediff_percentage_sse(
            r->prev_frame, (const uint8_t *) rgb24_frame, pixel_count);
#else
        /* Fallback to scalar diff detection */
        size_t diff_pixels = 0;
        const uint8_t *prev = r->prev_frame;
        const uint8_t *curr = (const uint8_t *) rgb24_frame;
        for (size_t i = 0; i < bitmap_size; i += 3) {
            if (prev[i] != curr[i] || prev[i + 1] != curr[i + 1] ||
                prev[i + 2] != curr[i + 2]) {
                diff_pixels++;
            }
        }
        diff_percentage = (int) ((diff_pixels * 100) / pixel_count);
#endif
        PROFILE_END("  Frame diff");

        /* Skip rendering if change is below threshold */
        if (diff_percentage < FRAME_SKIP_THRESHOLD) {
            return; /* Frame unchanged, skip transmission */
        }
    }

    /* Encode RGB data to base64 */
    PROFILE_START(); /* Base64 encoding time */
    size_t encoded_size =
        base64_encode_auto((const uint8_t *) rgb24_frame, bitmap_size,
                           (uint8_t *) r->encoded_buffer);
    r->encoded_buffer[encoded_size] = '\0';
    PROFILE_END("  Base64 encode");

    /* Send Kitty Graphics Protocol escape sequence with base64 data */
    /* Using Kitty mode (required for Kitty terminal) */
    const size_t chunk_size = 4096;

    PROFILE_START(); /* I/O transmission time */
    for (size_t encoded_offset = 0; encoded_offset < encoded_size;) {
        bool more_chunks = (encoded_offset + chunk_size) < encoded_size;

        if (encoded_offset == 0) {
            /* First chunk includes all image metadata */
            if (r->frame_number == 0) {
                /* First frame: create new image */
                printf("\033_Ga=T,i=%ld,f=24,s=%d,v=%d,q=2,c=%d,r=%d,m=%d;",
                       r->kitty_id, WIDTH, HEIGHT, r->screen_cols,
                       r->screen_rows, more_chunks ? 1 : 0);
            } else {
                /* Subsequent frames: frame action */
                printf("\033_Ga=f,r=1,i=%ld,f=24,x=0,y=0,s=%d,v=%d,m=%d;",
                       r->kitty_id, WIDTH, HEIGHT, more_chunks ? 1 : 0);
            }
        } else {
            /* Continuation chunks */
            if (r->frame_number == 0) {
                printf("\033_Gm=%d;", more_chunks ? 1 : 0);
            } else {
                printf("\033_Ga=f,r=1,m=%d;", more_chunks ? 1 : 0);
            }
        }

        /* Transfer payload */
        const size_t this_size =
            more_chunks ? chunk_size : encoded_size - encoded_offset;
        fwrite(r->encoded_buffer + encoded_offset, 1, this_size, stdout);
        printf("\033\\");
        fflush(stdout);

        encoded_offset += this_size;
    }

    /* For Kitty mode, animate the frame after first frame */
    if (r->frame_number > 0) {
        printf("\033_Ga=a,c=1,i=%ld;\033\\", r->kitty_id);
        fflush(stdout);
    }

    /* On first frame, add newline to move cursor below image */
    if (r->frame_number == 0) {
        printf("\r\n");
        fflush(stdout);
    }
    PROFILE_END("  I/O transmission");

    /* Update previous frame buffer for next diff */
    memcpy(r->prev_frame, rgb24_frame, bitmap_size);

    r->frame_number++;
    PROFILE_END("Total render time");
}

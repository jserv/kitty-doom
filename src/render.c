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

#define WIDTH 320
#define HEIGHT 200

struct renderer {
    int screen_rows, screen_cols;
    long kitty_id;
    int frame_number;
    size_t encoded_buffer_size;
    char *protocol_buffer; /* Buffer for batching Kitty protocol sequences */
    size_t protocol_buffer_size;
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

    /* Allocate protocol buffer for batching I/O
     * Size: max 64 chunks * (80 bytes header + 4096 data + 2 trailer) ~= 270 KB
     */
    const size_t protocol_buffer_size = 280 * 1024; /* 280 KB, rounded up */
    r->protocol_buffer = malloc(protocol_buffer_size);
    if (!r->protocol_buffer) {
        free(r);
        return NULL;
    }

    *r = (renderer_t) {
        .screen_rows = screen_rows,
        .screen_cols = screen_cols,
        .frame_number = 0,
        .encoded_buffer_size = encoded_buffer_size,
        .kitty_id = 0,                         /* Will be set below */
        .protocol_buffer = r->protocol_buffer, /* Already allocated above */
        .protocol_buffer_size = protocol_buffer_size,
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
    free(r->protocol_buffer);
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

    /* Encode RGB data to base64 */
    PROFILE_START(); /* Base64 encoding time */
    size_t encoded_size =
        base64_encode_auto((const uint8_t *) rgb24_frame, bitmap_size,
                           (uint8_t *) r->encoded_buffer);
    r->encoded_buffer[encoded_size] = '\0';
    PROFILE_END("  Base64 encode");

    /* Send Kitty Graphics Protocol escape sequence with base64 data */
    /* Batch all chunks into protocol_buffer for single fwrite() */
    const size_t chunk_size = 4096;
    char *buf = r->protocol_buffer;
    size_t buf_offset = 0;

    PROFILE_START(); /* I/O transmission time */
    for (size_t encoded_offset = 0; encoded_offset < encoded_size;) {
        bool more_chunks = (encoded_offset + chunk_size) < encoded_size;
        const size_t this_size =
            more_chunks ? chunk_size : encoded_size - encoded_offset;

        /* Build header */
        int header_len;
        size_t rem = r->protocol_buffer_size - buf_offset;

        if (encoded_offset == 0) {
            /* First chunk includes all image metadata */
            if (r->frame_number == 0) {
                /* First frame: create new image */
                header_len = snprintf(
                    buf + buf_offset, rem,
                    "\033_Ga=T,i=%ld,f=24,s=%d,v=%d,q=2,c=%d,r=%d,m=%d;",
                    r->kitty_id, WIDTH, HEIGHT, r->screen_cols, r->screen_rows,
                    more_chunks ? 1 : 0);
            } else {
                /* Subsequent frames: frame action */
                header_len =
                    snprintf(buf + buf_offset, rem,
                             "\033_Ga=f,r=1,i=%ld,f=24,x=0,y=0,s=%d,v=%d,m=%d;",
                             r->kitty_id, WIDTH, HEIGHT, more_chunks ? 1 : 0);
            }
        } else {
            /* Continuation chunks */
            if (r->frame_number == 0) {
                header_len = snprintf(buf + buf_offset, rem, "\033_Gm=%d;",
                                      more_chunks ? 1 : 0);
            } else {
                header_len =
                    snprintf(buf + buf_offset, rem, "\033_Ga=f,r=1,m=%d;",
                             more_chunks ? 1 : 0);
            }
        }

        /* Validate snprintf return value (GPT-5 + Claude recommendation) */
        if (header_len < 0) {
            fprintf(stderr,
                    "ERROR: snprintf encoding failed (frame %d, offset %zu)\n",
                    r->frame_number, encoded_offset);
            return; /* Skip frame to prevent corruption */
        }
        if ((size_t) header_len >= rem) {
            fprintf(stderr,
                    "ERROR: Protocol buffer overflow - header needs %d bytes, "
                    "only %zu available\n",
                    header_len, rem);
            return; /* Skip frame */
        }
        buf_offset += (size_t) header_len;

        /* Validate payload size before memcpy */
        if (buf_offset + this_size > r->protocol_buffer_size) {
            fprintf(
                stderr,
                "ERROR: Payload overflow - need %zu bytes, buffer size %zu\n",
                buf_offset + this_size, r->protocol_buffer_size);
            return;
        }

        /* Copy payload */
        memcpy(buf + buf_offset, r->encoded_buffer + encoded_offset, this_size);
        buf_offset += this_size;

        /* Validate trailer size before memcpy */
        if (buf_offset + 2 > r->protocol_buffer_size) {
            fprintf(stderr, "ERROR: Trailer overflow\n");
            return;
        }

        /* Copy trailer */
        memcpy(buf + buf_offset, "\033\\", 2);
        buf_offset += 2;

        encoded_offset += this_size;
    }

    /* For Kitty mode, animate the frame after first frame */
    if (r->frame_number > 0) {
        size_t rem = r->protocol_buffer_size - buf_offset;
        int anim_len = snprintf(buf + buf_offset, rem,
                                "\033_Ga=a,c=1,i=%ld;\033\\", r->kitty_id);

        /* Validate animation command snprintf */
        if (anim_len < 0) {
            fprintf(stderr, "ERROR: Animation snprintf failed\n");
            return;
        }
        if ((size_t) anim_len >= rem) {
            fprintf(stderr,
                    "ERROR: Animation command overflow - need %d bytes, have "
                    "%zu\n",
                    anim_len, rem);
            return;
        }
        buf_offset += (size_t) anim_len;
    }

    /* On first frame, add newline to move cursor below image */
    if (r->frame_number == 0) {
        if (buf_offset + 2 > r->protocol_buffer_size) {
            fprintf(stderr, "ERROR: Newline overflow\n");
            return;
        }
        memcpy(buf + buf_offset, "\r\n", 2);
        buf_offset += 2;
    }

    /* Single batched write */
    size_t written = fwrite(buf, 1, buf_offset, stdout);
    if (written != buf_offset) {
        fprintf(stderr,
                "WARNING: Short write - expected %zu bytes, wrote %zu\n",
                buf_offset, written);
    }
    if (fflush(stdout) != 0)
        fprintf(stderr, "WARNING: fflush failed\n");

    PROFILE_END("  I/O transmission");

    r->frame_number++;
    PROFILE_END("Total render time");
}

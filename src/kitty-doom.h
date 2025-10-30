/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kitty-doom is freely redistributable under the GNU GPL. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Common types */
typedef struct {
    int first;
    int second;
} int_pair_t;

/* Input subsystem */
typedef struct input input_t;

input_t *input_create(void);
void input_destroy(input_t *restrict input);
bool input_is_running(const input_t *restrict input);
void input_request_exit(input_t *restrict input);
int *input_get_device_attributes(const input_t *restrict input,
                                 int *restrict count);
int_pair_t input_get_screen_size(const input_t *restrict input);
int_pair_t input_get_screen_cells(const input_t *restrict input);

/* Renderer subsystem */
typedef struct renderer renderer_t;

renderer_t *renderer_create(int screen_rows, int screen_cols);
void renderer_destroy(renderer_t *restrict r);
void renderer_render_frame(renderer_t *restrict r,
                           const unsigned char *restrict rgb24_frame);

/* Operating System Abstraction Layer */
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

typedef struct os {
    struct termios term_attributes;
} os_t;

static inline os_t *os_create(void)
{
    os_t *os = malloc(sizeof(os_t));
    if (!os)
        return NULL;

    /* Get current terminal attributes */
    if (tcgetattr(STDIN_FILENO, &os->term_attributes) == -1) {
        free(os);
        return NULL;
    }

    /* Switch to raw mode (disables canonical input, flow control, etc.) */
    struct termios new_term_attributes = os->term_attributes;
    cfmakeraw(&new_term_attributes);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term_attributes) == -1) {
        free(os);
        return NULL;
    }

    return os;
}

static inline void os_destroy(os_t *os)
{
    if (!os)
        return;

    tcsetattr(STDIN_FILENO, TCSANOW, &os->term_attributes);
    free(os);
}

static inline int os_getch(void)
{
    unsigned char ch;
    ssize_t result = read(STDIN_FILENO, &ch, 1);
    return (result == 1) ? (int) ch : -1;
}

static inline int os_getch_timeout(int timeout_ms)
{
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

    if (result > 0) {
        /* Data available */
        unsigned char ch;
        ssize_t read_result = read(STDIN_FILENO, &ch, 1);
        return (read_result == 1) ? (int) ch : -1;
    }

    /* Timeout or error */
    return -1;
}

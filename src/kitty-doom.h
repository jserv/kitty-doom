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

/* Sound subsystem (optional, can be NULL if disabled) */
typedef struct sound_system sound_system_t;

sound_system_t *sound_init(void);
void sound_shutdown(sound_system_t *sound);
void sound_lock(sound_system_t *sound);
void sound_unlock(sound_system_t *sound);
bool sound_play_sfx(sound_system_t *sound, const char *sfx_name);
bool sound_play_music(sound_system_t *sound,
                      const char *music_name,
                      bool looping);
void sound_stop_music(sound_system_t *sound);
void sound_set_music_volume(sound_system_t *sound, float volume);
bool sound_is_music_playing(sound_system_t *sound);

/* Operating System Abstraction Layer */
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
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

    /* Best-effort terminal restoration - failure is non-fatal */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &os->term_attributes) != 0)
        perror("Warning: Failed to restore terminal settings");

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
    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };

    int result = poll(&pfd, 1, timeout_ms);

    if (result > 0 && (pfd.revents & POLLIN)) {
        /* Data available */
        unsigned char ch;
        ssize_t read_result = read(STDIN_FILENO, &ch, 1);
        return (read_result == 1) ? (int) ch : -1;
    }

    /* Timeout or error */
    return -1;
}

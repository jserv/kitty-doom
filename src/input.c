/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kitty-doom is freely redistributable under the GNU GPL. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PureDOOM.h"
#include "kitty-doom.h"

#define MAX_PARMS 32
#define MAX_DA 32
#define MAX_PENDING_RELEASES 16

typedef enum { STATE_GROUND, STATE_ESC, STATE_SS3, STATE_CSI } parser_state_t;

typedef struct {
    int key;
    struct timespec release_time;
} pending_release_t;

struct input {
    pthread_t thread;
    pthread_mutex_t query_mutex;
    pthread_cond_t query_condition;
    volatile bool exiting;
    volatile bool exit_requested;

    parser_state_t state;
    int parms[MAX_PARMS];
    int parm;
    int parm_count;
    char parm_prefix;

    int *device_attributes;
    int da_count;
    bool has_cell_size;
    int_pair_t cell_size;
    bool has_cursor_pos;
    int_pair_t cursor_pos;

    /* Pending key releases for non-blocking input */
    pending_release_t pending_releases[MAX_PENDING_RELEASES];
    int pending_count;
    pthread_mutex_t release_mutex;

    /* ESC key timeout tracking */
    struct timespec esc_time;
    bool esc_waiting;
};

static inline void for_each_modifier(int modifiers, void (*lambda)(int))
{
    if (modifiers >= 2) {
        const int modifier_mask = modifiers - 1;
        if (modifier_mask & 1)
            lambda(DOOM_KEY_SHIFT);
        if (modifier_mask & 2)
            lambda(DOOM_KEY_ALT);
        if (modifier_mask & 4)
            lambda(DOOM_KEY_CTRL);
    }
}

static void key_down(int key)
{
    doom_key_down(key);
}

/* Schedule a key release after specified delay (in milliseconds).
 * If the key is already scheduled, update its release time instead of creating
 * a duplicate entry. This handles key repeat correctly.
 */
static void sched_key_release(input_t *restrict input, int key, int delay_ms)
{
    pthread_mutex_lock(&input->release_mutex);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Calculate release time */
    struct timespec release_time = now;
    release_time.tv_nsec += delay_ms * 1000000L;
    if (release_time.tv_nsec >= 1000000000L) {
        release_time.tv_sec += release_time.tv_nsec / 1000000000L;
        release_time.tv_nsec %= 1000000000L;
    }

    /* Check if key is already scheduled - if so, update its time */
    for (int i = 0; i < input->pending_count; i++) {
        if (input->pending_releases[i].key == key) {
            input->pending_releases[i].release_time = release_time;
            pthread_mutex_unlock(&input->release_mutex);
            return;
        }
    }

    /* Add new key if not already scheduled */
    if (input->pending_count < MAX_PENDING_RELEASES) {
        input->pending_releases[input->pending_count++] =
            (pending_release_t) {.key = key, .release_time = release_time};
    }

    pthread_mutex_unlock(&input->release_mutex);
}

/* Schedule modifier key releases */
static void sched_modifier_releases(input_t *restrict input,
                                    int modifiers,
                                    int delay_ms)
{
    if (modifiers >= 2) {
        const int modifier_mask = modifiers - 1;
        if (modifier_mask & 1)
            sched_key_release(input, DOOM_KEY_SHIFT, delay_ms);
        if (modifier_mask & 2)
            sched_key_release(input, DOOM_KEY_ALT, delay_ms);
        if (modifier_mask & 4)
            sched_key_release(input, DOOM_KEY_CTRL, delay_ms);
    }
}

/* Process pending key releases - called from input thread loop */
static void process_pending_releases(input_t *restrict input)
{
    pthread_mutex_lock(&input->release_mutex);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Process releases in reverse order to allow safe removal */
    for (int i = input->pending_count - 1; i >= 0; i--) {
        const pending_release_t *pr = &input->pending_releases[i];

        /* Check if release time has passed */
        if (now.tv_sec > pr->release_time.tv_sec ||
            (now.tv_sec == pr->release_time.tv_sec &&
             now.tv_nsec >= pr->release_time.tv_nsec)) {
            /* Release the key */
            doom_key_up(pr->key);

            /* Remove from list by shifting remaining items */
            for (int j = i; j < input->pending_count - 1; j++)
                input->pending_releases[j] = input->pending_releases[j + 1];
            input->pending_count--;
        }
    }

    pthread_mutex_unlock(&input->release_mutex);
}

/* Check if a key is already scheduled for release */
static bool is_key_held(input_t *restrict input, int key)
{
    pthread_mutex_lock(&input->release_mutex);
    for (int i = 0; i < input->pending_count; i++) {
        if (input->pending_releases[i].key == key) {
            pthread_mutex_unlock(&input->release_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&input->release_mutex);
    return false;
}

static void ascii_key(input_t *restrict input, char ch)
{
    int doom_key = ch;
    if (doom_key == '\r')
        doom_key = DOOM_KEY_ENTER;
    if (doom_key == '\n')
        doom_key = DOOM_KEY_ENTER; /*  Kitty sends LF for Enter */

    /* Map Space, F, and I keys to fire (Ctrl is hard to capture in terminals)
     */
    if (ch == ' ' || ch == 'f' || ch == 'F' || ch == 'i' || ch == 'I')
        doom_key = DOOM_KEY_CTRL;

    /* Handle key repeat: only send key_down if not already held.
     * For repeated keys, just extend the release time.
     * This provides smooth continuous movement when holding arrow keys.
     */
    if (!is_key_held(input, doom_key))
        doom_key_down(doom_key);
    sched_key_release(input, doom_key, 50); /*  50ms delay */
}

static void ss3_key(input_t *restrict input, char ch)
{
    int doom_key = 0;
    switch (ch) {
    case 'P':
        doom_key = DOOM_KEY_F1;
        break;
    case 'Q':
        doom_key = DOOM_KEY_F2;
        break;
    case 'R':
        doom_key = DOOM_KEY_F3;
        break;
    case 'S':
        doom_key = DOOM_KEY_F4;
        break;
    }
    if (doom_key) {
        if (!is_key_held(input, doom_key))
            doom_key_down(doom_key);
        sched_key_release(input, doom_key, 50);
    }
}

static void csi_key(input_t *restrict input, char ch, int parm1, int parm2)
{
    int doom_key = 0;

    switch (ch) {
    case 'A':
        doom_key = DOOM_KEY_UP_ARROW;
        break;
    case 'B':
        doom_key = DOOM_KEY_DOWN_ARROW;
        break;
    case 'C':
        doom_key = DOOM_KEY_RIGHT_ARROW;
        break;
    case 'D':
        doom_key = DOOM_KEY_LEFT_ARROW;
        break;
    case '~':
        switch (parm1) {
        case 15:
            doom_key = DOOM_KEY_F5;
            break;
        case 17:
            doom_key = DOOM_KEY_F6;
            break;
        case 18:
            doom_key = DOOM_KEY_F7;
            break;
        case 19:
            doom_key = DOOM_KEY_F8;
            break;
        case 20:
            doom_key = DOOM_KEY_F9;
            break;
        case 21:
            doom_key = DOOM_KEY_F10;
            break;
        case 23:
            doom_key = DOOM_KEY_F11;
            break;
        case 24:
            doom_key = DOOM_KEY_F12;
            break;
        }
        break;
    }

    if (doom_key) {
        /* Differentiated key timing to handle terminal key repeat:
         * - Arrow keys: 150ms (covers typical terminal repeat interval of
         * 30-50ms)
         * - Other keys: 50ms (stable for menu navigation)
         *
         * Why 150ms for arrow keys?
         * Terminal key repeat sends events every 30-50ms. If our delay is too
         * short (e.g., 30ms), the key gets released before the next repeat
         * event arrives, causing down -> up -> down -> up cycles. 150ms ensures
         * the key stays pressed across multiple repeat events.
         */
        int delay_ms = 50; /* default */
        if (doom_key == DOOM_KEY_UP_ARROW || doom_key == DOOM_KEY_DOWN_ARROW ||
            doom_key == DOOM_KEY_LEFT_ARROW ||
            doom_key == DOOM_KEY_RIGHT_ARROW) {
            delay_ms = 150; /* long enough to cover terminal repeat intervals */
        }

        /* Handle key repeat: only send key_down if not already held */
        bool already_held = is_key_held(input, doom_key);

        if (!already_held) {
            for_each_modifier(parm2, key_down);
            doom_key_down(doom_key);
        }

        sched_key_release(input, doom_key, delay_ms);

        if (!already_held)
            sched_modifier_releases(input, parm2, delay_ms);
    }
}

static void device_attributes_report(input_t *restrict input)
{
    pthread_mutex_lock(&input->query_mutex);
    /* Signal that device attributes are available */
    pthread_cond_signal(&input->query_condition);
    pthread_mutex_unlock(&input->query_mutex);
}

static void cell_size_report(input_t *restrict input, int height, int width)
{
    pthread_mutex_lock(&input->query_mutex);
    input->cell_size = (int_pair_t) {.first = height, .second = width};
    input->has_cell_size = true;
    pthread_mutex_unlock(&input->query_mutex);
}

static void position_report(input_t *restrict input, int row, int col)
{
    pthread_mutex_lock(&input->query_mutex);
    input->cursor_pos = (int_pair_t) {.first = row, .second = col};
    input->has_cursor_pos = true;
    pthread_cond_signal(&input->query_condition);
    pthread_mutex_unlock(&input->query_mutex);
}

static void parse_char(input_t *restrict input, char ch)
{
    if (ch == 3) {
        /* Ctrl+C - immediate exit */
        input->exit_requested = true;
    } else if (ch == 27) {
        /* ESC - could be start of escape sequence OR standalone ESC key.
         * If parser is already in STATE_ESC, the previous ESC was standalone.
         */
        if (input->state == STATE_ESC)
            ascii_key(input, 27);
        input->state = STATE_ESC;
    } else if (input->state == STATE_GROUND) {
        ascii_key(input, ch);
    } else if (input->state == STATE_ESC) {
        if (ch == 'O') {
            /* Start of SS3 escape sequence */
            input->state = STATE_SS3;
        } else if (ch == '[') {
            /* Start of CSI escape sequence */
            input->state = STATE_CSI;
            input->parm = 0;
            input->parm_count = 0;
            input->parm_prefix = 0;
        } else {
            /* ESC followed by non-sequence character - send standalone ESC */
            ascii_key(input, 27);
            input->state = STATE_GROUND;
            /* Also process the current character if it's printable */
            if (ch >= 32 && ch < 127)
                ascii_key(input, ch);
        }
    } else if (input->state == STATE_SS3) {
        ss3_key(input, ch);
        input->state = STATE_GROUND;
    } else if (input->state == STATE_CSI) {
        if (ch >= '0' && ch <= '9') {
            input->parm = input->parm * 10 + (ch - '0');
        } else if (ch == ';') {
            if (input->parm_count < MAX_PARMS)
                input->parms[input->parm_count++] = input->parm;
            input->parm = 0;
        } else if (ch == '?' || ch == '>') {
            input->parm_prefix = ch;
        } else {
            if (input->parm_count < MAX_PARMS)
                input->parms[input->parm_count++] = input->parm;

            if (ch == 'c' && input->parm_prefix == '?') {
                /* Device attributes */
                if (input->device_attributes)
                    free(input->device_attributes);
                input->device_attributes =
                    malloc(input->parm_count * sizeof(int));
                if (input->device_attributes) {
                    memcpy(input->device_attributes, input->parms,
                           input->parm_count * sizeof(int));
                    input->da_count = input->parm_count;
                }
                device_attributes_report(input);
            } else if (ch == 't') {
                /* Cell size report */
                if (input->parm_count >= 3 && input->parms[0] == 4)
                    cell_size_report(input, input->parms[1], input->parms[2]);
            } else if (ch == 'R') {
                /* Cursor position report */
                if (input->parm_count >= 2)
                    position_report(input, input->parms[0], input->parms[1]);
            } else {
                csi_key(input, ch, input->parm_count > 0 ? input->parms[0] : 0,
                        input->parm_count > 1 ? input->parms[1] : 0);
            }

            input->state = STATE_GROUND;
        }
    }
}

static void *input_thread_func(void *arg)
{
    input_t *input = (input_t *) arg;
    while (!input->exiting) {
        /* Process any pending key releases first */
        process_pending_releases(input);

        int ch;

        /* If parser is in STATE_ESC, use timeout to detect standalone ESC */
        if (input->state == STATE_ESC) {
            ch = os_getch_timeout(1); /*  1ms timeout for quick polling */
            if (ch < 0) {
                /* Check if ESC has been waiting too long (100ms) */
                if (!input->esc_waiting) {
                    clock_gettime(CLOCK_MONOTONIC, &input->esc_time);
                    input->esc_waiting = true;
                } else {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    long elapsed_ms =
                        (now.tv_sec - input->esc_time.tv_sec) * 1000 +
                        (now.tv_nsec - input->esc_time.tv_nsec) / 1000000;
                    if (elapsed_ms >= 100) {
                        /* Timeout - the ESC was standalone */
                        ascii_key(input, 27);
                        input->state = STATE_GROUND;
                        input->esc_waiting = false;
                    }
                }
                continue;
            }
            /* Got a character, reset ESC waiting */
            input->esc_waiting = false;
        } else {
            /* Use short timeout to allow quick polling of pending releases */
            ch = os_getch_timeout(
                1); /*  1ms for responsive release processing */
            if (ch < 0) {
                /* No input - just loop back to process releases */
                continue;
            }
        }

        if (ch >= 0)
            parse_char(input, (char) ch);
    }
    return NULL;
}

input_t *input_create(void)
{
    input_t *input = malloc(sizeof(input_t));
    if (!input)
        return NULL;

    *input = (input_t) {
        .state = STATE_GROUND,
        .exiting = false,
        .exit_requested = false,
        .has_cell_size = false,
        .has_cursor_pos = false,
        .device_attributes = NULL,
        .da_count = 0,
        .pending_count = 0,
        .esc_waiting = false,
    };

    /* Initialize pthread synchronization primitives */
    if (pthread_mutex_init(&input->query_mutex, NULL) != 0) {
        free(input);
        return NULL;
    }

    if (pthread_cond_init(&input->query_condition, NULL) != 0) {
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    if (pthread_mutex_init(&input->release_mutex, NULL) != 0) {
        pthread_cond_destroy(&input->query_condition);
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    /* Hide the cursor */
    printf("\033[?25l");
    fflush(stdout);

    /* Start the keyboard thread */
    if (pthread_create(&input->thread, NULL, input_thread_func, input) != 0) {
        pthread_mutex_destroy(&input->release_mutex);
        pthread_cond_destroy(&input->query_condition);
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    /* Give the input thread time to start and be ready to receive responses.
     * This prevents timing issues where terminal queries are sent before the
     * input thread is ready to process responses.
     */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000}; /*  50ms */
    nanosleep(&ts, NULL);

    return input;
}

void input_destroy(input_t *input)
{
    if (!input)
        return;

    /* Signal thread to exit */
    input->exiting = true;

    /* Give the thread a moment to finish naturally */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000}; /*  10ms */
    nanosleep(&ts, NULL);

    pthread_join(input->thread, NULL);

    /* Restore terminal state */
    printf("\033[?25h"); /*  Show cursor */
    fflush(stdout);

    pthread_mutex_destroy(&input->query_mutex);
    pthread_cond_destroy(&input->query_condition);
    pthread_mutex_destroy(&input->release_mutex);

    free(input->device_attributes);
    free(input);
}

bool input_is_running(const input_t *restrict input)
{
    return input && !input->exit_requested;
}

void input_request_exit(input_t *restrict input)
{
    if (!input)
        return;

    /* Signal both the main loop and the input thread to exit */
    input->exit_requested = true;
    input->exiting = true;
}

int *input_get_device_attributes(const input_t *input, int *count)
{
    if (!input) {
        *count = 0;
        return NULL;
    }

    pthread_mutex_lock((pthread_mutex_t *) &input->query_mutex);

    /* Request primary device attributes */
    printf("\033[c");
    fflush(stdout);

    while (input->da_count == 0)
        pthread_cond_wait((pthread_cond_t *) &input->query_condition,
                          (pthread_mutex_t *) &input->query_mutex);

    *count = input->da_count;
    int *result = input->device_attributes;

    pthread_mutex_unlock((pthread_mutex_t *) &input->query_mutex);

    return result;
}

int_pair_t input_get_screen_size(const input_t *restrict input)
{
    if (!input)
        return (int_pair_t) {.first = 0, .second = 0};

    pthread_mutex_lock((pthread_mutex_t *) &input->query_mutex);

    /* Move to the bottom right corner and request the cell size and cursor
     * position
     */
    printf("\033[9999;9999H");
    printf("\033[16t");
    printf("\033[6n");
    fflush(stdout);

    input_t *inp = (input_t *) input;
    inp->has_cell_size = false;
    inp->has_cursor_pos = false;

    while (!input->has_cursor_pos)
        pthread_cond_wait((pthread_cond_t *) &input->query_condition,
                          (pthread_mutex_t *) &input->query_mutex);

    /* If no cell size is reported, assume VT340-compatible 20x10 */
    if (!input->has_cell_size) {
        inp->cell_size = (int_pair_t) {.first = 20, .second = 10};
    }

    const int cell_height = input->cell_size.first;
    const int cell_width = input->cell_size.second;
    const int rows = input->cursor_pos.first;
    const int columns = input->cursor_pos.second;

    const int_pair_t result = {
        .first = rows * cell_height,
        .second = columns * cell_width,
    };

    pthread_mutex_unlock((pthread_mutex_t *) &input->query_mutex);

    return result;
}

int_pair_t input_get_screen_cells(const input_t *restrict input)
{
    if (!input)
        return (int_pair_t) {.first = 0, .second = 0};

    pthread_mutex_lock((pthread_mutex_t *) &input->query_mutex);

    /* Move to the bottom right corner and request cursor position */
    printf("\033[9999;9999H");
    printf("\033[6n");
    fflush(stdout);

    input_t *inp = (input_t *) input;
    inp->has_cursor_pos = false;

    /* Wait with timeout (2 seconds) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    int wait_result =
        pthread_cond_timedwait((pthread_cond_t *) &input->query_condition,
                               (pthread_mutex_t *) &input->query_mutex, &ts);

    int_pair_t result;
    if (wait_result == 0 && input->has_cursor_pos) {
        /* Got response from terminal */
        result = input->cursor_pos;
    } else {
        /* Timeout or no response - use default size (80x24) */
        result = (int_pair_t) {.first = 24, .second = 80};
    }

    pthread_mutex_unlock((pthread_mutex_t *) &input->query_mutex);

    return result;
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kitty-doom is freely redistributable under the GNU GPL. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PureDOOM.h"
#include "kitty-doom.h"

#define MAX_PARMS 32
#define MAX_DA 32
#define MAX_PENDING_RELEASES 16
#define MAX_KEY_CODE 256

typedef enum { STATE_GROUND, STATE_ESC, STATE_SS3, STATE_CSI } parser_state_t;

typedef struct {
    int key;
    struct timespec release_time;
} pending_release_t;

/* Mouse state tracking for relative movement */
typedef struct {
    int last_x, last_y; /* Last recorded X/Y coordinate (terminal cells) */
    bool tracking;      /* Whether mouse tracking is active */
    bool buttons[3];    /* Button states [left, middle, right] */
} mouse_state_t;

struct input {
    pthread_t thread;
    pthread_mutex_t query_mutex;
    pthread_cond_t query_condition;
    pthread_cond_t ready_condition; /* For thread startup synchronization */
    atomic_bool exiting;
    atomic_bool exit_requested;
    bool ready; /* Set when input thread is ready to receive queries */

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

    /* Bitmap for O(1) key held detection (256 bits = 4 x 64-bit words)
     * Using atomic operations for lock-free access
     *
     * Memory ordering: relaxed is sufficient because:
     * - Each bit represents independent key state
     * - Key events are infrequent (> 1ms apart)
     * - Release delays (50-150ms) dwarf cache coherency latency (~100ns)
     * - Stale reads are harmless (corrected in next poll iteration)
     */
    _Atomic uint64_t held_keys_bitmap[4];

    /* ESC key timeout tracking */
    struct timespec esc_time;
    bool esc_waiting;

    /* Mouse tracking state */
    mouse_state_t mouse;
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

/* Mark key as held in bitmap (lock-free atomic operation) */
static inline void mark_key_held(input_t *restrict input, int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return;
    const int word = key / 64;
    const int bit = key % 64;
    atomic_fetch_or_explicit(&input->held_keys_bitmap[word], 1ULL << bit,
                             memory_order_relaxed);
}

/* Mark key as released in bitmap (lock-free atomic operation) */
static inline void mark_key_released(input_t *restrict input, int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return;
    const int word = key / 64;
    const int bit = key % 64;
    atomic_fetch_and_explicit(&input->held_keys_bitmap[word], ~(1ULL << bit),
                              memory_order_relaxed);
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
        mark_key_held(input, key); /* Mark in bitmap */
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
            mark_key_released(input, pr->key); /* Clear from bitmap */

            /* Remove from list by shifting remaining items */
            for (int j = i; j < input->pending_count - 1; j++)
                input->pending_releases[j] = input->pending_releases[j + 1];
            input->pending_count--;
        }
    }

    pthread_mutex_unlock(&input->release_mutex);
}

/* Check if a key is already held (lock-free atomic read, O(1)) */
static bool is_key_held(input_t *restrict input, int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return false;

    const int word = key / 64;
    const int bit = key % 64;

    uint64_t bitmap_word = atomic_load_explicit(&input->held_keys_bitmap[word],
                                                memory_order_relaxed);
    return (bitmap_word & (1ULL << bit)) != 0;
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
         * - Arrow keys: 80ms (balanced: good movement + fast menu response)
         * - Other keys: 50ms (stable for menu navigation)
         *
         * Why 80ms for arrow keys?
         * Terminal key repeat sends events every 30-50ms. After testing:
         * - 35ms: Menu extremely responsive, movement choppy (not smooth)
         * - 80ms: Menu fast, movement smooth (best balance)
         * - 100ms+: Menu sluggish, movement very smooth
         *
         * 80ms provides the best balance between smooth in-game movement
         * and fast menu navigation, verified through testing.
         */
        int delay_ms = 50; /* default */
        if (doom_key == DOOM_KEY_UP_ARROW || doom_key == DOOM_KEY_DOWN_ARROW ||
            doom_key == DOOM_KEY_LEFT_ARROW ||
            doom_key == DOOM_KEY_RIGHT_ARROW) {
            /* Balanced: smooth movement + fast menu */
            delay_ms = 80;
        }

        /* Handle key repeat: only send key_down if not already held.
         * Exception: if key is held but release is scheduled far in the future
         * (> 25ms), treat as a new distinct keypress for menu responsiveness.
         *
         * Why 25ms threshold?
         * - Terminal repeat events: 30-50ms interval
         * - Within 25ms: continuous key hold (extend release)
         * - Beyond 25ms: distinct keypress (immediate response)
         * - Balances smooth movement with responsive menus
         */
        bool already_held = is_key_held(input, doom_key);
        bool is_new_keypress = false;

        if (already_held) {
            /* Check how long until scheduled release */
            pthread_mutex_lock(&input->release_mutex);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            for (int i = 0; i < input->pending_count; i++) {
                if (input->pending_releases[i].key == doom_key) {
                    long time_to_release_ms =
                        (input->pending_releases[i].release_time.tv_sec -
                         now.tv_sec) *
                            1000 +
                        (input->pending_releases[i].release_time.tv_nsec -
                         now.tv_nsec) /
                            1000000;

                    /* If > 25ms until release, treat as new distinct keypress
                     */
                    if (time_to_release_ms > 25) {
                        /* Release old key immediately */
                        doom_key_up(doom_key);
                        mark_key_released(input, doom_key);

                        /* Remove from pending releases */
                        for (int j = i; j < input->pending_count - 1; j++)
                            input->pending_releases[j] =
                                input->pending_releases[j + 1];
                        input->pending_count--;

                        is_new_keypress = true;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&input->release_mutex);
        }

        if (!already_held || is_new_keypress) {
            for_each_modifier(parm2, key_down);
            doom_key_down(doom_key);
        }

        sched_key_release(input, doom_key, delay_ms);

        if (!already_held || is_new_keypress)
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

/* Parse SGR 1006 mouse event: \033[<Cb;Cx;CyM or \033[<Cb;Cx;Cym
 * Cb: button code + modifiers
 * Cx, Cy: column and row (1-based)
 * M: press, m: release
 *
 * Button codes (bits 0-1):
 * 0 = left, 1 = middle, 2 = right, 3 = release
 * Bit 5 (32): motion/drag flag
 * Bit 6 (64): wheel event flag
 * Bits 2-4: modifiers (shift=4, alt=8, ctrl=16)
 *
 * Thread safety: This function runs in input thread, calls PureDOOM API
 * (doom_mouse_move, doom_key_down) which we assume is thread-safe for
 * concurrent input events. Mouse state is owned by input thread only.
 */
static void parse_mouse_sgr(input_t *restrict input, char final_char)
{
#define MOUSE_SENSITIVITY 10 /* Adjust terminal cell movement to DOOM units */
#define MAX_DELTA_CLAMP 100  /* Clamp delta to ±100 cells (prevent jumps) */

    if (input->parm_count < 3)
        return;

    const int cb = input->parms[0]; /* Button code */
    const int cx = input->parms[1]; /* Column (1-based) */
    const int cy = input->parms[2]; /* Row (1-based) */

    /* Decode button code bitfield */
    const int button = cb & 3; /* Bits 0-1: button (0=left, 1=mid, 2=right) */
    const bool is_motion = (cb & 32) != 0;     /* Bit 5: motion flag */
    const bool is_wheel = (cb & 64) != 0;      /* Bit 6: wheel event flag */
    const bool is_press = (final_char == 'M'); /* M=press, m=release */

    /* Ignore wheel events (bit 6 set) - they use different button codes
     * Wheel up = 64, Wheel down = 65
     * Without this check, wheel events would be misinterpreted as left button
     */
    if (is_wheel)
        return;

    /* Initialize mouse tracking on first event */
    if (!input->mouse.tracking) {
        input->mouse.last_x = cx;
        input->mouse.last_y = cy;
        input->mouse.tracking = true;
        return;
    }

    /* Calculate relative movement (delta) with clamping
     * Clamping prevents huge deltas from terminal resize or coordinate jumps
     */
    int delta_x = cx - input->mouse.last_x;
    int delta_y = cy - input->mouse.last_y;

    /* Clamp deltas to prevent spurious jumps */
    if (delta_x > MAX_DELTA_CLAMP)
        delta_x = MAX_DELTA_CLAMP;
    else if (delta_x < -MAX_DELTA_CLAMP)
        delta_x = -MAX_DELTA_CLAMP;

    if (delta_y > MAX_DELTA_CLAMP)
        delta_y = MAX_DELTA_CLAMP;
    else if (delta_y < -MAX_DELTA_CLAMP)
        delta_y = -MAX_DELTA_CLAMP;

    /* Apply sensitivity and update DOOM engine */
    delta_x *= MOUSE_SENSITIVITY;
    delta_y *= MOUSE_SENSITIVITY;

    if (delta_x != 0 || delta_y != 0) {
        doom_mouse_move(delta_x, delta_y);
        input->mouse.last_x = cx;
        input->mouse.last_y = cy;
    }

    /* Handle button events (exclude motion-only events)
     * Note: Button holds use fixed 50ms delay. For continuous fire,
     * user must click repeatedly. This matches other action keys.
     */
    if (!is_motion && button < 3) {
        if (is_press && !input->mouse.buttons[button]) {
            /* Button pressed */
            input->mouse.buttons[button] = true;

            /* Map buttons to DOOM keys:
             * Left button -> Fire (Ctrl)
             * Right button -> Use/Open door (Space)
             * Middle button -> Run (Shift)
             */
            int doom_key = 0;
            switch (button) {
            case 0: /* Left button */
                doom_key = DOOM_KEY_CTRL;
                break;
            case 2:             /* Right button */
                doom_key = ' '; /* Use key (same as Space) */
                break;
            case 1:                        /* Middle button */
                doom_key = DOOM_KEY_SHIFT; /* Run key */
                break;
            }

            if (doom_key) {
                doom_key_down(doom_key);
                /* 50ms delay matches keyboard action keys
                 * For continuous fire, user must click repeatedly
                 */
                sched_key_release(input, doom_key, 50);
            }
        } else if (!is_press && input->mouse.buttons[button]) {
            /* Button released */
            input->mouse.buttons[button] = false;
            /* Key release is handled by sched_key_release() timer */
        }
    }

#undef MAX_DELTA_CLAMP
#undef MOUSE_SENSITIVITY
}

static void parse_char(input_t *restrict input, char ch)
{
    if (ch == 3) {
        /* Ctrl+C - immediate exit */
        atomic_store_explicit(&input->exit_requested, true,
                              memory_order_relaxed);
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
        /* Handle prefix characters FIRST (before digits) */
        if (ch == '?' || ch == '>' || ch == '<') {
            input->parm_prefix = ch;
        } else if (ch >= '0' && ch <= '9') {
            input->parm = input->parm * 10 + (ch - '0');
        } else if (ch == ';') {
            if (input->parm_count < MAX_PARMS)
                input->parms[input->parm_count++] = input->parm;
            input->parm = 0;
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
            } else if (ch == 'M' || ch == 'm') {
                /* SGR 1006 mouse event (if prefix is '<') */
                if (input->parm_prefix == '<')
                    parse_mouse_sgr(input, ch);
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

    /* Signal that the thread is ready to receive terminal responses */
    pthread_mutex_lock(&input->query_mutex);
    input->ready = true;
    pthread_cond_signal(&input->ready_condition);
    pthread_mutex_unlock(&input->query_mutex);

    while (!atomic_load_explicit(&input->exiting, memory_order_relaxed)) {
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
        .ready = false,
        .has_cell_size = false,
        .has_cursor_pos = false,
        .device_attributes = NULL,
        .da_count = 0,
        .pending_count = 0,
        .held_keys_bitmap = {0}, /* Initialize bitmap to all zeros */
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

    if (pthread_cond_init(&input->ready_condition, NULL) != 0) {
        pthread_cond_destroy(&input->query_condition);
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    if (pthread_mutex_init(&input->release_mutex, NULL) != 0) {
        pthread_cond_destroy(&input->ready_condition);
        pthread_cond_destroy(&input->query_condition);
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    /* Hide the cursor */
    printf("\033[?25l");
    fflush(stdout);

    /* Enable mouse tracking (SGR 1006 mode)
     * - ?1000h: Enable mouse button press/release events
     * - ?1003h: Enable "any event" tracking (includes motion)
     * - ?1006h: Enable SGR extended format (no 222 col limit)
     */
    printf("\033[?1000h\033[?1003h\033[?1006h");
    fflush(stdout);

    /* Start the keyboard thread */
    if (pthread_create(&input->thread, NULL, input_thread_func, input) != 0) {
        pthread_mutex_destroy(&input->release_mutex);
        pthread_cond_destroy(&input->ready_condition);
        pthread_cond_destroy(&input->query_condition);
        pthread_mutex_destroy(&input->query_mutex);
        free(input);
        return NULL;
    }

    /* Wait for the input thread to signal it's ready to receive responses.
     * This prevents timing issues where terminal queries are sent before the
     * input thread is ready to process responses.
     */
    pthread_mutex_lock(&input->query_mutex);
    while (!input->ready)
        pthread_cond_wait(&input->ready_condition, &input->query_mutex);
    pthread_mutex_unlock(&input->query_mutex);

    return input;
}

void input_destroy(input_t *input)
{
    if (!input)
        return;

    /* Signal thread to exit */
    atomic_store_explicit(&input->exiting, true, memory_order_relaxed);

    /* Give the thread a moment to finish naturally */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000}; /*  10ms */
    nanosleep(&ts, NULL);

    pthread_join(input->thread, NULL);

    /* Drain stdin to prevent spurious terminal responses from appearing
     * after exit. This handles delayed responses from terminal probes
     * (e.g., Kitty Graphics Protocol queries) that may arrive after the
     * main program has finished.
     */
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) == 0) {
        struct termios drain_tio = tio;
        drain_tio.c_lflag &= ~(ICANON | ECHO);
        drain_tio.c_cc[VMIN] = 0;
        drain_tio.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &drain_tio);

        /* Drain all pending input */
        char drain_buf[256];
        while (read(STDIN_FILENO, drain_buf, sizeof(drain_buf)) > 0)
            ;

        /* Restore original terminal state */
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }

    /* Restore terminal state */
    printf("\033[?1006l\033[?1003l\033[?1000l"); /* Disable mouse tracking */
    printf("\033[?25h");                         /*  Show cursor */
    fflush(stdout);

    pthread_mutex_destroy(&input->query_mutex);
    pthread_cond_destroy(&input->query_condition);
    pthread_cond_destroy(&input->ready_condition);
    pthread_mutex_destroy(&input->release_mutex);

    free(input->device_attributes);
    free(input);
}

bool input_is_running(const input_t *restrict input)
{
    return input &&
           !atomic_load_explicit(&input->exit_requested, memory_order_relaxed);
}

void input_request_exit(input_t *restrict input)
{
    if (!input)
        return;

    /* Signal both the main loop and the input thread to exit */
    atomic_store_explicit(&input->exit_requested, true, memory_order_relaxed);
    atomic_store_explicit(&input->exiting, true, memory_order_relaxed);
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

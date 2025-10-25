/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Lock-free bitmap concurrent access test
 *
 * Tests atomic bitmap operations under concurrent access
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#define MAX_KEY_CODE 256
#define NUM_THREADS 4
#define ITERATIONS 100000

static _Atomic uint64_t held_keys_bitmap[4] = {0};

static inline void mark_key_held(int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return;
    const int word = key / 64;
    const int bit = key % 64;
    atomic_fetch_or_explicit(&held_keys_bitmap[word], 1ULL << bit,
                             memory_order_relaxed);
}

static inline void mark_key_released(int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return;
    const int word = key / 64;
    const int bit = key % 64;
    atomic_fetch_and_explicit(&held_keys_bitmap[word], ~(1ULL << bit),
                              memory_order_relaxed);
}

static inline bool is_key_held(int key)
{
    if (key < 0 || key >= MAX_KEY_CODE)
        return false;
    const int word = key / 64;
    const int bit = key % 64;
    uint64_t bitmap_word =
        atomic_load_explicit(&held_keys_bitmap[word], memory_order_relaxed);
    return (bitmap_word & (1ULL << bit)) != 0;
}

/* Thread function: set/clear keys in dedicated range to avoid conflicts */
static void *worker_thread(void *arg)
{
    int thread_id = *(int *) arg;
    unsigned int seed = time(NULL) + thread_id;

    /* Each thread operates on a disjoint key range to avoid interference */
    int key_base = thread_id * (MAX_KEY_CODE / NUM_THREADS);
    int key_range = MAX_KEY_CODE / NUM_THREADS;

    for (int i = 0; i < ITERATIONS; i++) {
        int key = key_base + (rand_r(&seed) % key_range);

        mark_key_held(key);

        /* Verify immediately (no other thread touches this key) */
        if (!is_key_held(key)) {
            fprintf(stderr, "Thread %d: Key %d not held after mark!\n",
                    thread_id, key);
            exit(1);
        }

        /* Small random delay */
        for (volatile int j = 0; j < (rand_r(&seed) % 100); j++)
            ;

        mark_key_released(key);

        /* Verify cleared */
        if (is_key_held(key)) {
            fprintf(stderr, "Thread %d: Key %d still held after release!\n",
                    thread_id, key);
            exit(1);
        }
    }

    return NULL;
}

/* Wrapper for pthread compatibility in same-bit contention test */
static void *mark_key_held_wrapper(void *arg)
{
    int key = (int) (intptr_t) arg;
    mark_key_held(key);
    return NULL;
}

/* Thread function for same-word concurrent testing (all threads use keys 0-63)
 */
static void *same_word_worker(void *arg)
{
    int thread_id = *(int *) arg;
    unsigned int seed = time(NULL) + thread_id;

    /* All threads operate on word 0 (keys 0-63) */
    for (int i = 0; i < ITERATIONS; i++) {
        int key = rand_r(&seed) % 64; /* Keys 0-63 all map to word 0 */

        mark_key_held(key);

        /* Small random delay */
        for (volatile int j = 0; j < (rand_r(&seed) % 100); j++)
            ;

        mark_key_released(key);
    }

    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    printf("Lock-free bitmap concurrent test\n");
    printf("Threads: %d, Iterations: %d per thread\n\n", NUM_THREADS,
           ITERATIONS);

    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]) !=
            0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }

    /* Wait for completion */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify all keys are released */
    int held_count = 0;
    for (int key = 0; key < MAX_KEY_CODE; key++) {
        if (is_key_held(key))
            held_count++;
    }

    printf("Test completed successfully\n");
    printf("Final held keys: %d (should be 0)\n", held_count);

    if (held_count > 0) {
        fprintf(stderr, "ERROR: Some keys still held!\n");
        return 1;
    }

    printf("\nAll atomic operations correct under concurrent access\n");

    /* Additional test: concurrent access to same word, different bits */
    printf("\n--- Testing same-word concurrent access (keys 0-63) ---\n");

    /* Reset bitmap */
    for (int i = 0; i < 4; i++)
        atomic_store(&held_keys_bitmap[i], 0);

    /* Create threads that all modify word 0 (keys 0-63) */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, same_word_worker,
                           &thread_ids[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    held_count = 0;
    for (int key = 0; key < MAX_KEY_CODE; key++) {
        if (is_key_held(key))
            held_count++;
    }

    printf("Final held keys: %d (should be 0)\n", held_count);

    if (held_count > 0) {
        fprintf(stderr, "ERROR: Some keys still held!\n");
        return 1;
    }

    printf("Same-word concurrent access verified\n");

    /* Additional test: same-bit contention (worst case) */
    printf("\n--- Testing same-bit contention (all threads, key 42) ---\n");

    /* Reset bitmap */
    for (int i = 0; i < 4; i++)
        atomic_store(&held_keys_bitmap[i], 0);

    /* All threads hammer the same key */
    const int contention_key = 42;
    for (int round = 0; round < 1000; round++) {
        /* Create threads */
        for (int i = 0; i < NUM_THREADS; i++) {
            thread_ids[i] = i;
            if (pthread_create(&threads[i], NULL, mark_key_held_wrapper,
                               (void *) (intptr_t) contention_key) != 0) {
                fprintf(stderr, "Failed to create thread %d\n", i);
                return 1;
            }
        }
        for (int i = 0; i < NUM_THREADS; i++)
            pthread_join(threads[i], NULL);

        /* Verify key is set */
        if (!is_key_held(contention_key)) {
            fprintf(stderr, "ERROR: Key %d not held after contention!\n",
                    contention_key);
            return 1;
        }

        /* Clear */
        mark_key_released(contention_key);

        /* Verify cleared */
        if (is_key_held(contention_key)) {
            fprintf(stderr, "ERROR: Key %d still held after clear!\n",
                    contention_key);
            return 1;
        }
    }

    printf("Completed 1000 rounds of same-bit contention\n");
    printf("Same-bit contention verified\n");

    return 0;
}

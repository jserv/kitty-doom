/* Sound system implementation using miniaudio + PureDOOM audio buffer API
 *
 * PureDOOM audio specifications:
 * - Sample rate: 11025Hz (DOOM_SAMPLERATE)
 * - Buffer size: 512 samples per frame
 * - Format: 16-bit stereo (2 channels)
 * - Total: 2048 bytes per buffer (512 samples × 2 channels × 2 bytes)
 *
 * Integration approach:
 * - Use doom_get_sound_buffer() to retrieve mixed audio from PureDOOM
 * - miniaudio device reads from this buffer via callback
 * - No external sound files needed (DOOM mixes internally from WAD)
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty-doom.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* PureDOOM audio buffer API */
extern short *doom_get_sound_buffer(void);

#define DOOM_SAMPLERATE 11025

/* Sound system state */
struct sound_system {
    ma_device device;
    pthread_mutex_t mutex;
    bool initialized;
};

/* miniaudio callback - reads from PureDOOM's audio buffer
 * This runs in a separate thread managed by miniaudio
 *
 * PureDOOM's doom_get_sound_buffer() returns a fixed 512-frame buffer
 * (2048 bytes = 512 frames × 2 channels × 2 bytes).
 * We configure periodSizeInFrames=512 to match, but add safety checks for edge
 * cases where the device might request different sizes.
 */
static void audio_callback(ma_device *device,
                           void *output,
                           const void *input,
                           ma_uint32 frame_count)
{
    sound_system_t *s = (sound_system_t *) device->pUserData;
    (void) input; /* Unused */

    /* Lock to synchronize with doom_update() which calls
     * doom_get_sound_buffer() Reference: PureDOOM README SDL example uses
     * SDL_LockAudio()
     */
    pthread_mutex_lock(&s->mutex);

    /* Retrieve mixed audio from DOOM engine
     * doom_get_sound_buffer() internally calls I_UpdateSound() which mixes
     * all active sound effects and music into the buffer (always 512 frames)
     */
    short *doom_buffer = doom_get_sound_buffer();

    /* Safety: Only copy up to 512 frames (DOOM buffer size)
     * If device requests more, we'll fill remainder with silence
     * If device requests less, we only copy what's requested
     */
    ma_uint32 frames_to_copy = (frame_count < 512) ? frame_count : 512;
    size_t bytes_to_copy = frames_to_copy * 2 * sizeof(short); /* 2 channels */
    memcpy(output, doom_buffer, bytes_to_copy);

    /* If device requested more than 512 frames, fill remainder with silence */
    if (frame_count > 512) {
        short *output_ptr = (short *) output;
        size_t silence_samples = (frame_count - 512) * 2; /* 2 channels */
        memset(&output_ptr[512 * 2], 0, silence_samples * sizeof(short));
    }

    pthread_mutex_unlock(&s->mutex);
}

sound_system_t *sound_init(void)
{
    sound_system_t *s = malloc(sizeof(sound_system_t));
    if (!s) {
        fprintf(stderr, "sound_init: malloc failed\n");
        return NULL;
    }

    memset(s, 0, sizeof(*s));

    /* Initialize mutex for synchronization with doom_update() */
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        fprintf(stderr, "sound_init: pthread_mutex_init failed\n");
        free(s);
        return NULL;
    }

    /* Configure miniaudio device to match PureDOOM's specifications */
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16; /* 16-bit signed PCM */
    config.playback.channels = 2;           /* Stereo */
    config.sampleRate = DOOM_SAMPLERATE;    /* 11025 Hz */
    config.periodSizeInFrames = 512;        /* Match DOOM buffer size */
    config.periods = 2;                     /* Double buffering */
    config.dataCallback = audio_callback;
    config.pUserData = s;

    /* Initialize and start audio device */
    ma_result result = ma_device_init(NULL, &config, &s->device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "sound_init: ma_device_init failed: %d\n", result);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }

    result = ma_device_start(&s->device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "sound_init: ma_device_start failed: %d\n", result);
        ma_device_uninit(&s->device);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }

    s->initialized = true;
    return s;
}

void sound_shutdown(sound_system_t *s)
{
    if (!s)
        return;

    if (s->initialized) {
        ma_device_uninit(&s->device);
        pthread_mutex_destroy(&s->mutex);
        s->initialized = false;
    }

    free(s);
}

/* Lock audio mutex before calling doom_update()
 * This prevents race conditions between DOOM's sound engine and audio callback
 * Should be called by main loop before doom_update()
 */
void sound_lock(sound_system_t *s)
{
    if (s && s->initialized)
        pthread_mutex_lock(&s->mutex);
}

/* Unlock audio mutex after doom_update()
 * Should be called by main loop after doom_update()
 */
void sound_unlock(sound_system_t *s)
{
    if (s && s->initialized)
        pthread_mutex_unlock(&s->mutex);
}

/* Stub functions - PureDOOM handles sound/music internally via doom_update()
 * These APIs are kept for future manual control if needed
 */
bool sound_play_sfx(sound_system_t *s, const char *sfx_name)
{
    (void) s;
    (void) sfx_name;
    /* PureDOOM automatically plays sounds via S_StartSound() calls */
    return false;
}

bool sound_play_music(sound_system_t *s, const char *music_name, bool looping)
{
    (void) s;
    (void) music_name;
    (void) looping;
    /* PureDOOM automatically plays music via S_ChangeMusic() calls */
    return false;
}

void sound_stop_music(sound_system_t *s)
{
    (void) s;
    /* Music is controlled by PureDOOM internally */
}

void sound_set_music_volume(sound_system_t *s, float volume)
{
    (void) s;
    (void) volume;
    /* Volume is controlled by PureDOOM's snd_MusicVolume variable */
}

bool sound_is_music_playing(sound_system_t *s)
{
    (void) s;
    /* PureDOOM manages music state internally */
    return false;
}

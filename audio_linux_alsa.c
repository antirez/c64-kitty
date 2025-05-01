#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define BUFFERS_COUNT 3
#define MAX_C64_BUFFER_LEN (1024*64)
#define PCM_DEVICE "default"

// Our audio state is encapsulated here.
typedef struct {
    snd_pcm_t *pcm_handle;
    pthread_t playback_thread;
    int thread_running;

    pthread_mutex_t buffer_mutex;

    // Samples arriving from the emulator.
    short *c64_buffer;
    size_t c64_buffer_len;

    // Audio format parameters
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_format_t format;
    snd_pcm_uframes_t period_size;

} AudioState;

// Thread function for continuous audio playback
// ALSA has a synchronous API, so we can use a separate thread for playback
// It will continuously read from the c64_buffer and write to the ALSA PCM device
static void *playback_thread_func(void *data) {
    AudioState *state = (AudioState *)data;
    int err;

    // Buffer size based on period size
    size_t buffer_size = state->period_size * state->channels * snd_pcm_format_width(state->format) / 8;
    short *playback_buffer = malloc(buffer_size);

    if (!playback_buffer) {
        fprintf(stderr, "Failed to allocate playback buffer\n");
        return NULL;
    }

    while (state->thread_running) {
        // Clear buffer initially
        memset(playback_buffer, 0, buffer_size);

        size_t samples_to_play = state->period_size;

        // Lock mutex before accessing shared buffer
        pthread_mutex_lock(&state->buffer_mutex);

        if (state->c64_buffer_len > 0) {
            // Copy data from c64 buffer to playback buffer
            size_t samples_to_copy = samples_to_play;
            if (samples_to_copy > state->c64_buffer_len) {
                samples_to_copy = state->c64_buffer_len;
            }

            memcpy(playback_buffer, state->c64_buffer, samples_to_copy * sizeof(short));

            // Move remaining data to beginning of buffer
            if (samples_to_copy < state->c64_buffer_len) {
                memmove(state->c64_buffer,
                        state->c64_buffer + samples_to_copy,
                        (state->c64_buffer_len - samples_to_copy) * sizeof(short));
            }

            state->c64_buffer_len -= samples_to_copy;
        } else {
            // No data, just output silence
            printf("."); fflush(stdout);
        }

        pthread_mutex_unlock(&state->buffer_mutex);

        // Write to sound device
        err = snd_pcm_writei(state->pcm_handle, playback_buffer, samples_to_play);

        if (err == -EPIPE) {
            // EPIPE means underrun
            fprintf(stderr, "Underrun occurred\n");
            snd_pcm_prepare(state->pcm_handle);
        } else if (err < 0) {
            fprintf(stderr, "Error from writei: %s\n", snd_strerror(err));
            break;
        } else if (err != (int)samples_to_play) {
            fprintf(stderr, "Short write: wrote %d frames instead of %ld\n", err, samples_to_play);
        }
    }

    free(playback_buffer);
    return NULL;
}

/* This function receive samples from the emulator. It will
 * feed the buffer that will later be used in order to provide
 * samples to the core audio buffers. */
void audio_from_emulator(const float *samples, int num_samples, void *user_data) {
    AudioState *state = (AudioState *)user_data;

    pthread_mutex_lock(&state->buffer_mutex);

    if (state->c64_buffer_len >= MAX_C64_BUFFER_LEN) {
        printf("!"); fflush(stdout);
        pthread_mutex_unlock(&state->buffer_mutex);
        return;
    }

    state->c64_buffer = realloc(state->c64_buffer,
                              sizeof(short) * (state->c64_buffer_len + num_samples));

    if (state->c64_buffer == NULL) {
        fprintf(stderr, "Failed to reallocate audio buffer\n");
        pthread_mutex_unlock(&state->buffer_mutex);
        return;
    }

    for (int j = 0; j < num_samples; j++) {
        state->c64_buffer[state->c64_buffer_len] = (short)(samples[j] * 32767);
        state->c64_buffer_len++;
    }

    pthread_mutex_unlock(&state->buffer_mutex);
}

void audio_cleanup(void *audio_data) {
    AudioState *state = (AudioState *)audio_data;

    if (!state) return;

    // Stop playback thread
    if (state->thread_running) {
        state->thread_running = 0;
        pthread_join(state->playback_thread, NULL);
    }

    // Close ALSA device
    if (state->pcm_handle) {
        snd_pcm_drop(state->pcm_handle);
        snd_pcm_close(state->pcm_handle);
    }

    // Clean up buffer
    pthread_mutex_lock(&state->buffer_mutex);
    if (state->c64_buffer) {
        free(state->c64_buffer);
        state->c64_buffer = NULL;
    }
    pthread_mutex_unlock(&state->buffer_mutex);

    // Destroy mutex
    pthread_mutex_destroy(&state->buffer_mutex);

    free(state);
}

void *audio_init(void) {
    int err;

    // Initialize the audio state
    AudioState *state = malloc(sizeof(AudioState));
    if (state == NULL) return NULL;
    memset(state, 0, sizeof(*state));

    // Initialize mutex
    if (pthread_mutex_init(&state->buffer_mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize mutex\n");
        free(state);
        return NULL;
    }

    // Set up the audio format parameters
    state->sample_rate = 44100;
    state->channels = 1;  // Mono
    state->format = SND_PCM_FORMAT_S16_LE;  // 16-bit signed little endian

    // Open PCM device
    err = snd_pcm_open(&state->pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio device %s: %s\n", PCM_DEVICE, snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Allocate hardware params
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);

    // Fill with default values
    err = snd_pcm_hw_params_any(state->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "Cannot initialize hardware params: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Set access type
    err = snd_pcm_hw_params_set_access(state->pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "Cannot set access type: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Set format
    err = snd_pcm_hw_params_set_format(state->pcm_handle, hw_params, state->format);
    if (err < 0) {
        fprintf(stderr, "Cannot set format: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Set channels
    err = snd_pcm_hw_params_set_channels(state->pcm_handle, hw_params, state->channels);
    if (err < 0) {
        fprintf(stderr, "Cannot set channels: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Set sample rate
    unsigned int exact_rate = state->sample_rate;
    err = snd_pcm_hw_params_set_rate_near(state->pcm_handle, hw_params, &exact_rate, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    if (exact_rate != state->sample_rate) {
        fprintf(stderr, "Warning: sample rate changed from %d to %d\n",
                state->sample_rate, exact_rate);
        state->sample_rate = exact_rate;
    }

    // Set period size (equivalent to buffer size in the macOS version)
    state->period_size = state->sample_rate / 10;  // 100ms buffer like in macOS example
    err = snd_pcm_hw_params_set_period_size_near(state->pcm_handle, hw_params,
                                               &state->period_size, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot set period size: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Set buffer size (in periods)
    snd_pcm_uframes_t buffer_size = state->period_size * BUFFERS_COUNT;
    err = snd_pcm_hw_params_set_buffer_size_near(state->pcm_handle, hw_params, &buffer_size);
    if (err < 0) {
        fprintf(stderr, "Cannot set buffer size: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Apply hardware parameters
    err = snd_pcm_hw_params(state->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Prepare PCM device
    err = snd_pcm_prepare(state->pcm_handle);
    if (err < 0) {
        fprintf(stderr, "Cannot prepare audio interface: %s\n", snd_strerror(err));
        audio_cleanup(state);
        return NULL;
    }

    // Start playback thread
    state->thread_running = 1;
    err = pthread_create(&state->playback_thread, NULL, playback_thread_func, state);
    if (err != 0) {
        fprintf(stderr, "Cannot create playback thread: %d\n", err);
        audio_cleanup(state);
        return NULL;
    }

    printf("ALSA audio initialized successfully\n"); fflush(stdout);
    return (void*)state;
}

#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BUFFERS_COUNT 3
#define MAX_C64_BUFFER_LEN (1024*64)

// Our audio state is encapsulated here.
typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[BUFFERS_COUNT];
    UInt32 bufferSize;

    // Samples arriving from the emulator.
    short *c64_buffer;
    size_t c64_buffer_len;
} AudioState;

// Callback function for Audio Queue Services
static void BufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    AudioState *state = (AudioState *)inUserData;

    // Fill the buffer with sine wave samples
    short *samples = (short *)inBuffer->mAudioData;
    size_t numSamples = inBuffer->mAudioDataBytesCapacity / sizeof(short);

    if (state->c64_buffer_len == 0) {
        printf("."); fflush(stdout);
        for (size_t i = 0; i < numSamples; i++)
            samples[i] = 0;
    } else {
        if (numSamples > state->c64_buffer_len)
            numSamples = state->c64_buffer_len;
        for (size_t i = 0; i < numSamples; i++)
            samples[i] = state->c64_buffer[i];
        if (numSamples < state->c64_buffer_len) {
            memmove(state->c64_buffer, state->c64_buffer+numSamples,
                sizeof(short) * (state->c64_buffer_len - numSamples));
        }
        state->c64_buffer_len -= numSamples;
    }

    // Mark the buffer as filled and enqueue it again.
    inBuffer->mAudioDataByteSize = numSamples * sizeof(short);
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

/* This function receive samples from the emulator. It will
 * feed the buffer that will later be used in order to provide
 * samples to the core audio buffers. */
void audio_from_emulator(const float *samples, int num_samples, void *user_data){
    AudioState *state = user_data;
    if (state->c64_buffer_len >= MAX_C64_BUFFER_LEN) {
        printf("!"); fflush(stdout);
        return;
    }
    state->c64_buffer = realloc(state->c64_buffer, sizeof(short)*(state->c64_buffer_len + num_samples));
    for (int j = 0; j < num_samples; j++) {
        state->c64_buffer[state->c64_buffer_len] = (short) (samples[j]*32367);
        state->c64_buffer_len++;
    }
}

void *audio_init(void) {
    // Initialize the audio state
    AudioState *state = malloc(sizeof(AudioState));
    if (state == NULL) return NULL;
    memset(state,0,sizeof(*state));

    // Set up the audio format
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 44100;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = 1;  // Mono
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBitsPerChannel / 8 * format.mChannelsPerFrame;
    format.mBytesPerPacket = format.mBytesPerFrame * format.mFramesPerPacket;

    // Create a new audio queue for playback
    OSStatus status = AudioQueueNewOutput(&format, BufferCallback, state, NULL, NULL, 0, &state->queue);
    if (status) {
        fprintf(stderr, "Error creating audio queue: %d\n", (int)status);
        return NULL;
    }

    state->bufferSize = (44100/10) * sizeof(short);

    // Allocate and prime audio buffers
    for (int i = 0; i < BUFFERS_COUNT; i++) {
        status = AudioQueueAllocateBuffer(state->queue, state->bufferSize, &state->buffers[i]);
        if (status) {
            fprintf(stderr, "Error allocating buffer %d: %d\n", i, (int)status);
            return NULL;
        }

        // Prime the buffer by calling the callback directly: in the
        // real-time audio case we don't have anything to prime with: either
        // zero the buffer for silence, or start the audio once we have some
        // data.
        BufferCallback(state, state->queue, state->buffers[i]);
    }

    // Start the audio queue
    status = AudioQueueStart(state->queue, NULL);
    if (status) {
        fprintf(stderr, "Error starting audio queue: %d\n", (int)status);
        return NULL;
    }
    return (void*)state;
}

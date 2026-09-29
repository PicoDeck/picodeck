// HAL Audio - SDL2 Audio Implementation (Stub)

#include "hal_audio.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

// Audio configuration
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_SIZE 4096

static SDL_AudioDeviceID s_audio_dev = 0;

bool hal_audio_init(void) {
    SDL_AudioSpec want, have;

    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = 512;
    want.callback = NULL;  // We'll use SDL_QueueAudio

    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_audio_dev == 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
        return false;
    }

    SDL_PauseAudioDevice(s_audio_dev, 0);  // Start audio
    printf("[Audio] Initialized (%d Hz, %d channels)\n", have.freq, have.channels);
    return true;
}

void hal_audio_shutdown(void) {
    if (s_audio_dev) {
        SDL_CloseAudioDevice(s_audio_dev);
        s_audio_dev = 0;
        printf("[Audio] Shutdown\n");
    }
}

// hal_audio_update() is implemented in sim_audio.c — it drives tone
// generation, sound sample mixing, fileplayer streaming, and MP3 decode.

int hal_audio_push_samples(const int16_t* samples, int count) {
    if (!s_audio_dev || !samples || count <= 0) return 0;
    int bytes = count * sizeof(int16_t) * AUDIO_CHANNELS;
    return SDL_QueueAudio(s_audio_dev, samples, bytes) == 0 ? count : 0;
}

int hal_audio_buffer_space(void) {
    if (!s_audio_dev) return 0;

    // Return approximate queue space
    return AUDIO_BUFFER_SIZE - SDL_GetQueuedAudioSize(s_audio_dev) / sizeof(int16_t);
}

// Frames SDL has queued and not yet played.
uint32_t hal_audio_queued_frames(void) {
    if (!s_audio_dev) return 0;
    return SDL_GetQueuedAudioSize(s_audio_dev) / (sizeof(int16_t) * AUDIO_CHANNELS);
}

// HAL Audio - SDL2 Audio Implementation

#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Initialize audio subsystem
bool hal_audio_init(void);

// Shutdown audio subsystem
void hal_audio_shutdown(void);

// Update audio (call regularly from Core 1)
void hal_audio_update(void);

// Push samples to audio buffer
int hal_audio_push_samples(const int16_t* samples, int count);

// Get audio buffer space available
int hal_audio_buffer_space(void);

// Frames SDL has queued and not yet played.
uint32_t hal_audio_queued_frames(void);

#endif // HAL_AUDIO_H

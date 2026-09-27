#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pico/time.h"
#include "../os/os.h"

void audio_init(void);

/* Fixed ultrasonic output rate: everything (PCM stream, sound samples,
 * tone synth) is mixed into a single stream running at this rate, so the
 * PWM pulse repetition frequency is always inaudible (was: content rate —
 * 11025 Hz content whistled at 11 kHz, 22050 Hz content at 22 kHz). */
#define AUDIO_OUT_RATE 44100

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void audio_stop_tone(void);
void audio_set_volume(uint8_t volume);

// Starts the PCM stream (fixed AUDIO_OUT_RATE) only if not already running.
// Called by tone/sample playback so they mix in without disturbing an
// active stream (e.g. BGM fileplayer).
void audio_stream_ensure_running(void);

// Must be called from Core 1 before any audio playback.
// Creates an alarm pool whose timer ISRs fire on Core 1, keeping
// Core 0 free for the app/game loop.
void audio_core1_init(void);

// Returns the Core 1 alarm pool for audio timer registration.
// Used by sound.c and fileplayer.c to ensure their playback timers
// also fire on Core 1.
alarm_pool_t *audio_get_core1_alarm_pool(void);

// PCM sample streaming (for emulators, music players, etc.)
// Uses DMA paced by PWM DREQ — ~750x fewer interrupts than the old timer ISR.
// Samples are stereo interleaved int16_t pairs (L, R, L, R, ...).
// count = number of stereo frames (each frame = 2 int16_t values).
void audio_start_stream(uint32_t sample_rate);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);

// Must be called periodically from Core 1 to handle deferred DMA start.
// This ensures the stream DMA ISR fires on Core 1, not Core 0.
void audio_stream_poll(void);

// Debug: get DMA ISR count, underrun count, and ring buffer fill level.
void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used);

// Returns number of free sample slots in the ring buffer.
// Callers should check this before rendering audio to avoid overflow
// (overflow drops samples, causing severe audio corruption).
uint32_t audio_ring_free(void);

// The output's DMA refill interrupt, for the `audiostat` dev command.
typedef struct {
    bool running;         // the output is started
    uint32_t isr_count;   // refills since the last reset
    uint32_t isr_us;      // time spent in them since the last reset
    uint32_t isr_max_us;  // the longest one since the last reset
} audio_output_stats_t;

void audio_output_get_stats(audio_output_stats_t *out);
void audio_output_reset_stats(void);
// Zeroes the stream underrun count that audio_stream_debug reports.
void audio_stream_reset_underruns(void);

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pico/time.h"
#include "../os/os.h"

/* The one output rate: every source is mixed into a single stream at this
 * rate (audio_mix.c), so the PWM pulse repetition frequency is always
 * inaudible (it used to be the content rate: 11025 Hz content whistled at
 * 11 kHz). */
#define AUDIO_OUT_RATE 44100

// ── Boot and Core 1 ─────────────────────────────────────────────────────────
void audio_init(void);
// Must be called from Core 1 before any audio playback: creates an alarm
// pool whose timer ISRs fire on Core 1 (Core 1's tick timer uses it).
void audio_core1_init(void);
alarm_pool_t *audio_get_core1_alarm_pool(void);
// Core 1's tick: registers the output's refill ISR on Core 1 and starts a
// pending output (so the ISR never lands on Core 0).
void audio_stream_poll(void);

// ── Sources (audio_mix.c; the simulator runs the same code) ─────────────────
// Every source starts the output if it is off, and they all play at once.
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);  // 0 ms: until stopped
void audio_stop_tone(void);
// Master, 0-100, scales everything; app teardowns reset it to 100.
void audio_set_volume(uint8_t volume);

// The PCM stream (fileplayer, MOD, the native/Lua stream API): stereo
// interleaved int16 frames at the rate given to audio_start_stream.
// Starting empties the ring; stopping stops only the stream: the output
// and the other sources play on.
void audio_start_stream(uint32_t sample_rate);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);  // count = frames
// Free frames in the ring: check before rendering, overflow is dropped.
uint32_t audio_ring_free(void);
void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used);
void audio_stream_reset_underruns(void);

// ── The output (audio.c on the device, simulator/sim_audio.c) ───────────────
// Runs from the first sound until the app's teardown stops it.
void audio_output_ensure_running(void);
void audio_output_stop(void);
bool audio_output_running(void);
uint32_t audio_output_isr_count(void);  // refills since boot
// Re-derive the PWM divider after a clock change (launcher_apply_clock).
void audio_apply_clock(void);

// The output's DMA refill interrupt, for the `audiostat` dev command.
typedef struct {
    bool running;         // the output is started
    uint32_t isr_count;   // refills since the last reset
    uint32_t isr_us;      // time spent in them since the last reset
    uint32_t isr_max_us;  // the longest one since the last reset
} audio_output_stats_t;

void audio_output_get_stats(audio_output_stats_t *out);
void audio_output_reset_stats(void);

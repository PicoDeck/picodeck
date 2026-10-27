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
// and the other sources play on. A started stream plays from its first
// frame pushed: the empty ring before it is not an underrun.
void audio_start_stream(uint32_t sample_rate);
// As audio_start_stream, but the stream takes pushes and plays nothing
// until audio_stream_release(): a producer that fills the ring first (the
// fileplayer) never starts on an almost empty ring that one slow SD read
// right after the start (Core 0 loading a sample, say) would starve.
void audio_start_stream_held(uint32_t sample_rate);
// A held stream plays from its next render chunk (from its first frame,
// if the ring is still empty then). No effect on a stream that plays.
void audio_stream_release(void);
// Stops taking frames and keeps the ones in the ring (a paused
// fileplayer); audio_stream_release() plays on from the same frame.
void audio_stream_hold(void);
// While the stream is held, empties the ring: its producer moved (a
// paused or still-filling fileplayer's seek), so the release plays the new
// position. No effect on a stream that plays, or one another producer
// restarted since (audio_start_stream does not hold).
void audio_stream_flush_held(void);
// The producer has no more data (a fileplayer that finished): the ring
// plays out (a held stream is released), and its running dry is the end,
// not an underrun. The next start counts underruns again.
void audio_stream_drain(void);
// The producer's data wrapped to its loop start after the frames pushed so
// far (`audiostat` tells underruns just after a loop point apart).
void audio_stream_mark_loop(void);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);  // count = frames
// Free frames in the ring: check before rendering, overflow is dropped.
// 0 while the stream is stopped (pushes are dropped then), so a producer
// that paces on it waits until its stream is started again.
uint32_t audio_ring_free(void);
void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used);
// Starts a new window for the underrun count and audio_stream_get_stats.
void audio_stream_reset_underruns(void);

// When the stream ran dry, for `audiostat` (since the last reset). An
// underrun is an output frame the render found the ring empty for while
// the stream played and its producer still had data to come; a gap is a
// run of render chunks (32 frames) with underruns. Positions are ms into
// the stream's own content (its playback, from the start or release), at
// the rate of the stream that ran dry.
typedef struct {
    uint32_t underruns;        // output frames (44.1 kHz)
    uint32_t start_underruns;  // of those, in a stream's first second
    uint32_t loop_underruns;   // in the second after a loop point (not the start's)
    uint32_t gaps;             // runs of starved chunks
    int32_t first_ms;          // where the first gap began (-1: none)
    int32_t last_ms;           // where the last gap began (-1: none)
    int32_t low_ms;            // the ring's lowest fill while playing (-1: none)
    uint32_t starts;           // streams started
    uint32_t loops;            // loop points marked (audio_stream_mark_loop)
} audio_stream_stats_t;
void audio_stream_get_stats(audio_stream_stats_t *out);

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

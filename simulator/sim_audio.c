// Simulator audio output: stands in for src/drivers/audio.c's PWM and DMA.
// Everything else is the firmware's own code: the mixer, its stream and
// tone (src/drivers/audio_mix.c), the sample players (sound.c), the MP3
// player (mp3_player.c), the WAV streamer (fileplayer.c) and the MOD player
// (mod_player.c). sim_output_render() does the DMA refill ISR's part: on
// the Core 1 thread it renders the mix at AUDIO_OUT_RATE by wall clock and
// queues it to SDL, so flow control, sample timing and callbacks, MP3
// underruns and fades behave as on the device. Timing costs do not:
// measure those on hardware (tests/e2e/test_audio_hw.py).

#include "hal/hal_audio.h"
#include "drivers/audio.h"
#include "drivers/audio_mix.h"
#include "drivers/fileplayer.h"
#include "drivers/mp3_player.h"
#include "drivers/sound.h"

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

// Frames SDL may hold before a render is dropped instead of queued: a slow
// host must not build up seconds of latency (the sources advance anyway).
#define SIM_MAX_QUEUED_FRAMES (AUDIO_OUT_RATE / 5)
// The longest catch-up after a stalled thread: beyond it the frames are
// lost, as a late DMA refill would lose them.
#define SIM_MAX_RENDER_FRAMES (AUDIO_OUT_RATE / 10)
#define SIM_RENDER_CHUNK 256

static atomic_bool s_output_on;
static atomic_uint s_render_calls;

// audio_output_get_stats (`audiostat`): the render passes standing in for
// the refill interrupt (one per Core 1 tick that rendered), as audio.c
// counts its refills. The render thread carries out a reset (Core 0 only
// raises the flag), as on the device.
static atomic_uint s_pass_count, s_pass_us, s_pass_max_us;
static atomic_bool s_pass_reset_req;

static uint64_t sim_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void audio_init(void) {
    audio_mix_init();
}

void audio_core1_init(void) {}

alarm_pool_t *audio_get_core1_alarm_pool(void) {
    return NULL;
}

void audio_stream_poll(void) {}

void audio_apply_clock(void) {}

void audio_output_ensure_running(void) {
    atomic_store(&s_output_on, true);
}

void audio_output_stop(void) {
    atomic_store(&s_output_on, false);
}

bool audio_output_running(void) {
    return atomic_load(&s_output_on);
}

uint32_t audio_output_isr_count(void) {
    return atomic_load(&s_render_calls);
}

void audio_output_get_stats(audio_output_stats_t *out) {
    bool reset = atomic_load(&s_pass_reset_req);
    out->running = atomic_load(&s_output_on);
    out->isr_count = reset ? 0 : atomic_load(&s_pass_count);
    out->isr_us = reset ? 0 : atomic_load(&s_pass_us);
    out->isr_max_us = reset ? 0 : atomic_load(&s_pass_max_us);
}

void audio_output_reset_stats(void) {
    atomic_store(&s_pass_reset_req, true);
}

// The DMA refill ISR's part, on the Core 1 thread every tick: render the
// frames the output rate has consumed since the last call.
static void sim_output_render(void) {
    static bool s_was_on;
    static uint64_t s_last_us, s_rem;  // s_rem: sub-frame remainder, in frame-microseconds
    if (atomic_exchange(&s_pass_reset_req, false)) {
        atomic_store(&s_pass_count, 0);
        atomic_store(&s_pass_us, 0);
        atomic_store(&s_pass_max_us, 0);
    }
    uint64_t now = sim_time_us();
    if (!atomic_load(&s_output_on)) {
        s_was_on = false;
        return;
    }
    if (!s_was_on) {  // just started: its clock starts now
        s_was_on = true;
        s_last_us = now;
        s_rem = 0;
        return;
    }
    uint64_t due = (now - s_last_us) * AUDIO_OUT_RATE + s_rem;
    s_last_us = now;
    s_rem = due % 1000000u;
    uint64_t frames = due / 1000000u;
    if (frames > SIM_MAX_RENDER_FRAMES)
        frames = SIM_MAX_RENDER_FRAMES;
    int16_t buf[2 * SIM_RENDER_CHUNK];
    bool rendered = frames > 0;
    while (frames > 0) {
        int n = frames > SIM_RENDER_CHUNK ? SIM_RENDER_CHUNK : (int)frames;
        audio_mix_render(buf, n);
        atomic_fetch_add(&s_render_calls, 1);
        if (hal_audio_queued_frames() < SIM_MAX_QUEUED_FRAMES)
            hal_audio_push_samples(buf, n);
        frames -= (uint64_t)n;
    }
    if (rendered) {
        uint32_t dt = (uint32_t)(sim_time_us() - now);
        atomic_fetch_add(&s_pass_count, 1);
        atomic_fetch_add(&s_pass_us, dt);
        if (dt > atomic_load(&s_pass_max_us))
            atomic_store(&s_pass_max_us, dt);
    }
}

// Core 1's audio work in src/main.c's tick order (mod_player_update follows
// in the simulator's Core 1 loop).
void hal_audio_update(void) {
    mp3_player_update();
    fileplayer_update();
    sound_pump_callbacks();
    sim_output_render();
}

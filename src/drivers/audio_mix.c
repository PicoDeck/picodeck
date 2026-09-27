#include "audio_mix.h"
#include "audio.h"
#include "audio_ring.h"
#include "sound.h"
#include "pico/platform.h"
#include "pico/critical_section.h"

// ── audio_mix.c's own sources: the stream and the tone ──────────────────────
// s_src_cs guards audio_mix.c's own sources: the stream (starting/stopping
// it: the ring clear, the rate, s_stream_on) and the tone (all of its
// state), against the render, which holds it for one chunk. Pushes stay
// lock-free: audio_ring.h is single-producer single-consumer, and each
// producer starts and stops its own stream (fileplayer and MOD under their
// own locks, the stream API on Core 0).
static critical_section_t s_src_cs;
static bool s_src_cs_ready;

static inline void src_lock(void) {
  if (s_src_cs_ready)
    critical_section_enter_blocking(&s_src_cs);
}

static inline void src_unlock(void) {
  if (s_src_cs_ready)
    critical_section_exit(&s_src_cs);
}

// ── The PCM stream ──────────────────────────────────────────────────────────
static audio_ring_t s_ring;
static uint32_t s_stream_rate = AUDIO_OUT_RATE;
static volatile bool s_stream_on;
static volatile uint32_t s_stream_underruns;

// ── The tone: a square wave, timed in mixed frames ──────────────────────────
#define TONE_MIN_HZ 20
#define TONE_MAX_HZ 20000

static volatile bool s_tone_on;
static volatile bool s_tone_timed;          // false: until audio_stop_tone
static volatile uint32_t s_tone_frames_left;
static volatile uint32_t s_tone_hz = 440;
static uint32_t s_tone_phase;
static volatile int32_t s_tone_level;       // 0..128: +-(level * 128)

// ── Master volume ───────────────────────────────────────────────────────────
static volatile uint8_t s_volume = 100;
static volatile uint32_t s_volume_scale = 256;  // 256 = 100%

// Logarithmic tone level: lut[i] = round((10^(i/100) - 1) / 9 * 128).
static const uint8_t s_log_volume_lut[101] = {
    0,   0,   1,   1,   1,   2,   2,   2,   3,   3,   //  0-  9
    4,   4,   5,   5,   5,   6,   6,   7,   7,   8,   // 10- 19
    8,   9,   9,  10,  10,  11,  12,  12,  13,  14,   // 20- 29
   14,  15,  15,  16,  17,  18,  18,  19,  20,  21,   // 30- 39
   22,  22,  23,  24,  25,  26,  27,  28,  29,  30,   // 40- 49
   31,  32,  33,  34,  35,  36,  37,  39,  40,  41,   // 50- 59
   42,  44,  45,  46,  48,  49,  51,  52,  54,  55,   // 60- 69
   57,  59,  60,  62,  64,  66,  68,  70,  71,  73,   // 70- 79
   76,  78,  80,  82,  84,  86,  89,  91,  94,  96,   // 80- 89
   99, 101, 104, 107, 110, 113, 115, 119, 122, 125,   // 90- 99
  128                                                   // 100
};

// The mix accumulates in chunks of MIX_CHUNK frames (static: one render at
// a time, and SRAM is nearly full).
#define MIX_CHUNK 32
static int32_t s_mix_l[MIX_CHUNK];
static int32_t s_mix_r[MIX_CHUNK];

void audio_mix_init(void) {
  if (!s_src_cs_ready) {
    critical_section_init_with_lock_num(&s_src_cs,
                                        next_striped_spin_lock_num());
    s_src_cs_ready = true;
  }
}

void __time_critical_func(audio_mix_render)(int16_t *lr, int frames) {
  uint32_t vol = s_volume_scale;
  for (int base = 0; base < frames; base += MIX_CHUNK) {
    int n = frames - base < MIX_CHUNK ? frames - base : MIX_CHUNK;

    // The sample players write the chunk (zeros when none plays).
    sound_mixer_process(s_mix_l, s_mix_r, n);

    // + the PCM stream, + the tone: both audio_mix.c's own sources, held
    // under the same lock for the whole chunk so a play/stop/volume call
    // on Core 0 never lands between reading and writing either one's state.
    src_lock();
    if (s_stream_on) {
      for (int i = 0; i < n; i++) {
        int32_t sl, sr;
        if (!audio_ring_pop(&s_ring, s_stream_rate, AUDIO_OUT_RATE, &sl, &sr))
          s_stream_underruns++;
        s_mix_l[i] += sl;
        s_mix_r[i] += sr;
      }
    }
    if (s_tone_on) {
      uint32_t phase = s_tone_phase, hz = s_tone_hz;
      int32_t level = s_tone_level;
      bool timed = s_tone_timed;
      uint32_t left = s_tone_frames_left;
      for (int i = 0; i < n; i++) {
        phase += hz;
        if (phase >= AUDIO_OUT_RATE)
          phase -= AUDIO_OUT_RATE;
        int32_t t = phase < AUDIO_OUT_RATE / 2 ? level : -level;
        s_mix_l[i] += t * 128;
        s_mix_r[i] += t * 128;
        // left == 0 first: a duration that already rounded to 0 frames
        // must stop here, never decrement-and-wrap past it.
        if (timed && (left == 0 || --left == 0)) {
          s_tone_on = false;
          break;
        }
      }
      s_tone_phase = phase;
      s_tone_frames_left = left;
    }
    src_unlock();

    // Clip, then the master volume, which scales the signed mix (about
    // zero: scaling the PWM level pulled silence toward 0, a pop on every
    // volume change).
    for (int i = 0; i < n; i++) {
      int32_t ml = s_mix_l[i], mr = s_mix_r[i];
      if (ml > 32767) ml = 32767; else if (ml < -32768) ml = -32768;
      if (mr > 32767) mr = 32767; else if (mr < -32768) mr = -32768;
      lr[2 * (base + i)] = (int16_t)(ml * (int32_t)vol / 256);
      lr[2 * (base + i) + 1] = (int16_t)(mr * (int32_t)vol / 256);
    }
  }
}

// ── Tone ────────────────────────────────────────────────────────────────────

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
  if (freq_hz < TONE_MIN_HZ)
    freq_hz = TONE_MIN_HZ;
  if (freq_hz > TONE_MAX_HZ)
    freq_hz = TONE_MAX_HZ;
  src_lock();
  s_tone_on = false;
  s_tone_hz = freq_hz;
  s_tone_phase = 0;
  uint64_t frames = (uint64_t)duration_ms * AUDIO_OUT_RATE / 1000;
  s_tone_frames_left = frames > UINT32_MAX ? UINT32_MAX : (uint32_t)frames;
  s_tone_timed = duration_ms > 0;
  s_tone_level = s_log_volume_lut[s_volume];
  s_tone_on = true;
  src_unlock();
  // Never called while holding the lock: it may end up calling back into
  // audio.c, which must not block on a lock the render also takes.
  audio_output_ensure_running();
}

void audio_stop_tone(void) {
  src_lock();
  s_tone_on = false;
  s_tone_frames_left = 0;
  src_unlock();
}

bool audio_tone_playing(void) {
  return s_tone_on;
}

void audio_set_volume(uint8_t volume) {
  if (volume > 100)
    volume = 100;
  s_volume = volume;
  s_volume_scale = (uint32_t)volume * 256 / 100;
  src_lock();
  s_tone_level = s_log_volume_lut[volume];
  src_unlock();
}

// ── The PCM stream ──────────────────────────────────────────────────────────

void audio_start_stream(uint32_t sample_rate) {
  src_lock();
  audio_ring_clear(&s_ring);
  s_ring.phase = 0;
  s_stream_rate = sample_rate ? sample_rate : AUDIO_OUT_RATE;
  s_stream_on = true;
  src_unlock();
  // Never called while holding the lock: see audio_play_tone.
  audio_output_ensure_running();
}

void audio_stop_stream(void) {
  src_lock();
  s_stream_on = false;
  audio_ring_clear(&s_ring);
  src_unlock();
}

bool audio_stream_active(void) {
  return s_stream_on;
}

void audio_push_samples(const int16_t *samples, int count) {
  if (!s_stream_on || !samples || count <= 0)
    return;
  audio_ring_push(&s_ring, samples, count);  // drops what doesn't fit
}

uint32_t audio_ring_free(void) {
  return audio_ring_space(&s_ring);
}

void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns,
                        uint32_t *ring_used) {
  if (isr_count)
    *isr_count = audio_output_isr_count();
  if (underruns)
    *underruns = s_stream_underruns;
  if (ring_used)
    *ring_used = audio_ring_used(&s_ring);
}

void audio_stream_reset_underruns(void) {
  s_stream_underruns = 0;
}

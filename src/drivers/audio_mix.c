#include "audio_mix.h"
#include "audio.h"
#include "audio_ring.h"
#include "mp3_player.h"
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

// Forced inline: the render (RAM-resident, in the refill ISR) must not
// reach them through a flash veneer.
static __force_inline void src_lock(void) {
  if (s_src_cs_ready)
    critical_section_enter_blocking(&s_src_cs);
}

static __force_inline void src_unlock(void) {
  if (s_src_cs_ready)
    critical_section_exit(&s_src_cs);
}

// ── The PCM stream ──────────────────────────────────────────────────────────
// s_stream_on: started (pushes land in the ring). s_stream_live: the render
// takes frames; it goes live at a chunk that finds the ring non-empty
// unless held. s_stream_held: waits for audio_stream_release (the
// fileplayer's start and pause). s_stream_ended: the producer has no more
// data, so an empty ring is the end, not an underrun.
static audio_ring_t s_ring;
static uint32_t s_stream_rate = AUDIO_OUT_RATE;
static volatile bool s_stream_on;
static volatile bool s_stream_live;
static volatile bool s_stream_held;
static volatile bool s_stream_ended;
static volatile uint32_t s_stream_underruns;

// When the stream ran dry (audio_stream_get_stats, `audiostat`). The
// render writes them (a starved chunk through stream_note_starved, in
// flash; any other a store and the low-water compare); Core 0 resets
// them, all under s_src_cs.
// Positions are the ring's read counter: content frames played since the
// stream started (audio_ring_clear zeroes it), converted to ms on Core 0.
// One struct, so the RAM-resident render reaches every field from one
// base address (SRAM is nearly full).
#define DIAG_NONE UINT32_MAX
typedef struct {
  uint32_t start_u, loop_u, gaps;
  uint32_t first_pos, first_rate;  // first_pos DIAG_NONE: none yet
  uint32_t last_pos, last_rate;
  uint32_t low, low_rate;          // low DIAG_NONE: none yet
  uint32_t starts, loops;
  uint32_t loop_mark;              // write counter at the last loop point
  bool loop_marked;
  bool in_gap;                     // the last chunk starved
} stream_diag_t;
static stream_diag_t s_diag = {.first_pos = DIAG_NONE, .last_pos = DIAG_NONE,
                               .low = DIAG_NONE};

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

// A live chunk the ring ran dry for (s_src_cs held): count its `empty`
// frames and note when the gap began and near what. The start window is
// the stream's first second of content, the loop window the second after
// the last loop point (the read counter has passed it).
// In flash, not inlined: only a starved chunk calls it, the slow path (an
// audible gap already), so the RAM-resident render keeps only the call
// (SRAM is nearly full). The render reads QMI PSRAM (sample data) anyway,
// and nothing writes the flash while the output runs (OTA writes it at
// boot, before Core 1 starts).
static __attribute__((noinline)) void stream_note_starved(uint32_t empty) {
  uint32_t rd = s_ring.read, rate = s_stream_rate;
  s_stream_underruns += empty;
  if (rd < rate)
    s_diag.start_u += empty;
  else if (s_diag.loop_marked && rd - s_diag.loop_mark < rate)
    s_diag.loop_u += empty;
  if (!s_diag.in_gap) {
    s_diag.in_gap = true;
    s_diag.gaps++;
    if (s_diag.first_pos == DIAG_NONE) {
      s_diag.first_pos = rd;
      s_diag.first_rate = rate;
    }
    s_diag.last_pos = rd;
    s_diag.last_rate = rate;
  }
}

void __time_critical_func(audio_mix_render)(int16_t *lr, int frames) {
  uint32_t vol = s_volume_scale;
  for (int base = 0; base < frames; base += MIX_CHUNK) {
    int n = frames - base < MIX_CHUNK ? frames - base : MIX_CHUNK;

    // The sample players write the chunk (zeros when none plays).
    sound_mixer_process(s_mix_l, s_mix_r, n);

    // + the MP3 player
    mp3_player_mix(s_mix_l, s_mix_r, n);

    // + the PCM stream, + the tone: both audio_mix.c's own sources, held
    // under the same lock for the whole chunk so a play/stop/volume call
    // on Core 0 never lands between reading and writing either one's state.
    src_lock();
    if (s_stream_on) {
      // An unheld stream begins at its first frame.
      if (!s_stream_live && !s_stream_held && s_ring.write != s_ring.read)
        s_stream_live = true;
      if (s_stream_live) {
        uint32_t empty = 0;
        for (int i = 0; i < n; i++) {
          int32_t sl, sr;
          if (!audio_ring_pop(&s_ring, s_stream_rate, AUDIO_OUT_RATE, &sl, &sr))
            empty++;
          s_mix_l[i] += sl;
          s_mix_r[i] += sr;
        }
        // audiostat: a starved chunk calls out (above); any other costs a
        // store and the ring's low-water compare.
        if (!s_stream_ended) {
          if (empty)
            stream_note_starved(empty);
          else
            s_diag.in_gap = false;
          uint32_t used = s_ring.write - s_ring.read;
          if (used < s_diag.low) {
            s_diag.low = used;
            s_diag.low_rate = s_stream_rate;
          }
        }
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
        // left == 0 first is defensive (a timed tone starts with at least
        // 44 frames: 1 ms): never decrement-and-wrap past it.
        if (timed && (left == 0 || --left == 0)) {
          s_tone_on = false;
          break;
        }
      }
      s_tone_phase = phase;
      s_tone_frames_left = left;
    }
    src_unlock();

    // The master volume, then the clip: turned down, a sum over full scale
    // comes out quieter and undistorted, not a quieter clipped one. The
    // volume scales the signed mix (about zero: scaling the PWM level
    // pulled silence toward 0, a pop on every volume change).
    for (int i = 0; i < n; i++) {
      int32_t ml = s_mix_l[i] * (int32_t)vol / 256;
      int32_t mr = s_mix_r[i] * (int32_t)vol / 256;
      if (ml > 32767) ml = 32767; else if (ml < -32768) ml = -32768;
      if (mr > 32767) mr = 32767; else if (mr < -32768) mr = -32768;
      lr[2 * (base + i)] = (int16_t)ml;
      lr[2 * (base + i) + 1] = (int16_t)mr;
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
  // Called after unlocking: starting the output is slow hardware setup
  // (GPIO, PWM and DMA configuration), and its dma_claim_unused_channel
  // takes the hardware-claim spin lock, which must not nest inside s_src_cs.
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

uint8_t audio_get_volume(void) {
  return s_volume;
}

// ── The PCM stream ──────────────────────────────────────────────────────────

static void start_stream(uint32_t sample_rate, bool held) {
  src_lock();
  audio_ring_clear(&s_ring);
  s_ring.phase = 0;
  s_stream_rate = sample_rate ? sample_rate : AUDIO_OUT_RATE;
  s_stream_live = false;
  s_stream_held = held;
  s_stream_ended = false;
  s_diag.loop_marked = false;
  s_diag.in_gap = false;
  s_diag.starts++;
  s_stream_on = true;
  src_unlock();
  // Never called while holding the lock: see audio_play_tone.
  audio_output_ensure_running();
}

void audio_start_stream(uint32_t sample_rate) {
  start_stream(sample_rate, false);
}

void audio_start_stream_held(uint32_t sample_rate) {
  start_stream(sample_rate, true);
}

// The fileplayer calls this on every tick that finds the ring full: no
// lock unless there is something to release.
void audio_stream_release(void) {
  if (!s_stream_held)
    return;
  src_lock();
  s_stream_held = false;
  src_unlock();
}

void audio_stream_hold(void) {
  src_lock();
  s_stream_held = true;
  s_stream_live = false;
  s_diag.in_gap = false;
  src_unlock();
}

// Held, nothing in the ring has played since the producer's start or
// pause: dropping it loses nothing anyone heard. The read counter restarts
// with the ring, as at a start (the start window counts from here).
void audio_stream_flush_held(void) {
  src_lock();
  if (s_stream_on && s_stream_held) {
    audio_ring_clear(&s_ring);
    s_ring.phase = 0;
    s_diag.loop_marked = false;  // its write count was the old ring's
  }
  src_unlock();
}

void audio_stream_drain(void) {
  src_lock();
  s_stream_ended = true;
  s_stream_held = false;
  src_unlock();
}

void audio_stream_mark_loop(void) {
  src_lock();
  s_diag.loop_mark = s_ring.write;
  s_diag.loop_marked = true;
  s_diag.loops++;
  src_unlock();
}

void audio_stop_stream(void) {
  src_lock();
  s_stream_on = false;
  s_stream_live = false;
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

// 0 while the stream is off: pushes are dropped then, and the producers
// that pace on this (fileplayer, MOD) must wait, not read or render flat out.
uint32_t audio_ring_free(void) {
  return s_stream_on ? audio_ring_space(&s_ring) : 0;
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

// Under the lock: the render's s_stream_underruns++ is a read-modify-write
// that would put back a count read before the reset.
void audio_stream_reset_underruns(void) {
  src_lock();
  s_stream_underruns = 0;
  s_diag.start_u = s_diag.loop_u = s_diag.gaps = 0;
  s_diag.first_pos = s_diag.last_pos = s_diag.low = DIAG_NONE;
  s_diag.starts = s_diag.loops = 0;
  src_unlock();
}

// Content frames at `rate` as ms (-1 for none).
static int32_t pos_ms(uint32_t pos, uint32_t rate) {
  if (pos == DIAG_NONE || rate == 0)
    return -1;
  return (int32_t)((uint64_t)pos * 1000u / rate);
}

void audio_stream_get_stats(audio_stream_stats_t *out) {
  src_lock();
  out->underruns = s_stream_underruns;
  out->start_underruns = s_diag.start_u;
  out->loop_underruns = s_diag.loop_u;
  out->gaps = s_diag.gaps;
  uint32_t first = s_diag.first_pos, first_rate = s_diag.first_rate;
  uint32_t last = s_diag.last_pos, last_rate = s_diag.last_rate;
  uint32_t low = s_diag.low, low_rate = s_diag.low_rate;
  out->starts = s_diag.starts;
  out->loops = s_diag.loops;
  src_unlock();
  // The divisions outside the lock: the render waits on it.
  out->first_ms = pos_ms(first, first_rate);
  out->last_ms = pos_ms(last, last_rate);
  out->low_ms = pos_ms(low, low_rate);
}

// Host unit tests for mp3_sched.h: when Core 1's MP3 decoder decodes
// (issue #28: decoding contends with Core 0 for the XIP cache and the QMI,
// so a paced app's idle end-of-frame wait is the time to do it).
//
// The decision functions first, then a model of the whole pipeline over
// minutes of audio: Core 1's 1 ms update decoding into the 32 KB PCM ring
// (mp3_player.c's decode_fill_ring loop, same order of checks), the 8 KB
// stage refilled from the ring, the mixer draining the stage at the
// content's rate, and Core 0 paced at 30 fps (an idle window at the end of
// each frame) or unpaced (never idle). The decoder's unit is a granule (576
// samples a channel: half an MPEG-1 frame, a whole MPEG-2 one); a unit
// decodes at one speed while Core 0 is idle and at half that while it
// works (they contend). The model counts underruns (output frames that
// found the stage empty), the units decoded for each reason, and the decode
// time that overlapped Core 0's work.
#include "check.h"
#include "mp3_sched.h"

#include <stdbool.h>
#include <stdint.h>

static void test_low_ring_decodes_whatever_core0_does(void) {
  mp3_sched_t s;
  mp3_sched_init(&s);
  CHECK(mp3_sched_low(0, 32768));
  CHECK(mp3_sched_low(16384, 32768));
  CHECK(!mp3_sched_low(16385, 32768));
  CHECK_EQ_INT(mp3_sched_next(&s, true, 0, 0), MP3_SCHED_LOW);
  CHECK_EQ_INT(mp3_sched_next(&s, true, 2, 0), MP3_SCHED_LOW);
  CHECK_EQ_INT(mp3_sched_next(&s, true, 0, 100000), MP3_SCHED_LOW);
}

static void test_an_update_decodes_at_most_a_burst(void) {
  mp3_sched_t s;
  mp3_sched_init(&s);
  CHECK_EQ_INT(mp3_sched_next(&s, true, MP3_SCHED_BURST, 0), MP3_SCHED_NONE);
  CHECK_EQ_INT(mp3_sched_next(&s, false, MP3_SCHED_BURST, 100000), MP3_SCHED_NONE);
}

static void test_a_full_ring_decodes_ahead_only_in_a_long_enough_window(void) {
  mp3_sched_t s;
  mp3_sched_init(&s);
  CHECK_EQ_INT(mp3_sched_next(&s, false, 0, 0), MP3_SCHED_NONE);  // working
  CHECK_EQ_INT(mp3_sched_next(&s, false, 0, MP3_SCHED_SEED_US - 1), MP3_SCHED_NONE);
  CHECK_EQ_INT(mp3_sched_next(&s, false, 0, MP3_SCHED_SEED_US), MP3_SCHED_IDLE);
  CHECK_EQ_INT(mp3_sched_next(&s, false, 2, 50000), MP3_SCHED_IDLE);
}

static void test_the_estimate_follows_finished_frames(void) {
  mp3_sched_t s;
  mp3_sched_init(&s);
  for (int i = 0; i < 40; i++)
    mp3_sched_note(&s, 6000);
  CHECK(s.frame_us >= 5900 && s.frame_us <= 6000);
  for (int i = 0; i < 40; i++)
    mp3_sched_note(&s, 1000);
  CHECK(s.frame_us >= 1000 && s.frame_us <= 1100);
}

// ── The pipeline model ──────────────────────────────────────────────────────

#define RING_SIZE   32768u   // mp3_player.c PCM_RING_SIZE
#define STAGE_SIZE  8192u    // STAGING_BUF_SIZE
#define DT          50u      // model step, µs

typedef struct {
  // content
  uint32_t rate, channels, samples;  // samples per channel per unit
  // Core 1: one unit's decode with Core 0 idle / working
  uint32_t d_idle, d_busy;
  // Core 0: paced at `period` with `work` per frame, or never idle
  bool paced;
  uint32_t period, work;
  // a stretch of late frames (no windows): [late_from, late_to) s
  uint32_t late_from_us, late_to_us;
  uint32_t seconds;
} scenario_t;

typedef struct {
  uint64_t underruns;     // output frames with nothing staged
  uint32_t low_frames, idle_frames;
  uint64_t decode_us, contended_us;  // decode time, and the part during work
  uint32_t frame_us;      // the estimate at the end
} result_t;

// Core 0's state at time t: idle until `*until` (true), or working.
static bool core0_idle(const scenario_t *sc, uint64_t t, uint64_t *until) {
  if (!sc->paced)
    return false;
  uint64_t frame_start = t / sc->period * sc->period;
  bool late = t >= sc->late_from_us && t < sc->late_to_us;
  if (late || t - frame_start < sc->work)
    return false;
  *until = frame_start + sc->period;
  return true;
}

static uint32_t window_left(const scenario_t *sc, uint64_t t) {
  uint64_t until;
  return core0_idle(sc, t, &until) ? (uint32_t)(until - t) : 0;
}

static result_t run_model(const scenario_t *sc) {
  result_t r = {0};
  mp3_sched_t s;
  mp3_sched_init(&s);
  const uint32_t fb = 2u * sc->channels;          // bytes per PCM frame
  const uint32_t frame_bytes = sc->samples * fb;  // one unit's PCM
  uint32_t ring = MP3_SCHED_BURST * frame_bytes;  // play()'s pre-fill
  uint32_t stage = 0;
  uint64_t mix_acc = 0;                           // content frames, x1e6
  // Core 1
  bool busy = false, low = false;
  uint32_t decoded = 0;
  mp3_sched_why_t why = MP3_SCHED_NONE;
  uint64_t progress = 0, started = 0;              // progress in µs x d_busy
  uint64_t next_tick = 0;
  bool tick_pending = false;

  #define REFILL() do { \
      if (stage < STAGE_SIZE / 2) { \
        uint32_t n = STAGE_SIZE - stage; if (n > ring) n = ring; \
        n -= n % fb; stage += n; ring -= n; } } while (0)
  REFILL();

  const uint64_t end = (uint64_t)sc->seconds * 1000000u;
  for (uint64_t t = 0; t < end; t += DT) {
    // The mixer drains the stage at the content's rate.
    mix_acc += (uint64_t)sc->rate * DT;
    while (mix_acc >= 1000000u) {
      mix_acc -= 1000000u;
      if (stage >= fb) stage -= fb; else r.underruns++;
    }
    if (t >= next_tick) {
      tick_pending = true;
      next_tick += 1000;
    }
    uint64_t until;
    bool idle0 = core0_idle(sc, t, &until);
    if (busy) {
      // Decode progress: full speed while Core 0 idles, half while it works.
      progress += idle0 ? sc->d_busy * (uint64_t)DT / sc->d_idle : DT;
      r.decode_us += DT;
      if (!idle0) r.contended_us += DT;
      if (progress < sc->d_busy) continue;
      busy = false;
      ring += frame_bytes;
      decoded++;
      if (why == MP3_SCHED_IDLE) {
        r.idle_frames++;
        if (window_left(sc, t) > 0)     // finished inside its window
          mp3_sched_note(&s, (uint32_t)(t + DT - started));
      } else {
        r.low_frames++;
      }
      REFILL();                         // between frames, as the player does
    } else if (tick_pending) {
      // An update starts: refill, then decide (decode_fill_ring).
      tick_pending = false;
      REFILL();
      low = mp3_sched_low(ring, RING_SIZE);
      decoded = 0;
    } else {
      continue;
    }
    // The next frame of this update, if any.
    why = MP3_SCHED_NONE;
    if (RING_SIZE - 1 - ring >= frame_bytes)
      why = mp3_sched_next(&s, low, decoded, window_left(sc, t));
    if (why != MP3_SCHED_NONE) {
      busy = true;
      progress = 0;
      started = t;
    } else {
      decoded = MP3_SCHED_BURST;        // this update is over
    }
  }
  #undef REFILL
  r.frame_us = s.frame_us;
  return r;
}

// 44.1 kHz stereo: two granules a frame, 76.6 a second, each 2.5 ms with
// Core 0 idle (5 ms a frame).
static const scenario_t STEREO_PACED = {
    .rate = 44100, .channels = 2, .samples = 576,
    .d_idle = 2500, .d_busy = 5000,
    .paced = true, .period = 33333, .work = 22000,
    .seconds = 120};
#define STEREO_UNITS (120u * 76u)   // the music's units over the run

// Nova Rail's case: a 30 fps game with ~11 ms of idle per frame. Nearly
// every granule decodes in the idle windows, none underruns, and the
// estimate is the uncontended decode time.
static void test_paced_stereo_decodes_while_core0_idles(void) {
  result_t r = run_model(&STEREO_PACED);
  uint32_t units = r.idle_frames + r.low_frames;
  printf("  paced 44.1 stereo: %u idle + %u low granules, %llu of %llu us contended, "
         "estimate %u us, %llu underruns\n", r.idle_frames, r.low_frames,
         (unsigned long long)r.contended_us, (unsigned long long)r.decode_us,
         r.frame_us, (unsigned long long)r.underruns);
  CHECK_EQ_INT(r.underruns, 0);
  CHECK(units >= STEREO_UNITS);                  // kept up with the music
  CHECK(r.idle_frames * 100u >= units * 95u);
  CHECK(r.contended_us * 100u <= r.decode_us * 5u);
  CHECK(r.frame_us >= 2400 && r.frame_us <= 2650);
}

// Why the unit is a granule: with ~8 ms windows (25 ms of work a frame),
// a whole stereo frame of 10 ms (as libmad took with its state in QMI
// PSRAM) started in a window runs on past its end, and over half of the
// decoding lands on Core 0's work. Halved and made faster (granules of
// 3 ms, two to a window), under a third of it does.
static void test_short_windows_take_granules_not_frames(void) {
  scenario_t whole = STEREO_PACED;
  whole.work = 25000;
  whole.samples = 1152; whole.d_idle = 10000; whole.d_busy = 20000;
  result_t w = run_model(&whole);
  scenario_t gran = STEREO_PACED;
  gran.work = 25000;
  gran.d_idle = 3000; gran.d_busy = 6000;
  result_t g = run_model(&gran);
  printf("  8 ms windows: whole 10 ms frames %llu of %llu us contended, "
         "3 ms granules %llu of %llu (%u idle, %u low)\n",
         (unsigned long long)w.contended_us, (unsigned long long)w.decode_us,
         (unsigned long long)g.contended_us, (unsigned long long)g.decode_us,
         g.idle_frames, g.low_frames);
  CHECK_EQ_INT(w.underruns, 0);
  CHECK_EQ_INT(g.underruns, 0);
  CHECK(w.contended_us * 2u >= w.decode_us);
  CHECK(g.idle_frames + g.low_frames >= STEREO_UNITS);
  CHECK(g.idle_frames * 100u >= (g.idle_frames + g.low_frames) * 60u);
  CHECK(g.contended_us * 3u <= g.decode_us);
  CHECK(g.contended_us * 3u <= w.contended_us);
}

// An app that never paces keeps today's rule: granules only when the ring
// is low, every one of them while Core 0 works, and nothing starves.
static void test_unpaced_stereo_decodes_as_before(void) {
  scenario_t sc = STEREO_PACED;
  sc.paced = false;
  result_t r = run_model(&sc);
  CHECK_EQ_INT(r.underruns, 0);
  CHECK_EQ_INT(r.idle_frames, 0);
  CHECK(r.low_frames >= STEREO_UNITS);
  CHECK_EQ_INT(r.contended_us, r.decode_us);
}

// 22.05 kHz mono: one granule a frame, 38.3 a second.
static void test_paced_mono_22k_decodes_while_core0_idles(void) {
  scenario_t sc = STEREO_PACED;
  sc.rate = 22050; sc.channels = 1; sc.samples = 576;
  sc.d_idle = 1300; sc.d_busy = 2600;
  result_t r = run_model(&sc);
  uint32_t units = r.idle_frames + r.low_frames;
  CHECK_EQ_INT(r.underruns, 0);
  CHECK(units >= 120 * 38);
  CHECK(r.idle_frames * 100u >= units * 95u);
}

// Windows shorter than the estimate start nothing: the ring runs low and
// decodes as it always did, and nothing starves.
static void test_short_windows_fall_back_to_the_low_ring_rule(void) {
  scenario_t sc = STEREO_PACED;
  sc.work = 31500;                     // 1.8 ms windows, 3 ms seed estimate
  result_t r = run_model(&sc);
  CHECK_EQ_INT(r.underruns, 0);
  CHECK_EQ_INT(r.idle_frames, 0);
  CHECK(r.low_frames >= STEREO_UNITS);
}

// A decoder too slow to keep up in the windows alone (one 9 ms granule
// per 11 ms window, 2.55 needed per 33 ms): the low-ring rule makes up the
// rest, and nothing starves.
static void test_slow_decoder_mixes_both_reasons(void) {
  scenario_t sc = STEREO_PACED;
  sc.d_idle = 9000; sc.d_busy = 12000;
  result_t r = run_model(&sc);
  CHECK_EQ_INT(r.underruns, 0);
  CHECK(r.idle_frames > 0);
  CHECK(r.low_frames > 0);
  CHECK(r.idle_frames + r.low_frames >= STEREO_UNITS);
}

// Three seconds of late frames (no pacing wait, so no windows) in the
// middle of a paced run: the ring runs low and decodes; no underrun.
static void test_late_frames_never_starve(void) {
  scenario_t sc = STEREO_PACED;
  sc.late_from_us = 30000000; sc.late_to_us = 33000000;
  result_t r = run_model(&sc);
  CHECK_EQ_INT(r.underruns, 0);
  CHECK(r.low_frames > 0);
  CHECK(r.idle_frames + r.low_frames >= STEREO_UNITS);
}

int main(void) {
  test_low_ring_decodes_whatever_core0_does();
  test_an_update_decodes_at_most_a_burst();
  test_a_full_ring_decodes_ahead_only_in_a_long_enough_window();
  test_the_estimate_follows_finished_frames();
  test_paced_stereo_decodes_while_core0_idles();
  test_short_windows_take_granules_not_frames();
  test_unpaced_stereo_decodes_as_before();
  test_paced_mono_22k_decodes_while_core0_idles();
  test_short_windows_fall_back_to_the_low_ring_rule();
  test_slow_decoder_mixes_both_reasons();
  test_late_frames_never_starve();
  return check_report("test_mp3_sched");
}

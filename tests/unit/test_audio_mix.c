// Host unit tests for src/drivers/audio_mix.c, the one mixer that the
// device's DMA refill ISR and the simulator's output both render. Every
// source adds into the frame; starting or stopping the stream never
// restarts the output or touches the other sources; the master volume
// scales the sum before it is clipped; a tone lasts its duration in mixed
// frames.
#include "check.h"
#include "audio.h"
#include "audio_mix.h"
#include "sound.h"

#include <string.h>

// The output (audio.c on the device): a fake that counts starts and stops.
static bool s_out_on;
static int s_out_starts, s_out_stops;
void audio_output_ensure_running(void) {
  if (!s_out_on) {
    s_out_on = true;
    s_out_starts++;
  }
}
void audio_output_stop(void) {
  s_out_on = false;
  s_out_stops++;
}
bool audio_output_running(void) { return s_out_on; }
uint32_t audio_output_isr_count(void) { return 0; }

// The MP3 player (mp3_player.c) is a fake source too: s_mp3_value per frame.
static bool s_mp3_on;
static int32_t s_mp3_value;
void mp3_player_mix(int32_t *l, int32_t *r, int frames) {
  if (!s_mp3_on)
    return;
  for (int i = 0; i < frames; i++) {
    l[i] += s_mp3_value;
    r[i] += s_mp3_value;
  }
}

static int16_t s_lr[2 * 8192];

static void reset_all(void) {
  audio_mix_init();
  audio_stop_tone();
  audio_stop_stream();
  audio_set_volume(100);
  audio_stream_reset_underruns();
  sound_init();
  s_out_on = false;
  s_out_starts = s_out_stops = 0;
  s_mp3_on = false;
}

static void render(int frames) { audio_mix_render(s_lr, frames); }
static int left(int frame) { return s_lr[2 * frame]; }
static int right(int frame) { return s_lr[2 * frame + 1]; }

// A playing sample player whose every sample is v (mono, AUDIO_OUT_RATE).
static sound_player_t *play_const(int16_t v) {
  sound_sample_t *s = sound_sample_new_blank(0.1f, AUDIO_OUT_RATE, 16, 1);
  CHECK(s != NULL);
  for (uint32_t i = 0; i + 1 < s->length; i += 2)
    memcpy(s->data + i, &v, 2);
  sound_player_t *p = sound_player_create();
  CHECK(p != NULL);
  sound_player_set_sample(p, s);
  sound_player_play(p, 0);
  return p;
}

// n stream frames of v on both sides.
static void push_const(int16_t v, int n) {
  int16_t f[2] = {v, v};
  for (int i = 0; i < n; i++)
    audio_push_samples(f, 1);
}

// The stream ring keeps 8 bits per channel: v comes back as its top byte.
static int ring_value(int16_t v) {
  return ((((int32_t)v + 32768) >> 8) - 128) * 256;
}

static void test_sources_add_up(void) {
  reset_all();
  play_const(1000);
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 4);
  render(1);
  CHECK_EQ_INT(left(0), 1000 + ring_value(2000));
  CHECK_EQ_INT(right(0), 1000 + ring_value(2000));
}

static void test_stopping_the_stream_leaves_the_rest(void) {
  reset_all();
  play_const(1000);
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 64);
  audio_stop_stream();
  CHECK(!audio_stream_active());
  render(1);
  CHECK_EQ_INT(left(0), 1000);         // the sample plays on
  CHECK(s_out_on);                     // and the output with it
  CHECK_EQ_INT(s_out_stops, 0);
}

static void test_starting_a_stream_does_not_restart_anything(void) {
  reset_all();
  sound_player_t *p = play_const(1000);
  CHECK_EQ_INT(s_out_starts, 1);
  render(100);
  uint32_t pos = p->position;
  audio_start_stream(22050);           // a FilePlayer starts mid-sample
  CHECK(audio_stream_active());
  CHECK_EQ_INT(s_out_starts, 1);
  CHECK_EQ_INT(s_out_stops, 0);
  CHECK_EQ_INT(p->position, pos);      // the sample was not rewound
  render(1);
  CHECK_EQ_INT(left(0), 1000);
}

static void test_master_volume_scales_the_sum(void) {
  reset_all();
  play_const(1000);
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 4);
  audio_set_volume(50);
  render(1);
  CHECK_EQ_INT(left(0), (1000 + ring_value(2000)) * 128 / 256);
}

static void test_the_sum_clips(void) {
  reset_all();
  play_const(30000);
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(30000, 4);
  render(1);
  CHECK_EQ_INT(left(0), 32767);
}

// The master volume scales the sum before the clip: a sum over full scale
// turned down to half is half the sum, not half of a clipped one.
static void test_the_volume_applies_before_the_clip(void) {
  reset_all();
  play_const(1000);
  s_mp3_on = true;
  s_mp3_value = 30000;
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(30000, 4);
  audio_set_volume(50);
  render(1);
  int sum = 1000 + ring_value(30000) + 30000;  // 60952: over the clip
  CHECK(sum > 32767);
  CHECK_EQ_INT(left(0), sum * 128 / 256);
  CHECK(left(0) != 32767 * 128 / 256);
  s_mp3_on = false;
}

static void test_a_tone_lasts_its_duration(void) {
  reset_all();
  audio_play_tone(1000, 10);           // 441 frames
  CHECK(s_out_on);
  CHECK(audio_tone_playing());
  render(440);
  CHECK(audio_tone_playing());
  CHECK(left(0) != 0);
  render(1);
  CHECK(!audio_tone_playing());
  render(1);
  CHECK_EQ_INT(left(0), 0);
}

static void test_a_timed_tone_replaces_an_untimed_one(void) {
  reset_all();
  audio_play_tone(500, 0);             // until stopped
  render(100);
  audio_play_tone(1000, 10);           // 441 frames from now
  render(440);
  CHECK(audio_tone_playing());
  render(1);
  CHECK(!audio_tone_playing());
}

static void test_underruns_count_only_while_streaming(void) {
  reset_all();
  uint32_t under = 1;
  render(64);
  audio_stream_debug(NULL, &under, NULL);
  CHECK_EQ_INT(under, 0);
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(1000, 1);
  render(64);
  audio_stream_debug(NULL, &under, NULL);
  CHECK_EQ_INT(under, 63);
}

// #34: a started stream plays from its first frame. The empty ring before
// it is the stream not having begun, not an underrun.
static void test_a_stream_begins_at_its_first_frame(void) {
  reset_all();
  audio_start_stream(AUDIO_OUT_RATE);
  render(256);
  uint32_t under = 1, used = 1;
  audio_stream_debug(NULL, &under, &used);
  CHECK_EQ_INT(under, 0);
  push_const(2000, 10);
  render(64);                          // 10 frames, then 54 missing
  CHECK_EQ_INT(left(0), ring_value(2000));
  CHECK_EQ_INT(left(9), ring_value(2000));
  CHECK_EQ_INT(left(10), 0);
  audio_stream_debug(NULL, &under, &used);
  CHECK_EQ_INT(under, 54);
  CHECK_EQ_INT(used, 0);
}

// A held stream (the fileplayer's play()) takes pushes but plays nothing
// until its producer releases it, so it never starts on a nearly empty
// ring that a slow SD read could starve.
static void test_a_held_stream_plays_when_released(void) {
  reset_all();
  audio_start_stream_held(AUDIO_OUT_RATE);
  CHECK(audio_stream_active());
  CHECK_EQ_INT(audio_ring_free(), 4096);
  push_const(2000, 100);
  render(64);
  CHECK_EQ_INT(left(0), 0);
  uint32_t under = 1, used = 0;
  audio_stream_debug(NULL, &under, &used);
  CHECK_EQ_INT(under, 0);
  CHECK_EQ_INT(used, 100);             // nothing was taken
  audio_stream_release();
  render(1);
  CHECK_EQ_INT(left(0), ring_value(2000));
  audio_stream_debug(NULL, NULL, &used);
  CHECK_EQ_INT(used, 99);
  // Starting again (another play()) holds again.
  audio_start_stream_held(AUDIO_OUT_RATE);
  push_const(2000, 10);
  render(32);
  CHECK_EQ_INT(left(0), 0);
}

// Hold (a paused fileplayer) stops taking frames and keeps the rest; the
// release plays on from the same frame, and the pause is not an underrun.
static void test_hold_keeps_the_ring(void) {
  reset_all();
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(1000, 50);
  push_const(3000, 50);
  render(50);
  audio_stream_hold();
  render(4096);
  CHECK_EQ_INT(left(0), 0);
  uint32_t under = 1, used = 0;
  audio_stream_debug(NULL, &under, &used);
  CHECK_EQ_INT(under, 0);
  CHECK_EQ_INT(used, 50);
  audio_stream_release();
  render(1);
  CHECK_EQ_INT(left(0), ring_value(3000));
}

// A held stream's producer moved (a paused fileplayer's seek): the flush
// empties the ring, so the release plays the new position. A stream that
// plays, or one another producer restarted unheld, is left alone.
static void test_flush_empties_only_a_held_stream(void) {
  reset_all();
  audio_start_stream_held(AUDIO_OUT_RATE);
  push_const(1000, 100);
  audio_stream_flush_held();
  uint32_t used = 1;
  audio_stream_debug(NULL, NULL, &used);
  CHECK_EQ_INT(used, 0);
  CHECK_EQ_INT(audio_ring_free(), 4096);
  push_const(3000, 10);
  audio_stream_release();
  render(1);
  CHECK_EQ_INT(left(0), ring_value(3000));
  // Playing: no effect.
  audio_stream_flush_held();
  audio_stream_debug(NULL, NULL, &used);
  CHECK_EQ_INT(used, 9);
  // Restarted unheld (another producer): no effect either.
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 10);
  audio_stream_flush_held();
  audio_stream_debug(NULL, NULL, &used);
  CHECK_EQ_INT(used, 10);
  // Stopped: nothing to flush, and pushes are dropped as before.
  audio_stop_stream();
  audio_stream_flush_held();
  CHECK_EQ_INT(audio_ring_free(), 0);
}

// Drain: the producer has nothing more (a fileplayer that finished), so
// the ring running dry is the end of the sound, not an underrun. A drain
// also releases a held stream (a file shorter than the ring).
static void test_a_drained_stream_ends_without_underruns(void) {
  reset_all();
  audio_start_stream_held(AUDIO_OUT_RATE);
  push_const(2000, 10);
  audio_stream_drain();
  render(4096);
  CHECK_EQ_INT(left(0), ring_value(2000));
  uint32_t under = 1;
  audio_stream_debug(NULL, &under, NULL);
  CHECK_EQ_INT(under, 0);
  audio_stream_stats_t st;
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.gaps, 0);
  // The next start counts again.
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 1);
  render(32);
  audio_stream_debug(NULL, &under, NULL);
  CHECK_EQ_INT(under, 31);
}

// Plays n frames of the stream (pushing as it goes, the ring never empty).
static void play_through(int n) {
  while (n > 0) {
    int k = n > 1024 ? 1024 : n;
    push_const(1000, k);
    render(k);
    n -= k;
  }
}

// audiostat's stream fields: when the gaps were (ms into the stream, the
// start and loop windows), how many, and the ring's low-water mark.
static void test_stream_stats_say_when(void) {
  reset_all();
  audio_stream_stats_t st;
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.underruns, 0);
  CHECK_EQ_INT(st.gaps, 0);
  CHECK_EQ_INT(st.first_ms, -1);
  CHECK_EQ_INT(st.last_ms, -1);
  CHECK_EQ_INT(st.low_ms, -1);
  CHECK_EQ_INT(st.starts, 0);

  audio_start_stream(AUDIO_OUT_RATE);
  push_const(1000, 4096);
  render(1000);                        // 3096 frames left: 70.2 ms
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.starts, 1);
  CHECK_EQ_INT(st.low_ms, 70);
  render(3096 + 64);                   // a gap 92.9 ms in: the start window
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.underruns, 64);
  CHECK_EQ_INT(st.start_underruns, 64);
  CHECK_EQ_INT(st.loop_underruns, 0);
  CHECK_EQ_INT(st.gaps, 1);          // three starved chunks: one gap
  CHECK_EQ_INT(st.first_ms, 92);
  CHECK_EQ_INT(st.last_ms, 92);
  CHECK_EQ_INT(st.low_ms, 0);

  // 1.5 s in (past the start window), a second gap: neither start nor loop.
  play_through(44100 * 3 / 2 - 4096);
  render(32);
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.underruns, 96);
  CHECK_EQ_INT(st.start_underruns, 64);
  CHECK_EQ_INT(st.loop_underruns, 0);
  CHECK_EQ_INT(st.gaps, 2);
  CHECK_EQ_INT(st.first_ms, 92);
  CHECK_EQ_INT(st.last_ms, 1500);

  // The producer loops, and the ring runs dry 50 ms past the loop point.
  push_const(1000, 441);
  audio_stream_mark_loop();
  push_const(1000, 2205);
  render(441 + 2205 + 32);
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.loops, 1);
  CHECK_EQ_INT(st.underruns, 128);
  CHECK_EQ_INT(st.start_underruns, 64);
  CHECK_EQ_INT(st.loop_underruns, 32);
  CHECK_EQ_INT(st.gaps, 3);
  CHECK_EQ_INT(st.first_ms, 92);
  CHECK_EQ_INT(st.last_ms, 1560);

  // A reset starts a new window; the stream plays on.
  audio_stream_reset_underruns();
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.underruns, 0);
  CHECK_EQ_INT(st.start_underruns, 0);
  CHECK_EQ_INT(st.loop_underruns, 0);
  CHECK_EQ_INT(st.gaps, 0);
  CHECK_EQ_INT(st.first_ms, -1);
  CHECK_EQ_INT(st.last_ms, -1);
  CHECK_EQ_INT(st.low_ms, -1);
  CHECK_EQ_INT(st.starts, 0);
  CHECK_EQ_INT(st.loops, 0);
  push_const(1000, 2205);
  render(32);
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.low_ms, 49);         // 2173 frames: 49.3 ms
}

// Times are in the stream's own content: 22.05 kHz frames are 2 output
// frames each, and a held stream's clock starts at its release.
static void test_stream_stats_at_half_rate(void) {
  reset_all();
  audio_start_stream_held(22050);
  push_const(1000, 2205);              // 100 ms of 22.05 kHz
  render(1000);                        // held: nothing plays
  audio_stream_release();
  render(4410 + 64);                   // the 2205 frames, then 64 missing
  audio_stream_stats_t st;
  audio_stream_get_stats(&st);
  CHECK_EQ_INT(st.underruns, 64);
  CHECK_EQ_INT(st.first_ms, 100);
  CHECK_EQ_INT(st.start_underruns, 64);
}

static void test_half_rate_stream_repeats_each_frame(void) {
  reset_all();
  audio_start_stream(22050);
  push_const(4096, 1);
  push_const(-4096, 1);
  render(4);
  CHECK_EQ_INT(left(0), ring_value(4096));
  CHECK_EQ_INT(left(1), ring_value(4096));
  CHECK_EQ_INT(left(2), ring_value(-4096));
  CHECK_EQ_INT(left(3), ring_value(-4096));
}

static void test_zero_rate_streams_at_the_output_rate(void) {
  reset_all();
  audio_start_stream(0);
  push_const(4096, 1);
  push_const(-4096, 1);
  render(2);
  CHECK_EQ_INT(left(0), ring_value(4096));
  CHECK_EQ_INT(left(1), ring_value(-4096));
}

// While the stream is off, pushes are dropped and the ring reports no room,
// so the producers that pace on it (fileplayer, MOD) wait instead of
// racing through their data.
static void test_pushes_while_stopped_are_dropped(void) {
  reset_all();
  push_const(1000, 10);
  CHECK_EQ_INT(audio_ring_free(), 0);
  audio_start_stream(AUDIO_OUT_RATE);
  CHECK_EQ_INT(audio_ring_free(), 4096);  // nothing of the pushes got in
}

static void test_the_mp3_adds_in_too(void) {
  reset_all();
  play_const(1000);
  s_mp3_on = true;
  s_mp3_value = 500;
  audio_start_stream(AUDIO_OUT_RATE);
  push_const(2000, 4);
  render(1);
  CHECK_EQ_INT(left(0), 1000 + 500 + ring_value(2000));
  audio_set_volume(50);
  render(1);
  CHECK_EQ_INT(left(0), (1000 + 500 + ring_value(2000)) * 128 / 256);
  s_mp3_on = false;
}

int main(void) {
  test_sources_add_up();
  test_stopping_the_stream_leaves_the_rest();
  test_starting_a_stream_does_not_restart_anything();
  test_master_volume_scales_the_sum();
  test_the_sum_clips();
  test_the_volume_applies_before_the_clip();
  test_a_tone_lasts_its_duration();
  test_a_timed_tone_replaces_an_untimed_one();
  test_underruns_count_only_while_streaming();
  test_a_stream_begins_at_its_first_frame();
  test_a_held_stream_plays_when_released();
  test_hold_keeps_the_ring();
  test_flush_empties_only_a_held_stream();
  test_a_drained_stream_ends_without_underruns();
  test_stream_stats_say_when();
  test_stream_stats_at_half_rate();
  test_half_rate_stream_repeats_each_frame();
  test_zero_rate_streams_at_the_output_rate();
  test_pushes_while_stopped_are_dropped();
  test_the_mp3_adds_in_too();
  sound_init();  // LeakSanitizer: the last test's samples
  return check_report("test_audio_mix");
}

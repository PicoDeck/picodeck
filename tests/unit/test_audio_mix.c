// Host unit tests for src/drivers/audio_mix.c, the one mixer that the
// device's DMA refill ISR and the simulator's output both render. Every
// source adds into the frame; starting or stopping the stream never
// restarts the output or touches the other sources; the master volume
// scales the sum; a tone lasts its duration in mixed frames.
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

static int16_t s_lr[2 * 512];

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
  render(64);
  audio_stream_debug(NULL, &under, NULL);
  CHECK_EQ_INT(under, 64);
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

static void test_pushes_while_stopped_are_dropped(void) {
  reset_all();
  push_const(1000, 10);
  CHECK_EQ_INT(audio_ring_free(), 4096);
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
  test_a_tone_lasts_its_duration();
  test_a_timed_tone_replaces_an_untimed_one();
  test_underruns_count_only_while_streaming();
  test_half_rate_stream_repeats_each_frame();
  test_zero_rate_streams_at_the_output_rate();
  test_pushes_while_stopped_are_dropped();
  test_the_mp3_adds_in_too();
  sound_init();  // LeakSanitizer: the last test's samples
  return check_report("test_audio_mix");
}

// Host unit tests for src/drivers/pcm_stage.h: the MP3 player's staging
// buffer as the mixer reads it (resampling, mono, volume, the fade
// envelope, underruns, the end of a stream) and as the refill writes it
// (compaction, the tail staying put while the mixer reads).
#include "check.h"
#include "pcm_stage.h"

#include <string.h>

#define OUT 44100u

static uint8_t s_buf[256] __attribute__((aligned(4)));
static int32_t s_l[128], s_r[128];

static void fresh(pcm_stage_t *s, uint32_t rate, uint8_t channels) {
  pcm_stage_init(s, s_buf, sizeof(s_buf), OUT);
  pcm_stage_set_format(s, rate, channels);
}

// Full gain at once: the tests that are not about fading.
static void full(pcm_stage_t *s) { s->fade = PCM_FADE_NONE; }

static void put(pcm_stage_t *s, const int16_t *v, uint32_t values) {
  uint32_t space;
  uint8_t *tail = pcm_stage_tail(s, &space);
  CHECK(values * 2 <= space);
  memcpy(tail, v, values * 2);
  pcm_stage_commit(s, values * 2);
}

static void put_const(pcm_stage_t *s, int16_t v, uint32_t frames) {
  for (uint32_t i = 0; i < frames * s->channels; i++)
    put(s, &v, 1);
}

static void mix(pcm_stage_t *s, int frames) {
  memset(s_l, 0, sizeof(s_l));
  memset(s_r, 0, sizeof(s_r));
  pcm_stage_mix(s, s_l, s_r, frames);
}

static void test_stereo_at_the_mixer_rate(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 2);
  full(&s);
  const int16_t v[4] = {100, -100, 200, -200};
  put(&s, v, 4);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 100);
  CHECK_EQ_INT(s_r[0], -100);
  CHECK_EQ_INT(s_l[1], 200);
  CHECK_EQ_INT(s_r[1], -200);
  CHECK_EQ_INT(s.frames_played, 2);
  CHECK_EQ_INT(pcm_stage_frames(&s), 0);
}

static void test_mono_plays_on_both_sides(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  const int16_t v[2] = {300, -300};
  put(&s, v, 2);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 300);
  CHECK_EQ_INT(s_r[0], 300);
  CHECK_EQ_INT(s_l[1], -300);
  CHECK_EQ_INT(s_r[1], -300);
}

static void test_half_rate_content_repeats_each_frame(void) {
  pcm_stage_t s;
  fresh(&s, 22050, 1);
  full(&s);
  const int16_t v[2] = {100, 200};
  put(&s, v, 2);
  mix(&s, 4);
  CHECK_EQ_INT(s_l[0], 100);
  CHECK_EQ_INT(s_l[1], 100);
  CHECK_EQ_INT(s_l[2], 200);
  CHECK_EQ_INT(s_l[3], 200);
  CHECK_EQ_INT(s.frames_played, 2);
}

static void test_zero_rate_plays_at_the_mixer_rate(void) {
  pcm_stage_t s;
  fresh(&s, 0, 1);
  full(&s);
  const int16_t v[2] = {100, 200};
  put(&s, v, 2);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 100);
  CHECK_EQ_INT(s_l[1], 200);
}

static void test_volume_scales_about_zero(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  s.vol_scale = 128;
  const int16_t v[2] = {1000, -1000};
  put(&s, v, 2);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 500);
  CHECK_EQ_INT(s_l[1], -500);
}

static void test_mix_adds_to_what_is_there(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  const int16_t v = 100;
  put(&s, &v, 1);
  memset(s_l, 0, sizeof(s_l));
  memset(s_r, 0, sizeof(s_r));
  s_l[0] = 7;
  s_r[0] = -7;
  pcm_stage_mix(&s, s_l, s_r, 1);
  CHECK_EQ_INT(s_l[0], 107);
  CHECK_EQ_INT(s_r[0], 93);
}

static void test_running_dry_is_an_underrun(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 2);
  full(&s);
  mix(&s, 10);
  CHECK_EQ_INT(s.underruns, 10);
  CHECK_EQ_INT(s_l[0], 0);
  CHECK_EQ_INT(s.frames_played, 0);
}

static void test_end_of_stream_is_not_an_underrun(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  const int16_t v = 100;
  put(&s, &v, 1);
  s.eof = true;          // the decoder reached the end: this is the tail
  mix(&s, 10);
  CHECK_EQ_INT(s_l[0], 100);
  CHECK_EQ_INT(s.underruns, 0);
  CHECK_EQ_INT(pcm_stage_frames(&s), 0);
}

static void test_fresh_stage_is_silent_until_faded_in(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  put_const(&s, 1000, 10);
  mix(&s, 10);           // reset leaves it silent: prefilled, not playing
  CHECK_EQ_INT(s_l[0], 0);
  CHECK_EQ_INT(pcm_stage_frames(&s), 10);
  CHECK_EQ_INT(s.underruns, 0);
}

static void test_fade_in_ramps_up_from_silence(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  put_const(&s, 1000, 100);
  pcm_stage_fade_in(&s);
  mix(&s, PCM_STAGE_FADE_FRAMES);
  CHECK_EQ_INT(s_l[0], 0);
  for (unsigned i = 1; i < PCM_STAGE_FADE_FRAMES; i++)
    CHECK(s_l[i] >= s_l[i - 1]);
  CHECK_EQ_INT(s_l[PCM_STAGE_FADE_FRAMES - 1], 1000 * 252 / 256);
  CHECK(s.fade == PCM_FADE_NONE);
  mix(&s, 1);
  CHECK_EQ_INT(s_l[0], 1000);
}

static void test_fade_out_ends_silent_and_keeps_its_data(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  put_const(&s, 1000, 100);
  pcm_stage_fade_out(&s);
  mix(&s, 100);          // stops at the end of the ramp
  CHECK_EQ_INT(s_l[0], 1000);
  CHECK_EQ_INT(s_l[PCM_STAGE_FADE_FRAMES - 1], 1000 * 4 / 256);
  CHECK_EQ_INT(s_l[PCM_STAGE_FADE_FRAMES], 0);
  CHECK(s.fade == PCM_FADE_SILENT);
  CHECK_EQ_INT(pcm_stage_frames(&s), 100 - PCM_STAGE_FADE_FRAMES);
  mix(&s, 10);           // silent: adds nothing, keeps the rest for a resume
  CHECK_EQ_INT(s_l[0], 0);
  CHECK_EQ_INT(pcm_stage_frames(&s), 100 - PCM_STAGE_FADE_FRAMES);
  pcm_stage_fade_out(&s);  // already silent: no change
  CHECK(s.fade == PCM_FADE_SILENT);
}

static void test_fade_reversal_never_jumps(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  put_const(&s, 1000, 100);
  pcm_stage_fade_in(&s);
  mix(&s, 16);                           // gains 0, 4, ... 60 (of 256)
  CHECK_EQ_INT(s_l[15], 1000 * 60 / 256);
  pcm_stage_fade_out(&s);                // turn back down from here
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 1000 * 64 / 256);  // the gain the ramp up had next
  CHECK_EQ_INT(s_l[1], 1000 * 60 / 256);
  pcm_stage_fade_in(&s);                 // and back up again
  mix(&s, 1);
  CHECK_EQ_INT(s_l[0], 1000 * 56 / 256);
  mix(&s, 1);
  CHECK_EQ_INT(s_l[0], 1000 * 60 / 256);
}

static void test_fade_in_waits_for_audio(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  pcm_stage_fade_in(&s);
  mix(&s, PCM_STAGE_FADE_FRAMES);        // starved for a whole ramp's length
  CHECK(s.fade == PCM_FADE_IN);
  put_const(&s, 1000, 10);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 0);               // the ramp starts with the audio
  CHECK_EQ_INT(s_l[1], 1000 * 4 / 256);
}

static void test_fade_out_finishes_while_starved(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  pcm_stage_fade_out(&s);
  mix(&s, PCM_STAGE_FADE_FRAMES);        // no data: must still reach silence
  CHECK(s.fade == PCM_FADE_SILENT);
}

static void test_compact_and_commit_keep_the_order(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  for (int16_t i = 1; i <= 10; i++)
    put(&s, &i, 1);
  mix(&s, 4);                            // 1..4
  pcm_stage_compact(&s);
  CHECK_EQ_INT(s.pos, 0);
  CHECK_EQ_INT(pcm_stage_frames(&s), 6);
  for (int16_t i = 11; i <= 12; i++)
    put(&s, &i, 1);
  mix(&s, 8);
  for (int i = 0; i < 8; i++)
    CHECK_EQ_INT(s_l[i], 5 + i);
}

static void test_tail_stays_put_while_the_reader_consumes(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  full(&s);
  for (int16_t i = 1; i <= 4; i++)
    put(&s, &i, 1);
  uint32_t space;
  uint8_t *tail = pcm_stage_tail(&s, &space);
  mix(&s, 3);                            // the reader moves on meanwhile
  uint32_t space2;
  CHECK(pcm_stage_tail(&s, &space2) == tail);
  CHECK_EQ_INT(space2, space);
  const int16_t v = 5;
  memcpy(tail, &v, 2);                   // the writer's copy, unlocked
  pcm_stage_commit(&s, 2);
  mix(&s, 2);
  CHECK_EQ_INT(s_l[0], 4);
  CHECK_EQ_INT(s_l[1], 5);
}

static void test_tail_space_is_whole_frames(void) {
  pcm_stage_t s;
  fresh(&s, OUT, 1);
  const int16_t v = 1;
  put(&s, &v, 1);                        // 2 bytes used
  pcm_stage_set_format(&s, OUT, 2);      // now 4-byte frames
  uint32_t space;
  pcm_stage_tail(&s, &space);
  CHECK_EQ_INT(space, (sizeof(s_buf) - 2) / 4 * 4);
}

int main(void) {
  test_stereo_at_the_mixer_rate();
  test_mono_plays_on_both_sides();
  test_half_rate_content_repeats_each_frame();
  test_zero_rate_plays_at_the_mixer_rate();
  test_volume_scales_about_zero();
  test_mix_adds_to_what_is_there();
  test_running_dry_is_an_underrun();
  test_end_of_stream_is_not_an_underrun();
  test_fresh_stage_is_silent_until_faded_in();
  test_fade_in_ramps_up_from_silence();
  test_fade_out_ends_silent_and_keeps_its_data();
  test_fade_reversal_never_jumps();
  test_fade_in_waits_for_audio();
  test_fade_out_finishes_while_starved();
  test_compact_and_commit_keep_the_order();
  test_tail_stays_put_while_the_reader_consumes();
  test_tail_space_is_whole_frames();
  return check_report("test_pcm_stage");
}

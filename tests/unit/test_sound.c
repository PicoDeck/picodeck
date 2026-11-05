// Host unit tests for src/drivers/sound.c (the firmware sample mixer; the
// simulator has its own copy in simulator/sim_audio.c). Review row Audio
// High: a sampleplayer kept no reference to its sample, so freeing the
// sample left the mixer (Core 1 DMA ISR) reading freed data. The Lua bridge
// now anchors the sample, and sound_sample_destroy detaches any player still
// using it, so the mixer never sees a freed sample. Built with ASan: an
// attached player after destroy is a heap-use-after-free in the mixer.
#include "check.h"
#include "sound.h"
#include "audio.h"
#include "fakes/sdcard_fake.h"

#include <stdlib.h>
#include <string.h>

size_t umm_fake_live(void);

// audio.c is not linked: sound_player_play only needs the output running.
static int s_stream_starts;
void audio_output_ensure_running(void) { s_stream_starts++; }

static int32_t s_l[64], s_r[64];

static void mix(void) { sound_mixer_process(s_l, s_r, 64); }

static sound_sample_t *blank(float seconds) {
  sound_sample_t *s = sound_sample_new_blank(seconds, 22050, 16, 1);
  if (s)  // non-silent, so a mixed frame is visible in the output
    for (uint32_t i = 0; i + 1 < s->length; i += 2)
      *(int16_t *)(s->data + i) = 1000;
  return s;
}

static void test_destroy_detaches_playing_player(void) {
  sound_init();
  sound_player_t *p = sound_player_create();
  sound_sample_t *s = blank(0.1f);
  CHECK(p && s);
  CHECK(sound_player_set_sample(p, s));
  sound_player_play(p, 0);
  mix();
  CHECK_EQ_INT(s_l[0], 1000);
  CHECK(sound_player_is_playing(p));

  sound_sample_destroy(s);            // the sample dies while playing
  CHECK(!sound_player_is_playing(p));
  CHECK(p->sample == NULL);
  CHECK_EQ_INT(sound_get_playing_source_count(), 0);
  mix();                              // must not read the freed sample
  CHECK_EQ_INT(s_l[0], 0);
  sound_player_play(p, 1);            // no sample: a no-op, not a crash
  CHECK(!sound_player_is_playing(p));

  sound_player_destroy(p);
}

static void test_destroy_detaches_every_player(void) {
  sound_init();
  sound_player_t *a = sound_player_create(), *b = sound_player_create();
  sound_sample_t *s = blank(0.1f), *other = blank(0.1f);
  sound_player_set_sample(a, s);
  sound_player_set_sample(b, s);
  sound_player_t *c = sound_player_create();
  sound_player_set_sample(c, other);
  sound_player_play(a, 0);
  sound_player_play(b, 0);
  sound_player_play(c, 0);
  sound_sample_destroy(s);
  CHECK(a->sample == NULL && b->sample == NULL);
  CHECK(c->sample == other && sound_player_is_playing(c));
  mix();
  CHECK_EQ_INT(s_l[0], 1000);         // only c is left
  sound_player_destroy(a);
  sound_player_destroy(b);
  sound_player_destroy(c);
  sound_sample_destroy(other);
}

static void test_player_slots_are_distinct(void) {
  sound_init();
  sound_player_t *p[SOUND_MAX_PLAYERS];
  for (int i = 0; i < SOUND_MAX_PLAYERS; i++) {
    p[i] = sound_player_create();   // no sample set: must still hold a slot
    CHECK(p[i] != NULL);
    for (int j = 0; j < i; j++)
      CHECK(p[i] != p[j]);
  }
  CHECK(sound_player_create() == NULL);  // all taken
  sound_player_destroy(p[3]);
  CHECK(sound_player_create() == p[3]);  // freed slot is reused

  // A slot stays taken after its sample is destroyed under it.
  sound_sample_t *s = blank(0.05f);
  sound_player_set_sample(p[0], s);
  sound_sample_destroy(s);
  CHECK(sound_player_create() == NULL);
  for (int i = 0; i < SOUND_MAX_PLAYERS; i++)
    sound_player_destroy(p[i]);
}

static void test_player_destroy_leaves_sample(void) {
  sound_init();
  sound_sample_t *s = blank(0.05f);
  sound_player_t *p = sound_player_create();
  sound_player_set_sample(p, s);
  sound_player_play(p, 0);
  sound_player_destroy(p);            // never frees the sample
  CHECK(s->loaded && s->data != NULL);
  sound_player_t *q = sound_player_create();
  sound_player_set_sample(q, s);
  sound_player_play(q, 0);
  mix();
  CHECK_EQ_INT(s_l[0], 1000);
  sound_player_destroy(q);
  sound_sample_destroy(s);
}

static void test_sample_slots_recycle(void) {
  sound_init();
  for (int round = 0; round < 4 * SOUND_MAX_PLAYERS; round++) {
    sound_sample_t *s = blank(0.01f);
    CHECK(s != NULL);
    sound_player_t *p = sound_player_create();
    CHECK(p != NULL);
    sound_player_set_sample(p, s);
    sound_player_play(p, 1);
    sound_sample_destroy(s);
    sound_player_destroy(p);
  }
}

// A mono 16-bit 22050 Hz WAV of `frames` frames, every sample = `value`.
static void put_wav(const char *path, uint32_t frames, int16_t value) {
  uint32_t data = frames * 2;
  size_t len = 44 + data;
  uint8_t *b = calloc(1, len);
  uint32_t u32;
  uint16_t u16;
  memcpy(b, "RIFF", 4); u32 = 36 + data; memcpy(b + 4, &u32, 4);
  memcpy(b + 8, "WAVEfmt ", 8); u32 = 16; memcpy(b + 16, &u32, 4);
  u16 = 1; memcpy(b + 20, &u16, 2);            // PCM
  u16 = 1; memcpy(b + 22, &u16, 2);            // mono
  u32 = 22050; memcpy(b + 24, &u32, 4);
  u32 = 44100; memcpy(b + 28, &u32, 4);        // byte rate
  u16 = 2; memcpy(b + 32, &u16, 2);            // block align
  u16 = 16; memcpy(b + 34, &u16, 2);
  memcpy(b + 36, "data", 4); memcpy(b + 40, &data, 4);
  for (uint32_t i = 0; i < frames; i++) memcpy(b + 44 + 2 * i, &value, 2);
  sdfake_put(path, (const char *)b, len);
  free(b);
}

static void test_reload_live_sample(void) {
  // sample:load() on a loaded, playing sample: the old data is freed (it
  // leaked before; LeakSanitizer reports it) and the player is rewound into
  // the new data rather than left past its end.
  sdfake_reset();
  put_wav("/long.wav", 4000, 1000);
  put_wav("/short.wav", 100, 2000);
  sound_init();
  sound_sample_t *s = sound_sample_create();
  CHECK(sound_sample_load(s, "/long.wav"));
  CHECK_EQ_INT(sound_sample_get_length(s), 4000);
  sound_player_t *p = sound_player_create();
  sound_player_set_sample(p, s);
  sound_player_play(p, 0);
  for (int i = 0; i < 20; i++) mix();          // well past frame 100
  CHECK(p->position > 200);
  CHECK(sound_sample_load(s, "/short.wav"));
  CHECK_EQ_INT(sound_sample_get_length(s), 100);
  CHECK_EQ_INT(p->position, 0);
  mix();
  CHECK_EQ_INT(s_l[0], 2000);
  CHECK(!sound_sample_load(s, "/missing.wav"));  // a failed load keeps the data
  CHECK_EQ_INT(sound_sample_get_length(s), 100);
  sound_player_destroy(p);
  sound_sample_destroy(s);
}

static void test_volume_is_0_to_100(void) {
  sound_init();
  sound_player_t *p = sound_player_create();
  CHECK_EQ_INT(sound_player_get_volume(p), 100);   // default
  sound_player_set_volume(p, 255);
  CHECK_EQ_INT(sound_player_get_volume(p), 100);
  sound_player_set_volume(p, 40);
  CHECK_EQ_INT(sound_player_get_volume(p), 40);
  sound_player_destroy(p);
}

static void test_init_reclaims_everything(void) {
  sound_init();
  for (int i = 0; i < SOUND_MAX_PLAYERS; i++) {
    sound_player_t *p = sound_player_create();
    sound_sample_t *s = blank(0.01f);
    CHECK(p && s);
    sound_player_set_sample(p, s);
    sound_player_play(p, 0);
  }
  sound_init();                       // app exit: LeakSanitizer checks the data
  CHECK_EQ_INT(sound_get_playing_source_count(), 0);
  mix();
  CHECK_EQ_INT(s_l[0], 0);
  CHECK(sound_player_create() != NULL);
  sound_init();
}

// Task 11 review (Task 14): setPlayRange(start, ...) with start past the
// sample's end made effective_start > effective_end, and the loop reset
// then read sample->data out of bounds (ASan: heap-buffer-overflow).
static void test_play_range_is_clamped(void) {
  sound_init();
  sound_player_t *p = sound_player_create();
  sound_sample_t *s = blank(0.01f);           // 220 frames of 1000
  CHECK(p && s);
  sound_player_set_sample(p, s);
  uint32_t frames = sound_sample_get_length(s);

  sound_player_set_play_range(p, frames + 1000, 0);   // start past the end
  sound_player_play(p, 0);
  for (int i = 0; i < 20; i++) mix();
  CHECK(sound_player_is_playing(p));

  sound_player_set_play_range(p, frames + 1000, frames + 2000);
  sound_player_play(p, 0);
  for (int i = 0; i < 20; i++) mix();

  sound_player_set_play_range(p, 50, 10);             // start after end
  sound_player_play(p, 3);
  for (int i = 0; i < 40; i++) mix();

  sound_player_set_play_range(p, 10, 50);             // a real range
  sound_player_play(p, 0);
  mix();
  CHECK_EQ_INT(s_l[0], 1000);
  CHECK(p->position >= 10 * 2 && p->position < 50 * 2);

  sound_player_destroy(p);
  sound_sample_destroy(s);
}

// Review #7: samples had the players' cap of 8, calloc'd from the ~2.6 KB
// SRAM heap. Now each is a umm allocation on a list, freed by sound_init.
static void test_samples_have_no_fixed_limit(void) {
  sound_init();
  enum { N = 64 };
  sound_sample_t *s[N];
  for (int i = 0; i < N; i++) {
    s[i] = sound_sample_create();
    CHECK(s[i] != NULL);
    for (int j = 0; j < i; j++)
      CHECK(s[i] != s[j]);
  }
  // Unlink from the head, the middle and the tail of the list.
  for (int i = 0; i < N; i += 2)
    sound_sample_destroy(s[i]);
  sound_sample_destroy(s[N - 1]);
  for (int i = 0; i < N; i += 2) {
    s[i] = blank(0.01f);
    CHECK(s[i] != NULL);
  }
  sound_init();  // frees the rest exactly once (ASan: no double free; LSan: no leak)
}

static void test_sample_structs_come_from_the_umm_heap(void) {
  sound_init();
  size_t before = umm_fake_live();
  sound_sample_t *s = sound_sample_create();
  CHECK(s != NULL);
  CHECK_EQ_INT((int)(umm_fake_live() - before), 1);
  sound_sample_destroy(s);
  CHECK_EQ_INT((int)(umm_fake_live() - before), 0);
}

// A player moved off its voice: the voice is free and the mixer reads no copy.
static void test_park_frees_the_voice_and_keeps_the_state(void) {
  sound_init();
  sound_sample_t *s = blank(0.1f);
  sound_player_t *p = sound_player_create();
  CHECK(p && s);
  sound_player_set_sample(p, s);
  sound_player_set_volume(p, 40);
  sound_player_set_rate(p, 2.0f);
  sound_player_set_play_range(p, 10, 500);

  sound_player_t parked;
  sound_player_play(p, 0);
  CHECK(!(sound_voices_parkable() & (1u << sound_player_voice(p))));
  CHECK(!sound_player_park(p, &parked));  // audible
  CHECK(p->in_use && p->playing);

  sound_player_stop(p);
  CHECK(sound_voices_parkable() & (1u << sound_player_voice(p)));
  CHECK(sound_player_park(p, &parked));
  CHECK(!p->in_use && !p->playing && !p->sample);
  CHECK(!parked.in_use && !parked.playing);
  CHECK(parked.sample == s);
  CHECK_EQ_INT((int)(parked.volume), 40);
  CHECK(parked.rate == 2.0f);
  CHECK_EQ_INT((int)(parked.play_start), 10);
  CHECK_EQ_INT((int)(parked.play_end), 500);
  CHECK(!(sound_voices_parkable() & (1u << sound_player_voice(p))));
  mix();  // nothing to read

  // Every voice is free for someone else.
  sound_player_t *held[SOUND_MAX_PLAYERS];
  for (int i = 0; i < SOUND_MAX_PLAYERS; i++) {
    held[i] = sound_player_create();
    CHECK(held[i] != NULL);
  }
  for (int i = 0; i < SOUND_MAX_PLAYERS; i++)
    sound_player_destroy(held[i]);
  sound_sample_destroy(s);
}

static void test_unpark_plays_on_from_where_it_was(void) {
  sound_init();
  sound_sample_t *s = blank(0.1f);
  sound_player_t *p = sound_player_create();
  sound_player_set_sample(p, s);
  sound_player_play(p, 0);
  mix();
  uint32_t where = p->position;
  CHECK(where > 0);

  // A paused player parks too, still "playing".
  sound_player_t parked;
  p->paused = true;
  CHECK(sound_voices_parkable() & (1u << sound_player_voice(p)));
  CHECK(sound_player_park(p, &parked));
  CHECK(parked.playing && parked.paused);
  CHECK_EQ_INT((int)(parked.position), where);

  // Back on another voice: it goes on from there.
  sound_player_t *voice = sound_player_create();
  CHECK(voice != NULL);
  sound_player_unpark(voice, &parked);
  CHECK(voice->in_use && voice->sample == s);
  voice->paused = false;
  memset(s_l, 0, sizeof(s_l));
  mix();
  CHECK_EQ_INT((int)(s_l[0]), 1000);  // mixed again
  CHECK(voice->position > where);
  // The mixer never read the copy.
  CHECK_EQ_INT((int)(parked.position), where);
  sound_player_destroy(voice);
  sound_sample_destroy(s);
}

static int s_fired;
static int count_fired(void *arg) {
  (void)arg;
  s_fired++;
  return 0;
}

// The callback's flag must reach the pump first; with no callback it can't matter.
static void test_park_waits_for_a_callback_flag(void) {
  sound_init();
  sound_sample_t *s = blank(0.001f);
  sound_player_t *p = sound_player_create();
  sound_player_set_sample(p, s);
  sound_player_set_finish_callback(p, count_fired, NULL);
  sound_player_play(p, 1);
  s_fired = 0;
  for (int i = 0; i < 8 && p->playing; i++)
    mix();
  CHECK(!p->playing && p->finish_pending);

  sound_player_t parked;
  CHECK(!sound_player_park(p, &parked));
  sound_pump_callbacks();
  CHECK_EQ_INT((int)(s_fired), 1);
  CHECK(sound_player_park(p, &parked));
  CHECK(!p->finish_callback && !p->finish_pending);
  CHECK(parked.finish_callback == count_fired);

  // No callback: the flag holds nothing back, and is not left behind.
  p = sound_player_create();
  sound_player_set_sample(p, s);
  sound_player_play(p, 1);
  for (int i = 0; i < 8 && p->playing; i++)
    mix();
  CHECK(p->finish_pending);
  CHECK(sound_player_park(p, &parked));
  CHECK(!p->finish_pending);
  sound_sample_destroy(s);
}

static void test_clock_counts_milliseconds_of_output(void) {
  sound_init();
  CHECK_EQ_INT((int)(sound_get_current_time()), 0);
  // 64 frames a mix; 44100 frames is one second, and no chunk's rounding adds up.
  for (int i = 0; i < 44100 / 64; i++)
    mix();
  CHECK(sound_get_current_time() >= 999 && sound_get_current_time() <= 1000);
  for (int i = 0; i < 9 * 44100 / 64; i++)
    mix();
  CHECK(sound_get_current_time() >= 9990 && sound_get_current_time() <= 10000);
  sound_reset_time();
  CHECK_EQ_INT((int)(sound_get_current_time()), 0);
}

static void test_blank_samples_are_silent(void) {
  sound_init();
  sound_sample_t *s8 = sound_sample_new_blank(0.01f, 8000, 8, 1);
  sound_sample_t *s16 = sound_sample_new_blank(0.01f, 8000, 16, 1);
  CHECK(s8 && s16);
  CHECK_EQ_INT((int)(s8->data[0]), 0x80);
  CHECK_EQ_INT((int)(s8->data[s8->length - 1]), 0x80);
  CHECK_EQ_INT((int)(s16->data[0]), 0);
  // And the mixer hears silence in both.
  sound_player_t *p = sound_player_create();
  sound_player_set_sample(p, s8);
  sound_player_play(p, 0);
  mix();
  CHECK_EQ_INT((int)(s_l[0]), 0);
  sound_player_set_sample(p, s16);
  mix();
  CHECK_EQ_INT((int)(s_l[0]), 0);
  sound_player_destroy(p);
  sound_sample_destroy(s8);
  sound_sample_destroy(s16);
}

int main(void) {
  test_destroy_detaches_playing_player();
  test_destroy_detaches_every_player();
  test_player_slots_are_distinct();
  test_player_destroy_leaves_sample();
  test_sample_slots_recycle();
  test_reload_live_sample();
  test_volume_is_0_to_100();
  test_init_reclaims_everything();
  test_play_range_is_clamped();
  test_samples_have_no_fixed_limit();
  test_sample_structs_come_from_the_umm_heap();
  test_park_frees_the_voice_and_keeps_the_state();
  test_unpark_plays_on_from_where_it_was();
  test_park_waits_for_a_callback_flag();
  test_clock_counts_milliseconds_of_output();
  test_blank_samples_are_silent();
  CHECK(s_stream_starts > 0);
  return check_report("test_sound");
}

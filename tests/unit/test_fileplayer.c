// Host unit tests for src/drivers/fileplayer.c (the firmware WAV streamer;
// since Task 14 the simulator runs this same file).
//
// Review rows (Audio/storage): Critical 1 "The WAV fileplayer f_reads 4 KB
// on every tick and pushes it into a 4096-frame ring that silently drops
// overflow" and High "Cross-core use-after-free: Core 0 closes and frees
// s_current_file ... One s_current_file is shared by all fileplayer
// instances".
//
// audio.c is replaced by a model of the stream ring: audio_push_samples()
// counts what fits and what would have been dropped, drain() plays frames
// out. The ring race test runs fileplayer_update() on a second thread (Core
// 1) against load/play/stop on this one (Core 0); built with ASan, a use of
// a closed file is a heap-use-after-free.
#include "check.h"
#include "fileplayer.h"
#include "audio.h"
#include "sdcard.h"
#include "fakes/sdcard_fake.h"
#include "qoa/qoa.h"  // vendored reference encoder, for fixtures

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// ── Stream ring model (audio.c) ─────────────────────────────────────────────
#define RING_FRAMES 4096u
static uint32_t s_ring_used;
static uint64_t s_pushed;     // frames accepted into the ring
static uint64_t s_dropped;    // frames offered with no room (audio lost)
static int s_starts, s_stops;
static uint32_t s_rate;
static int16_t s_last_l;      // value of the most recent frame pushed
static bool s_arm_first;      // record the next frame pushed in s_first_l
static int16_t s_first_l;

void audio_start_stream(uint32_t sample_rate) { s_starts++; s_rate = sample_rate; s_ring_used = 0; }
void audio_stop_stream(void) { s_stops++; }
uint32_t audio_ring_free(void) { return RING_FRAMES - s_ring_used; }
void audio_push_samples(const int16_t *samples, int count) {
  for (int i = 0; i < count; i++) {
    if (s_ring_used >= RING_FRAMES) { s_dropped++; continue; }
    s_ring_used++;
    s_pushed++;
    s_last_l = samples[i * 2];
    if (s_arm_first) { s_first_l = s_last_l; s_arm_first = false; }
  }
}
static void drain(uint32_t frames) {
  s_ring_used = frames > s_ring_used ? 0 : s_ring_used - frames;
}

static void ring_reset(void) {
  s_ring_used = 0; s_pushed = s_dropped = 0; s_starts = s_stops = 0;
}

// ── WAV fixtures ────────────────────────────────────────────────────────────
static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

// A 16-bit PCM WAV of `frames` frames whose every sample is `value`.
static void put_wav(const char *path, uint32_t frames, uint16_t channels,
                    uint32_t rate, int16_t value) {
  uint32_t data = frames * channels * 2;
  uint8_t *buf = calloc(1, 44 + data);
  memcpy(buf, "RIFF", 4); le32(buf + 4, 36 + data); memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4); le32(buf + 16, 16); le16(buf + 20, 1);
  le16(buf + 22, channels); le32(buf + 24, rate); le32(buf + 28, rate * channels * 2);
  le16(buf + 32, channels * 2); le16(buf + 34, 16);
  memcpy(buf + 36, "data", 4); le32(buf + 40, data);
  for (uint32_t i = 0; i < frames * channels; i++) le16(buf + 44 + i * 2, (uint16_t)value);
  sdfake_put(path, (const char *)buf, 44 + data);
  free(buf);
}

// A QOA file of the interleaved 16-bit `pcm`, `frames` content frames per
// channel, encoded with the vendored reference encoder.
static void put_qoa_pcm(const char *path, const int16_t *pcm, uint32_t frames,
                        uint8_t channels, uint32_t rate) {
  qoa_desc d;
  memset(&d, 0, sizeof(d));
  d.channels = channels;
  d.samplerate = rate;
  d.samples = frames;
  unsigned len = 0;
  void *q = qoa_encode((const short *)pcm, &d, &len);
  CHECK(q != NULL);
  sdfake_put(path, (const char *)q, len);
  free(q);
}

// A QOA file of `frames` content frames per channel whose every sample is
// `value`, encoded with the vendored reference encoder.
static void put_qoa(const char *path, uint32_t frames, uint8_t channels,
                    uint32_t rate, int16_t value) {
  int16_t *pcm = malloc((size_t)frames * channels * 2);
  for (uint32_t i = 0; i < frames * channels; i++) pcm[i] = value;
  qoa_desc d;
  memset(&d, 0, sizeof(d));
  d.channels = channels;
  d.samplerate = rate;
  d.samples = frames;
  unsigned len = 0;
  void *q = qoa_encode((const short *)pcm, &d, &len);
  CHECK(q != NULL);
  sdfake_put(path, (const char *)q, len);
  free(q);
  free(pcm);
}

static void setup(void) {
  sdfake_reset();
  ring_reset();
  fileplayer_init();
  fileplayer_reset();
}

// Tick Core 1 until the player stops, draining `per_tick` frames each tick
// (the DMA's pace between polls). Returns the number of ticks.
static int run(fileplayer_t *p, uint32_t per_tick, int max_ticks) {
  int t = 0;
  while (fileplayer_is_playing(p) && t < max_ticks) {
    fileplayer_update();
    drain(per_tick);
    t++;
  }
  return t;
}

// Critical 1: the producer never offers the ring more than it can hold, so
// nothing is dropped and every frame of the file is played.
static void test_flow_control_plays_every_frame(void) {
  setup();
  put_wav("/a.wav", 22050, 2, 22050, 1000);   // 1 s stereo
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.wav"));
  CHECK(fileplayer_play(p, 1));
  // 44 frames per tick is ~2 ms of 22050 Hz content: far slower than SD.
  int ticks = run(p, 44, 100000);
  CHECK(!fileplayer_is_playing(p));
  CHECK_EQ_INT(s_dropped, 0);
  CHECK_EQ_INT(s_pushed, 22050);
  // It took (at least) as many ticks as the audio needs to drain.
  CHECK(ticks >= (22050 - (int)RING_FRAMES) / 44);
  fileplayer_destroy(p);
}

static void test_mono_and_rate_flow_control(void) {
  setup();
  put_wav("/m.wav", 11025, 1, 11025, -500);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/m.wav"));
  fileplayer_set_rate(p, 0.5f);               // each input frame -> 2 out
  CHECK(fileplayer_play(p, 1));
  run(p, 100, 100000);
  CHECK_EQ_INT(s_dropped, 0);
  CHECK_EQ_INT(s_pushed, 22050);
  fileplayer_destroy(p);
}

// A full ring: no SD read at all (the card stays free for Core 0).
static void test_full_ring_reads_nothing(void) {
  setup();
  put_wav("/a.wav", 22050, 2, 22050, 1);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.wav"));
  CHECK(fileplayer_play(p, 1));
  s_ring_used = RING_FRAMES;
  int before = sdfake_try_reads();
  for (int i = 0; i < 10; i++) fileplayer_update();
  CHECK_EQ_INT(sdfake_try_reads(), before);
  CHECK_EQ_INT(fileplayer_get_offset(p), 0);
  CHECK(fileplayer_is_playing(p));
  fileplayer_destroy(p);
}

// SD busy (Core 0 holds the card): the tick is skipped, nothing is lost.
static void test_sd_busy_skips_the_tick(void) {
  setup();
  put_wav("/a.wav", 4410, 2, 22050, 7);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.wav"));
  CHECK(fileplayer_play(p, 1));
  sdfake_set_busy(true);
  for (int i = 0; i < 20; i++) fileplayer_update();
  CHECK_EQ_INT(s_pushed, 0);
  CHECK(fileplayer_is_playing(p));
  CHECK_EQ_INT(sdfake_blocking_while_busy(), 0);  // Core 1 only try-locks
  sdfake_set_busy(false);
  run(p, 256, 100000);
  CHECK_EQ_INT(s_pushed, 4410);
  CHECK_EQ_INT(s_dropped, 0);
  fileplayer_destroy(p);
}

// High row: each fileplayer has its own file. Loading b must not close a's
// file or give a b's data and length.
static void test_players_keep_their_own_files(void) {
  setup();
  put_wav("/long.wav", 22050, 2, 22050, 1111);
  put_wav("/short.wav", 2205, 2, 22050, 2222);
  fileplayer_t *a = fileplayer_create(), *b = fileplayer_create();
  CHECK(a && b && a != b);
  CHECK(fileplayer_load(a, "/long.wav"));
  CHECK(fileplayer_load(b, "/short.wav"));
  CHECK_EQ_INT(fileplayer_get_length(a), 22050);
  CHECK_EQ_INT(fileplayer_get_length(b), 2205);
  CHECK(fileplayer_play(a, 1));
  run(a, 256, 100000);
  CHECK_EQ_INT(s_pushed, 22050);
  CHECK_EQ_INT(s_last_l, 1111);
  ring_reset();
  CHECK(fileplayer_play(b, 1));
  run(b, 256, 100000);
  CHECK_EQ_INT(s_pushed, 2205);
  CHECK_EQ_INT(s_last_l, 2222);
  // Stopping one player leaves the other's file loaded: a can play again.
  fileplayer_stop(b);
  ring_reset();
  CHECK(fileplayer_play(a, 1));
  run(a, 256, 100000);
  CHECK_EQ_INT(s_pushed, 22050);
  fileplayer_destroy(a);
  fileplayer_destroy(b);
  CHECK_EQ_INT(sdfake_try_reads() > 0, 1);
}

// High row: Core 0 load/play/stop against Core 1's update. Before the
// player lock, stop() closed (freed) the file while update() read it.
static atomic_bool s_run_core1;
static void *core1(void *arg) {
  (void)arg;
  while (atomic_load(&s_run_core1)) {
    fileplayer_update();
    drain(512);
  }
  return NULL;
}

static void test_cross_core_stop_is_safe(void) {
  setup();
  put_wav("/a.wav", 44100, 2, 44100, 5);
  put_wav("/b.wav", 4410, 1, 22050, 6);
  fileplayer_t *p = fileplayer_create(), *q = fileplayer_create();
  atomic_store(&s_run_core1, true);
  pthread_t t;
  pthread_create(&t, NULL, core1, NULL);
  for (int i = 0; i < 3000; i++) {
    fileplayer_load(p, (i & 1) ? "/a.wav" : "/b.wav");
    fileplayer_play(p, 1);
    if (i % 3 == 0) fileplayer_load(q, "/b.wav");
    fileplayer_stop(p);
  }
  atomic_store(&s_run_core1, false);
  pthread_join(t, NULL);
  fileplayer_destroy(p);
  fileplayer_destroy(q);
  CHECK(1);
}

// QOA through the same flow control: the producer paces on the ring, so a
// 1 s mono file plays every one of its frames and drops none.
static void test_qoa_plays_every_frame(void) {
  setup();
  put_qoa("/a.qoa", 22050, 1, 22050, 1000);  // 1 s mono
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.qoa"));
  CHECK_EQ_INT(fileplayer_get_length(p), 22050);
  CHECK(fileplayer_play(p, 1));
  int ticks = run(p, 44, 100000);
  CHECK(!fileplayer_is_playing(p));
  CHECK_EQ_INT(s_dropped, 0);
  CHECK_EQ_INT(s_pushed, 22050);
  CHECK(ticks >= (22050 - (int)RING_FRAMES) / 44);
  // QOA is lossy: the constant comes back near it, not at it.
  CHECK(s_last_l > 900 && s_last_l < 1100);
  fileplayer_destroy(p);
}

static void test_qoa_stereo_at_44100(void) {
  setup();
  put_qoa("/s.qoa", 44100, 2, 44100, -3000);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/s.qoa"));
  CHECK_EQ_INT(fileplayer_get_length(p), 44100);
  CHECK(fileplayer_play(p, 1));
  run(p, 88, 100000);
  CHECK(!fileplayer_is_playing(p));
  CHECK_EQ_INT(s_dropped, 0);
  CHECK_EQ_INT(s_pushed, 44100);
  fileplayer_destroy(p);
}

// A QOA frame boundary is a decode unit; SD busy must still skip the tick.
static void test_qoa_sd_busy_skips_the_tick(void) {
  setup();
  put_qoa("/a.qoa", 4410, 2, 22050, 7);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.qoa"));
  CHECK(fileplayer_play(p, 1));
  sdfake_set_busy(true);
  for (int i = 0; i < 20; i++) fileplayer_update();
  CHECK_EQ_INT(s_pushed, 0);
  CHECK(fileplayer_is_playing(p));
  // Core 1 only try-locks the card: no call that would wait for Core 0's
  // SD mutex (which, held by Core 1 under s_lock, stalls Core 0 too).
  CHECK_EQ_INT(sdfake_blocking_while_busy(), 0);
  sdfake_set_busy(false);
  run(p, 256, 100000);
  CHECK_EQ_INT(s_pushed, 4410);
  CHECK_EQ_INT(s_dropped, 0);
  fileplayer_destroy(p);
}

// Seek replays from the offset (sample-exact: test_qoa_seek_is_sample_exact);
// looping a QOA rewinds the compressed stream too.
static void test_qoa_seek_and_loop(void) {
  setup();
  put_qoa("/seek.qoa", 66150, 1, 22050, 500);  // 3 s mono
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/seek.qoa"));
  CHECK(fileplayer_play(p, 1));
  for (int i = 0; i < 10; i++) { fileplayer_update(); drain(44); }
  fileplayer_set_offset(p, 2);
  CHECK_EQ_INT(fileplayer_get_offset(p), 2);
  uint64_t before = s_pushed;
  run(p, 44, 100000);
  // One second of content remains after the seek.
  uint64_t after = s_pushed - before;
  CHECK(after > 22050 * 99 / 100 && after < 22050 * 101 / 100);
  CHECK(!fileplayer_is_playing(p));

  // play(2) delivers the file twice.
  ring_reset();
  CHECK(fileplayer_play(p, 2));
  run(p, 100, 100000);
  CHECK_EQ_INT(s_pushed, 66150 * 2);
  CHECK_EQ_INT(s_dropped, 0);
  fileplayer_destroy(p);
}

// A seek lands on its sample, not on the start of the QOA frame holding it:
// the audio after it is the audio at the offset, and the file's tail still
// plays. 3 s mono at 22050 Hz: 500 until 2 s, then -2000, and 3000 for the
// last 2000 samples. 2 s (44100) is 3140 samples into frame 8 (40960).
static void test_qoa_seek_is_sample_exact(void) {
  setup();
  enum { N = 66150, AT = 44100, TAIL = 64150 };
  int16_t *pcm = malloc(N * 2);
  for (int i = 0; i < N; i++) pcm[i] = i < AT ? 500 : (i < TAIL ? -2000 : 3000);
  put_qoa_pcm("/step.qoa", pcm, N, 1, 22050);
  free(pcm);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/step.qoa"));
  CHECK(fileplayer_play(p, 1));
  for (int i = 0; i < 10; i++) { fileplayer_update(); drain(44); }
  fileplayer_set_offset(p, 2);
  uint64_t before = s_pushed;
  s_arm_first = true;
  run(p, 44, 100000);
  CHECK_EQ_INT((int)(s_pushed - before), N - AT);
  CHECK(s_first_l < -1000);  // the -2000 after 2 s, not the 500 before it
  CHECK(s_last_l > 2000);    // the file's last samples were delivered
  fileplayer_destroy(p);
}

// A QOA file cut short (its header counts more samples than its frames
// hold, e.g. an interrupted copy) ends where the frames do: play(2) plays
// what is there twice, as a short WAV does, rather than stopping at the cut.
static void test_qoa_short_file_ends_at_its_frames(void) {
  setup();
  enum { N = 12000 };  // frames of 5120, 5120 and 1760
  int16_t *pcm = malloc(N * 2);
  for (int i = 0; i < N; i++) pcm[i] = 900;
  qoa_desc d;
  memset(&d, 0, sizeof(d));
  d.channels = 1;
  d.samplerate = 22050;
  d.samples = N;
  unsigned len = 0;
  uint8_t *q = qoa_encode((const short *)pcm, &d, &len);
  CHECK(q != NULL);
  uint32_t full = 8 + 16 + 8 * 256;              // one full mono frame
  sdfake_put("/cut.qoa", (const char *)q, 8 + 2 * full);  // drop the tail frame
  free(q);
  free(pcm);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/cut.qoa"));
  CHECK(fileplayer_play(p, 2));
  run(p, 44, 100000);
  CHECK(!fileplayer_is_playing(p));
  CHECK_EQ_INT(s_pushed, 2 * 2 * 5120);
  fileplayer_destroy(p);
}

// A copy cut part way through a frame (as an interrupted one usually is)
// plays its whole frames and loops like a short WAV; the cut frame is not
// playable, and the length says what is there.
static void test_qoa_cut_mid_frame_plays_what_is_there(void) {
  setup();
  enum { N = 12000 };  // mono frames of 5120, 5120 and 1760
  int16_t *pcm = malloc(N * 2);
  for (int i = 0; i < N; i++) pcm[i] = 900;
  qoa_desc d;
  memset(&d, 0, sizeof(d));
  d.channels = 1;
  d.samplerate = 22050;
  d.samples = N;
  unsigned len = 0;
  uint8_t *q = qoa_encode((const short *)pcm, &d, &len);
  CHECK(q != NULL);
  uint32_t full = 8 + 16 + 8 * 256;
  sdfake_put("/cut.qoa", (const char *)q, 8 + full + 1000);  // mid frame 2
  free(q);
  free(pcm);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/cut.qoa"));
  CHECK_EQ_INT(fileplayer_get_length(p), 5120);
  CHECK(fileplayer_play(p, 2));
  run(p, 44, 100000);
  CHECK(!fileplayer_is_playing(p));
  CHECK_EQ_INT(s_pushed, 2 * 5120);
  fileplayer_destroy(p);
}

// No tick decodes more than a bounded run of frames, skipped ones included:
// play() finds the ring empty, a seek skips up to a frame's worth.
static void test_qoa_tick_decode_is_bounded(void) {
  setup();
  put_qoa("/b.qoa", 66150, 1, 22050, 300);  // 3 s mono
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/b.qoa"));
  CHECK(fileplayer_play(p, 1));
  fileplayer_update();                      // an empty ring: 4096 free
  CHECK(s_pushed > 0 && s_pushed <= 1024);
  drain(RING_FRAMES);
  fileplayer_set_offset(p, 2);              // 3140 frames into frame 8
  uint64_t before = s_pushed;
  fileplayer_update();                      // all 1024 dropped: none pushed
  CHECK_EQ_INT((int)(s_pushed - before), 0);
  run(p, 44, 100000);
  CHECK_EQ_INT((int)(s_pushed - before), 66150 - 44100);
  fileplayer_destroy(p);
}

// A NaN rate (Lua setRate(0/0)) must not leave a player "playing" forever
// with nothing to push: it clamps like any rate below the minimum.
static void test_nan_rate_still_plays(void) {
  setup();
  put_wav("/n.wav", 2205, 1, 22050, 5);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/n.wav"));
  fileplayer_set_rate(p, 0.0f / 0.0f);
  CHECK(fileplayer_get_rate(p) >= 0.1f);
  CHECK(fileplayer_play(p, 1));
  run(p, 44, 100000);
  CHECK(!fileplayer_is_playing(p));
  fileplayer_destroy(p);
}

// QOA decodes a chunk at a time into the stack: loading and playing one
// takes nothing from the shared PSRAM heap, where a block left behind would
// split the space a later app may need in one piece.
size_t umm_fake_live(void);
static void test_qoa_needs_no_heap(void) {
  setup();
  size_t base = umm_fake_live();
  put_qoa("/a.qoa", 12000, 2, 22050, 7);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/a.qoa"));
  CHECK(fileplayer_play(p, 1));
  for (int i = 0; i < 5; i++) { fileplayer_update(); drain(44); }
  CHECK_EQ_INT((int)umm_fake_live(), (int)base);
  run(p, 44, 100000);
  CHECK_EQ_INT(s_pushed, 12000);
  CHECK_EQ_INT(s_dropped, 0);
  fileplayer_destroy(p);
  CHECK_EQ_INT((int)umm_fake_live(), (int)base);
}

int main(void) {
  test_flow_control_plays_every_frame();
  test_mono_and_rate_flow_control();
  test_full_ring_reads_nothing();
  test_sd_busy_skips_the_tick();
  test_players_keep_their_own_files();
  test_cross_core_stop_is_safe();
  test_qoa_plays_every_frame();
  test_qoa_stereo_at_44100();
  test_qoa_sd_busy_skips_the_tick();
  test_qoa_seek_and_loop();
  test_qoa_seek_is_sample_exact();
  test_qoa_needs_no_heap();
  test_qoa_cut_mid_frame_plays_what_is_there();
  test_qoa_tick_decode_is_bounded();
  test_nan_rate_still_plays();
  test_qoa_short_file_ends_at_its_frames();
  fileplayer_reset();
  sdfake_reset();
  return check_report("test_fileplayer");
}

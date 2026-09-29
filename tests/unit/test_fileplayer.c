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
static uint32_t s_underruns;  // frames the output found the ring empty
#define CAP_MAX 200000u
static int16_t s_cap[CAP_MAX];  // left sample of every frame pushed, in order
static uint32_t s_cap_n;

void audio_start_stream(uint32_t sample_rate) { s_starts++; s_rate = sample_rate; s_ring_used = 0; }
void audio_stop_stream(void) { s_stops++; }
uint32_t audio_ring_free(void) { return RING_FRAMES - s_ring_used; }
void audio_push_samples(const int16_t *samples, int count) {
  for (int i = 0; i < count; i++) {
    if (s_ring_used >= RING_FRAMES) { s_dropped++; continue; }
    s_ring_used++;
    s_pushed++;
    s_last_l = samples[i * 2];
    if (s_cap_n < CAP_MAX) s_cap[s_cap_n++] = s_last_l;
    if (s_arm_first) { s_first_l = s_last_l; s_arm_first = false; }
  }
}
void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns,
                        uint32_t *ring_used) {
  if (isr_count) *isr_count = 0;
  if (underruns) *underruns = s_underruns;
  if (ring_used) *ring_used = s_ring_used;
}
static void drain(uint32_t frames) {
  if (frames > s_ring_used) s_underruns += frames - s_ring_used;
  s_ring_used = frames > s_ring_used ? 0 : s_ring_used - frames;
}

static void ring_reset(void) {
  s_ring_used = 0; s_pushed = s_dropped = 0; s_starts = s_stops = 0;
  s_underruns = 0; s_cap_n = 0;
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

// A mono WAV of `seconds` whole seconds at `rate`; second i holds vals[i].
static void put_wav_steps(const char *path, uint32_t seconds, uint32_t rate,
                          const int16_t *vals) {
  uint32_t frames = seconds * rate, data = frames * 2;
  uint8_t *buf = calloc(1, 44 + data);
  memcpy(buf, "RIFF", 4); le32(buf + 4, 36 + data); memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4); le32(buf + 16, 16); le16(buf + 20, 1);
  le16(buf + 22, 1); le32(buf + 24, rate); le32(buf + 28, rate * 2);
  le16(buf + 32, 2); le16(buf + 34, 16);
  memcpy(buf + 36, "data", 4); le32(buf + 40, data);
  for (uint32_t i = 0; i < frames; i++) le16(buf + 44 + i * 2, (uint16_t)vals[i / rate]);
  sdfake_put(path, (const char *)buf, 44 + data);
  free(buf);
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

// ── #29 setLoopRange ────────────────────────────────────────────────────────
// Tick until `frames` frames have been pushed (the player loops for ever
// once a range is set), draining the ring like the DMA would.
static void run_until_pushed(fileplayer_t *p, uint32_t frames) {
  for (int t = 0; t < 200000 && s_cap_n < frames && fileplayer_is_playing(p); t++) {
    fileplayer_update();
    drain(44);
  }
}

// Steps 1000 / 5000 / 9000 for the 1st / 2nd / 3rd second of a 3 s mono
// 22050 Hz file, looped 1-2 s: the intro plays once, then 1-2 s for ever,
// and the 3rd second (9000) never plays. `tol` frames around each edge are
// not checked (QOA's lossy steps).
static void check_step_loop(uint32_t tol, uint32_t total) {
  CHECK(s_cap_n >= total);
  for (uint32_t i = 0; i < total; i++) {
    if (i < 22050 - tol) CHECK(s_cap[i] < 3000);                 // 0-1 s: intro
    else if (i >= 22050 + tol)                                   // 1-2 s, repeated
      CHECK(s_cap[i] > 3000 && s_cap[i] < 7000);
    CHECK(s_cap[i] < 7000);                                      // never 2-3 s
  }
}

static void test_wav_loop_range(void) {
  setup();
  const int16_t v[3] = {1000, 5000, 9000};
  put_wav_steps("/r.wav", 3, 22050, v);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/r.wav"));
  fileplayer_set_loop_range(p, 1, 2);
  CHECK(fileplayer_play(p, 1));
  run_until_pushed(p, 22050 * 4);
  CHECK(fileplayer_is_playing(p));
  check_step_loop(0, 22050 * 4);
  CHECK_EQ_INT(s_dropped, 0);
  fileplayer_destroy(p);
}

static void test_qoa_loop_range(void) {
  setup();
  enum { N = 66150 };
  int16_t *pcm = malloc(N * 2);
  for (int i = 0; i < N; i++) pcm[i] = i < 22050 ? 1000 : (i < 44100 ? 5000 : 9000);
  put_qoa_pcm("/r.qoa", pcm, N, 1, 22050);
  free(pcm);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/r.qoa"));
  fileplayer_set_loop_range(p, 1, 2);
  CHECK(fileplayer_play(p, 1));
  run_until_pushed(p, 22050 * 4);
  CHECK(fileplayer_is_playing(p));
  check_step_loop(400, 22050 * 4);
  fileplayer_destroy(p);
}

// An end of 0 is the end of the data: 2-3 s loops the last second, and the
// loop callback fires at each wrap.
static int s_loops;
static int count_loop(void *arg) { (void)arg; s_loops++; return 0; }

static void test_loop_range_to_end_of_data(void) {
  setup();
  const int16_t v[3] = {1000, 5000, 9000};
  put_wav_steps("/r.wav", 3, 22050, v);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/r.wav"));
  fileplayer_set_loop_callback(p, count_loop, NULL);
  s_loops = 0;
  fileplayer_set_loop_range(p, 2, 0);
  CHECK(fileplayer_play(p, 1));
  run_until_pushed(p, 22050 * 3 + 22050 * 2);
  CHECK_EQ_INT(s_loops, 2);
  for (uint32_t i = 44100; i < s_cap_n; i++) CHECK(s_cap[i] == 9000);
  fileplayer_destroy(p);
}

// No arguments loops the whole file; so do an end past the data and an
// empty (reversed) range.
static void test_loop_range_whole_file_cases(void) {
  const uint32_t args[3][2] = {{0, 0}, {0, 99}, {2, 1}};
  for (int c = 0; c < 3; c++) {
    setup();
    const int16_t v[2] = {1000, 5000};
    put_wav_steps("/r.wav", 2, 22050, v);
    fileplayer_t *p = fileplayer_create();
    CHECK(fileplayer_load(p, "/r.wav"));
    fileplayer_set_loop_range(p, args[c][0], args[c][1]);
    CHECK(fileplayer_play(p, 1));
    run_until_pushed(p, 44100 * 2);
    CHECK(fileplayer_is_playing(p));
    for (uint32_t i = 0; i < 44100 * 2; i++)
      CHECK_EQ_INT(s_cap[i], i % 44100 < 22050 ? 1000 : 5000);
    fileplayer_destroy(p);
  }
}

// The play(repeat) rule holds with a range: a pass that pushes nothing (an
// empty data chunk) ends the play instead of rewinding every tick.
static void test_loop_range_on_empty_file_finishes(void) {
  setup();
  put_wav("/e.wav", 0, 1, 22050, 0);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/e.wav"));
  fileplayer_set_loop_range(p, 1, 2);
  CHECK(fileplayer_play(p, 0));
  run(p, 44, 100);
  CHECK(!fileplayer_is_playing(p));
  fileplayer_destroy(p);
}

// ── #31 load() reasons ──────────────────────────────────────────────────────
static void test_load_reports_why(void) {
  setup();
  fileplayer_t *p = fileplayer_create();
  const char *why = NULL;
  CHECK(!fileplayer_load_err(p, "/missing.wav", &why));
  CHECK(why && strstr(why, "open"));

  sdfake_put("/a.mp3", "ID3\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16);
  why = NULL;
  CHECK(!fileplayer_load_err(p, "/a.mp3", &why));
  CHECK(why && strstr(why, "MP3"));

  sdfake_put("/junk.bin", "0123456789abcdef0123", 20);
  why = NULL;
  CHECK(!fileplayer_load_err(p, "/junk.bin", &why));
  CHECK(why && strstr(why, "format"));

  // 8-bit WAV: a Sample takes it, the streamer does not.
  put_wav("/b8.wav", 100, 1, 22050, 0);
  size_t n8 = 0;
  const char *g8 = sdfake_get("/b8.wav", &n8);
  CHECK(g8 != NULL);
  char *w8 = malloc(n8);
  memcpy(w8, g8, n8);
  w8[34] = 8;  // bits per sample
  sdfake_put("/b8.wav", w8, n8);
  free(w8);
  why = NULL;
  CHECK(!fileplayer_load_err(p, "/b8.wav", &why));
  CHECK(why && strstr(why, "16-bit"));

  // A QOA with 3 channels (the reference encoder writes them).
  put_qoa("/c3.qoa", 5000, 3, 22050, 100);
  why = NULL;
  CHECK(!fileplayer_load_err(p, "/c3.qoa", &why));
  CHECK(why != NULL);
  CHECK_EQ_INT(fileplayer_get_length(p), 0);
  CHECK(!fileplayer_play(p, 1));

  // Success: no reason, and a good file after bad ones plays.
  put_wav("/ok.wav", 100, 1, 22050, 7);
  why = "stale";
  CHECK(fileplayer_load_err(p, "/ok.wav", &why));
  CHECK(why == NULL);
  CHECK_EQ_INT(fileplayer_get_sample_rate(p), 22050);
  CHECK(fileplayer_play(p, 1));
  fileplayer_destroy(p);
}

// ── #33 underruns ───────────────────────────────────────────────────────────
static void test_did_underrun_tracks_the_stream(void) {
  setup();
  put_wav("/u.wav", 44100, 2, 22050, 3);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/u.wav"));
  CHECK(fileplayer_play(p, 1));
  // The ring is empty until the first push: not an underrun.
  s_underruns += 500;
  for (int i = 0; i < 20; i++) { fileplayer_update(); drain(44); }
  CHECK(!fileplayer_did_underrun(p));
  // The DMA outruns the producer: the ring runs dry.
  drain(RING_FRAMES);
  drain(100);
  fileplayer_update();
  CHECK(fileplayer_did_underrun(p));
  CHECK(!fileplayer_did_underrun(p));   // cleared by the read
  // Sticky until read: it survives ticks that found no new underrun.
  drain(RING_FRAMES);
  drain(100);
  fileplayer_update();
  for (int i = 0; i < 5; i++) { fileplayer_update(); drain(44); }
  CHECK(fileplayer_did_underrun(p));
  // A new play() starts clean.
  drain(RING_FRAMES); drain(10);
  fileplayer_update();
  CHECK(fileplayer_play(p, 1));
  CHECK(!fileplayer_did_underrun(p));
  // audiostat reset lowers the count: not an underrun.
  for (int i = 0; i < 5; i++) { fileplayer_update(); drain(44); }
  s_underruns = 0;
  fileplayer_update();
  CHECK(!fileplayer_did_underrun(p));
  fileplayer_destroy(p);
}

static void test_underrun_ignored_while_paused(void) {
  setup();
  put_wav("/u.wav", 44100, 2, 22050, 3);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/u.wav"));
  CHECK(fileplayer_play(p, 1));
  for (int i = 0; i < 10; i++) { fileplayer_update(); drain(44); }
  fileplayer_pause(p);
  drain(RING_FRAMES); drain(2000);       // the ring empties while paused
  fileplayer_resume(p);
  for (int i = 0; i < 10; i++) { fileplayer_update(); drain(44); }
  CHECK(!fileplayer_did_underrun(p));
  fileplayer_destroy(p);
}

static void test_stop_on_underrun(void) {
  for (int qoa = 0; qoa < 2; qoa++) {
    setup();
    if (qoa) put_qoa("/u.qoa", 44100, 1, 22050, 3);
    else put_wav("/u.wav", 44100, 2, 22050, 3);
    fileplayer_t *p = fileplayer_create();
    CHECK(fileplayer_load(p, qoa ? "/u.qoa" : "/u.wav"));
    fileplayer_set_stop_on_underrun(p, true);
    CHECK(fileplayer_play(p, 1));
    for (int i = 0; i < 20; i++) { fileplayer_update(); drain(44); }
    CHECK(fileplayer_is_playing(p));     // keeping up: keeps playing
    drain(RING_FRAMES); drain(100);
    int stops = s_stops;
    fileplayer_update();
    CHECK(!fileplayer_is_playing(p));
    CHECK_EQ_INT(s_stops, stops + 1);
    CHECK(fileplayer_did_underrun(p));   // and it says why
    fileplayer_destroy(p);
  }
  // Without the flag the same starvation plays on.
  setup();
  put_wav("/u.wav", 44100, 2, 22050, 3);
  fileplayer_t *p = fileplayer_create();
  CHECK(fileplayer_load(p, "/u.wav"));
  CHECK(fileplayer_play(p, 1));
  for (int i = 0; i < 20; i++) { fileplayer_update(); drain(44); }
  drain(RING_FRAMES); drain(100);
  fileplayer_update();
  CHECK(fileplayer_is_playing(p));
  CHECK(fileplayer_did_underrun(p));
  fileplayer_destroy(p);
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
  test_wav_loop_range();
  test_qoa_loop_range();
  test_loop_range_to_end_of_data();
  test_loop_range_whole_file_cases();
  test_loop_range_on_empty_file_finishes();
  test_load_reports_why();
  test_did_underrun_tracks_the_stream();
  test_underrun_ignored_while_paused();
  test_stop_on_underrun();
  fileplayer_reset();
  sdfake_reset();
  return check_report("test_fileplayer");
}

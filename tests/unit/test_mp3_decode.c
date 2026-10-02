// Host unit tests for what mp3_player.c's decoder writes and when it
// decodes (issue #28).
//
// - Mono: every sample reaches the mixer, in order (the decoder packs a
//   frame's samples and writes the ring once, not once per sample).
// - Decoding ahead: while Core 0 waits in a paced app's frame deadline
//   (os/core0_idle.h), the decoder fills the PCM ring past the half-full
//   mark it otherwise stops at, losing and repeating nothing. Without a
//   window, with decoding ahead switched off, and in fed mode (the video
//   player), it never does.
// - ID3v2 tags: a tagged file plays and loops exactly as the same file
//   untagged, with no decode error: play() and every loop start past the
//   tags (one in the decode buffer, one longer than it, two in a row).
//
// fixtures/mp3/chord22m.mp3: 1 s of test_mp3_fed's chord, 22.05 kHz mono
// MPEG-2 at 32 kbps (39 frames of 576 samples, no ID3 tag, no Xing frame),
//   ffmpeg -f lavfi -i "aevalsrc=<the chord>:s=22050:d=1" -ac 1
//     -c:a libmp3lame -b:a 32k -write_xing 0 -id3v2_version 0
//     -fflags +bitexact -flags:a +bitexact -f mp3 chord22m.mp3
// fixtures/mp3/chord128.mp3: test_mp3_refill's (44.1 kHz stereo, 116 frames).
#include "check.h"
#include "mp3_player.h"
#include "mp3_sched.h"
#include "audio.h"
#include "pio_psram.h"
#include "core0_idle.h"
#include "fakes/sdcard_fake.h"
#define FPM_DEFAULT  // as mp3_player.c
#include "mad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"

// ── What mp3_player.c needs from the rest of the firmware ───────────────────
static uint64_t s_now_us;  // the clock: moved only by the tests
void hal_sleep_ms(uint32_t ms) { (void)ms; }
void hal_sleep_us(uint64_t us) { (void)us; }
uint64_t hal_get_time_us(void) { return s_now_us; }
bool audio_output_running(void) { return false; }
void audio_output_ensure_running(void) {}
bool pio_psram_available(void) { return false; }
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
  (void)addr; (void)dst; (void)len; abort();
}
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
  (void)addr; (void)src; (void)len; abort();
}

typedef struct {
  uint8_t *data;
  uint32_t len;
} blob_t;

static blob_t load_fixture(const char *name) {
  blob_t b = {0};
  char path[512];
  snprintf(path, sizeof path, FIX "%s", name);
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (!f) exit(1);
  fseek(f, 0, SEEK_END);
  b.len = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  b.data = malloc(b.len);
  CHECK(fread(b.data, 1, b.len, f) == b.len);
  fclose(f);
  return b;
}

static void put_file(const char *path, blob_t b) {
  sdfake_reset();
  sdfake_put(path, (const char *)b.data, b.len);
}

static uint32_t diag(int i) {
  uint32_t d[11];
  mp3_player_get_diag(d);
  return d[i];
}
#define DIAG_FRAMES 6

static mp3_sched_stats_t sched(void) {
  mp3_sched_stats_t s;
  mp3_player_get_sched_stats(&s);
  return s;
}

// libmad straight over the whole file: channel 0 of every granule.
static uint32_t reference_decode(blob_t b, int16_t *out, uint32_t cap) {
  uint8_t *buf = calloc(1, b.len + MAD_BUFFER_GUARD);
  memcpy(buf, b.data, b.len);
  struct mad_stream *st = malloc(sizeof *st);
  struct mad_frame *fr = malloc(sizeof *fr);
  struct mad_frame_mem *fm = malloc(sizeof *fm);
  struct mad_synth *sy = malloc(sizeof *sy);
  mad_stream_init(st);
  mad_frame_bind(fr, fm);
  mad_frame_init(fr);
  mad_synth_init(sy);
  mad_stream_buffer(st, buf, b.len + MAD_BUFFER_GUARD);
  uint32_t n = 0;
  for (;;) {
    if (mad_frame_decode_begin(fr, st) != 0) {
      if (MAD_RECOVERABLE(st->error)) continue;
      break;
    }
    for (unsigned gr = 0; gr < mad_frame_granules(fr); gr++) {
      if (mad_frame_decode_granule(fr, st, gr) != 0) break;
      mad_synth_granule(sy, fr, gr);
      for (unsigned i = 0; i < sy->pcm.length && n < cap; i++)
        out[n++] = sy->pcm.samplesX[i][0];
    }
    mad_frame_decode_end(fr, st);
  }
  free(st); free(fr); free(fm); free(sy); free(buf);
  return n;
}

// A mono file: the mixer hears every sample of every frame, in order. At
// the mixer's 44.1 kHz each 22.05 kHz sample plays twice, on both sides;
// the first 64 output frames are the fade-in.
static void test_mono_plays_every_sample_in_order(void) {
  blob_t b = load_fixture("chord22m.mp3");
  static int16_t want[64 * 576];
  uint32_t nwant = reference_decode(b, want, 64 * 576);
  CHECK(nwant >= 36 * 576);

  put_file("/mono.mp3", b);
  mp3_player_t *p = mp3_player_create();
  CHECK(mp3_player_load(p, "/mono.mp3"));
  CHECK_EQ_U32(mp3_player_get_sample_rate(p), 22050);
  CHECK(mp3_player_play(p, 1));
  static int32_t l[128], r[128];
  uint32_t out = 0, bad = 0, first_bad = 0;
  for (int i = 0; i < 2000 && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    mp3_player_mix(l, r, 128);
    for (int k = 0; k < 128; k++, out++) {
      if (out < 64 || out / 2 >= nwant) continue;
      if (l[k] != want[out / 2] || r[k] != want[out / 2]) {
        if (!bad) first_bad = out;
        bad++;
      }
    }
  }
  if (bad)
    printf("  mono: %u output frames differ, the first at %u\n", bad, first_bad);
  CHECK_EQ_U32(bad, 0);
  CHECK(out / 2 >= nwant);
  CHECK(!mp3_player_is_playing(p));
  mp3_player_stop(p);
  free(b.data);
}

// Frames decoded by `updates` Core 1 updates after play(), with nothing
// mixed meanwhile (so the ring only fills).
static uint32_t frames_after(int updates) {
  uint32_t before = diag(DIAG_FRAMES);
  for (int i = 0; i < updates; i++)
    mp3_player_update();
  return diag(DIAG_FRAMES) - before;
}

static void start_music(void) {
  mp3_player_t *p = mp3_player_create();
  CHECK(mp3_player_load(p, "/music.mp3"));
  CHECK(mp3_player_play(p, 1));
  mp3_player_reset_sched_stats();
}

static void test_decoding_ahead_needs_core0_idle(void) {
  blob_t b = load_fixture("chord128.mp3");
  put_file("/music.mp3", b);
  mp3_player_t *p = mp3_player_create();

  // Core 0 working: the ring stops filling past half.
  s_now_us = 1000000;
  start_music();
  uint32_t working = frames_after(20);
  CHECK_EQ_U32(sched().idle_frames, 0);
  mp3_player_stop(p);

  // Core 0 idle for the next 20 ms: the decoder fills the ring.
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 20000);
  uint32_t idle = frames_after(20);
  core0_idle_end(s_now_us);
  printf("  frames decoded ahead: %u with Core 0 working, %u idle\n", working, idle);
  CHECK(idle > working);
  CHECK(sched().idle_frames > 0);
  // (The decoder's unit is a granule: two in each of these frames.)
  CHECK_EQ_U32(sched().idle_frames, 2 * (idle - working));
  mp3_player_stop(p);

  // A window shorter than a frame's estimate starts nothing.
  start_music();
  core0_idle_begin(s_now_us, s_now_us + MP3_SCHED_SEED_US - 1);
  CHECK_EQ_U32(frames_after(20), working);
  core0_idle_end(s_now_us);
  mp3_player_stop(p);

  // Switched off (xipstat mp3idle off): as if Core 0 never idled.
  mp3_player_set_decode_ahead(false);
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 20000);
  CHECK_EQ_U32(frames_after(20), working);
  CHECK_EQ_U32(sched().idle_frames, 0);
  core0_idle_end(s_now_us);
  mp3_player_stop(p);
  mp3_player_set_decode_ahead(true);
  free(b.data);
}

// Decoding ahead the whole way through loses and repeats nothing.
static void test_decoding_ahead_loses_no_frame(void) {
  blob_t b = load_fixture("chord128.mp3");
  put_file("/music.mp3", b);
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 1000000);
  static int32_t l[128], r[128];
  uint32_t furthest = 0;
  for (int i = 0; i < 5000 && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    mp3_player_mix(l, r, 128);
    uint32_t pos = mp3_player_get_position(p);
    if (pos > furthest) furthest = pos;
  }
  core0_idle_end(s_now_us);
  CHECK_EQ_U32(furthest, 116u * 1152u);
  CHECK_EQ_U32(diag(DIAG_FRAMES), 116);
  CHECK(sched().idle_frames > 0);
  CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
  mp3_player_stop(p);
  free(b.data);
}

// The video player's fed session decodes as before, window or not.
static void test_fed_mode_never_decodes_ahead(void) {
  blob_t b = load_fixture("chord128.mp3");
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(b.data, b.len), b.len);
  mp3_player_start_fed_output();
  mp3_player_reset_sched_stats();
  core0_idle_begin(s_now_us, s_now_us + 1000000);
  for (int i = 0; i < 20; i++)
    mp3_player_update();
  core0_idle_end(s_now_us);
  CHECK_EQ_U32(sched().idle_frames, 0);
  mp3_player_stop_fed();
  free(b.data);
}

// An ID3v2.3 tag of `body` bytes whose contents look like MP3 frame
// headers (0xFF 0xF3 ... as in cover art), before the file's audio.
static blob_t tagged(blob_t audio, uint32_t body, int tags) {
  blob_t b;
  b.len = audio.len + (uint32_t)tags * (10u + body);
  b.data = malloc(b.len);
  uint8_t *p = b.data;
  for (int t = 0; t < tags; t++) {
    p[0] = 'I'; p[1] = 'D'; p[2] = '3'; p[3] = 3; p[4] = 0; p[5] = 0;
    p[6] = (uint8_t)((body >> 21) & 0x7f); p[7] = (uint8_t)((body >> 14) & 0x7f);
    p[8] = (uint8_t)((body >> 7) & 0x7f);  p[9] = (uint8_t)(body & 0x7f);
    for (uint32_t i = 0; i < body; i++)
      p[10 + i] = (i % 7 == 0) ? 0xFF : (i % 7 == 1) ? 0xF3 : (uint8_t)(i * 31u);
    p += 10 + body;
  }
  memcpy(p, audio.data, audio.len);
  return b;
}

// Plays `path` looping for `renders` mixer renders; the output and the
// decode errors (mp3stats' mad_err).
static uint32_t play_looped(const char *path, int32_t *out, int renders) {
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  CHECK(mp3_player_load(p, path));
  mp3_player_set_loop(p, true);
  CHECK(mp3_player_play(p, 0));
  static int32_t l[128], r[128];
  for (int i = 0; i < renders; i++) {
    mp3_player_update();
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    mp3_player_mix(l, r, 128);
    memcpy(out + i * 128, l, sizeof l);
  }
  CHECK(mp3_player_is_playing(p));
  mp3_player_stop(p);
  return diag(7);
}

static void test_id3v2_tags_are_skipped_at_play_and_at_every_loop(void) {
  blob_t b = load_fixture("chord22m.mp3");   // ~1.07 s: three loops below
  enum { RENDERS = 1100 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  put_file("/plain.mp3", b);
  uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
  CHECK(diag(DIAG_FRAMES) > 3 * 39);
  static const struct { uint32_t body; int tags; } cases[] = {
    {4534, 1},    // in the 8 KB decode buffer (Nova Rail's: 4.5 KB)
    {20000, 1},   // longer than it: read again from its end
    {300, 2},     // two in a row
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    blob_t t = tagged(b, cases[c].body, cases[c].tags);
    put_file("/tagged.mp3", t);
    uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
    CHECK_EQ_U32(terrs, errs);
    CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < (size_t)RENDERS * 128; i++)
      if (got[i] != want[i]) { first = i; break; }
    if (first != SIZE_MAX)
      printf("  tag %u x%d: output differs from frame %zu\n",
             (unsigned)cases[c].body, cases[c].tags, first);
    CHECK(first == SIZE_MAX);
    free(t.data);
  }
  free(b.data);
}

int main(void) {
  CHECK(mp3_player_init());
  test_mono_plays_every_sample_in_order();
  test_decoding_ahead_needs_core0_idle();
  test_decoding_ahead_loses_no_frame();
  test_fed_mode_never_decodes_ahead();
  test_id3v2_tags_are_skipped_at_play_and_at_every_loop();
  mp3_player_deinit();
  return check_report("test_mp3_decode");
}
